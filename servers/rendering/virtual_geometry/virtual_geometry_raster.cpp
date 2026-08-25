/**************************************************************************/
/*  virtual_geometry_raster.cpp                                           */
/**************************************************************************/

#include "virtual_geometry_raster.h"

#include "core/error/error_macros.h"

namespace RendererVirtualGeometry {

namespace {

static const ClusterDescriptor *_find_cluster(const Package &p_package, uint64_t p_id) {
	for (const ClusterDescriptor &cluster : p_package.manifest.clusters) {
		if (cluster.stable_id == p_id) {
			return &cluster;
		}
	}
	return nullptr;
}

static const RefinementGroupDescriptor *_find_group(const Package &p_package, uint64_t p_id) {
	for (const RefinementGroupDescriptor &group : p_package.manifest.groups) {
		if (group.stable_id == p_id) {
			return &group;
		}
	}
	return nullptr;
}

static const PageDescriptor *_find_page(const Package &p_package, uint64_t p_id) {
	for (const PageDescriptor &page : p_package.manifest.pages) {
		if (page.stable_id == p_id) {
			return &page;
		}
	}
	return nullptr;
}

static bool _cluster_in_frustum(const ClusterDescriptor &p_cluster, const VirtualGeometryRasterView &p_view) {
	for (const Plane &plane : p_view.frustum_planes) {
		if (plane.distance_to(p_cluster.sphere_center) >= p_cluster.sphere_radius) {
			return false;
		}
	}
	return true;
}

static bool _bounds_in_frustum(const AABB &p_bounds, const VirtualGeometryRasterView &p_view) {
	for (const Plane &plane : p_view.frustum_planes) {
		if (plane.distance_to(p_bounds.get_support(-plane.normal)) >= 0.0) return false;
	}
	return true;
}

static bool _cluster_cone_visible(const ClusterDescriptor &p_cluster, const VirtualGeometryRasterView &p_view) {
	// VG1 does not serialize conservative normal cones. Do not infer one from
	// vertex normals: an invalid cone must fail open, especially for double-sided
	// materials and mirrored transforms.
	(void)p_cluster;
	(void)p_view;
	return true;
}

static real_t _projected_error(const RefinementGroupDescriptor &p_group, const VirtualGeometryRasterView &p_view) {
	const real_t distance = MAX(real_t(0.001), p_view.camera_position.distance_to(p_group.bounds.get_center()) - p_group.bounds.size.length() * 0.5);
	const real_t half_fov = MAX(real_t(0.001), p_view.vertical_fov_radians * 0.5);
	const real_t pixels_per_meter = real_t(p_view.viewport_height) * 0.5 / Math::tan(half_fov);
	return p_group.geometric_error * pixels_per_meter / distance;
}

static bool _group_fine_active(const RefinementGroupDescriptor &p_group, const HashSet<uint64_t> &p_active_clusters) {
	for (uint64_t id : p_group.fine_cluster_ids) {
		if (!p_active_clusters.has(id)) {
			return false;
		}
	}
	return true;
}

static void _append_cluster(uint64_t p_cluster_id, bool p_fallback, const Package &p_package, const HashSet<uint64_t> &p_active_clusters, const VirtualGeometryRasterSelectionInput &p_input, VirtualGeometryRasterSelection &r_selection) {
	const ClusterDescriptor *cluster = _find_cluster(p_package, p_cluster_id);
	if (!cluster || !p_active_clusters.has(p_cluster_id)) {
		return;
	}
	uint32_t visibility_mask = 0;
	for (int eye = 0; eye < p_input.views.size(); eye++) {
		const VirtualGeometryRasterView &view = p_input.views[eye];
		if (!_cluster_in_frustum(*cluster, view)) {
			r_selection.diagnostics.frustum_rejected_clusters++;
			continue;
		}
		if (!_cluster_cone_visible(*cluster, view)) {
			r_selection.diagnostics.cone_rejected_clusters++;
			continue;
		}
		const bool fail_open = view.camera_cut || view.near_plane_uncertain || !view.history_valid || view.stereo_boundary_uncertain || !view.occlusion_valid;
		if (!fail_open && view.occluded_cluster_ids.has(p_cluster_id)) {
			r_selection.diagnostics.occlusion_rejected_clusters++;
			continue;
		}
		visibility_mask |= 1u << eye;
		r_selection.diagnostics.per_eye_selected[MIN(eye, 1)]++;
		r_selection.diagnostics.fail_open_occlusion |= fail_open;
	}
	if (visibility_mask == 0) {
		return;
	}
	if (r_selection.commands.size() >= int(p_input.command_capacity)) {
		r_selection.diagnostics.overflow_clusters++;
		return;
	}
	VirtualGeometryIndexedIndirectCommand command;
	command.index_count = cluster->triangle_count * 3;
	command.instance_count = 1;
	// The GPU page-table lookup consumes first_instance as the stable selected
	// record index. It resolves page-local decoded stream offsets rather than
	// introducing a page RID or a node per cluster.
	command.first_instance = r_selection.cluster_ids.size();
	r_selection.cluster_ids.push_back(p_cluster_id);
	r_selection.eye_visibility_masks.push_back(visibility_mask);
	r_selection.commands.push_back(command);
	r_selection.diagnostics.selected_clusters++;
	if (p_fallback) {
		r_selection.diagnostics.fallback_clusters++;
	}
}

static void _request_missing_fine_pages(const RefinementGroupDescriptor &p_group, const Package &p_package, const HashSet<uint64_t> &p_active_clusters, real_t p_projected_error, VirtualGeometryRasterSelection &r_selection) {
	(void)p_active_clusters;
	HashSet<uint64_t> already_requested;
	for (uint64_t page_id : r_selection.requested_page_ids) {
		already_requested.insert(page_id);
	}
	for (uint64_t cluster_id : p_group.fine_cluster_ids) {
		const ClusterDescriptor *cluster = _find_cluster(p_package, cluster_id);
		if (!cluster || already_requested.has(cluster->page_id) || !_find_page(p_package, cluster->page_id)) {
			continue;
		}
		already_requested.insert(cluster->page_id);
		r_selection.requested_page_ids.push_back(cluster->page_id);
		const double scaled_error = CLAMP(double(p_projected_error) * 65536.0, 0.0, double(UINT32_MAX - 1));
		r_selection.requested_page_priorities.push_back(uint32_t(scaled_error) + 1u);
	}
}

static void _select_group(uint64_t p_group_id, const Package &p_package, const HashSet<uint64_t> &p_active_clusters, const VirtualGeometryRasterSelectionInput &p_input, VirtualGeometryRasterSelectionState *p_state, HashSet<uint64_t> &r_visiting, VirtualGeometryRasterSelection &r_selection) {
	const RefinementGroupDescriptor *group = _find_group(p_package, p_group_id);
	if (!group || r_visiting.has(p_group_id)) {
		return;
	}
	r_visiting.insert(p_group_id);
	r_selection.diagnostics.candidate_groups++;
	bool visible_in_union = false;
	for (const VirtualGeometryRasterView &view : p_input.views) visible_in_union |= _bounds_in_frustum(group->bounds, view);
	if (!visible_in_union) {
		r_visiting.erase(p_group_id);
		return;
	}

	real_t maximum_projected_error = 0.0;
	for (const VirtualGeometryRasterView &view : p_input.views) {
		maximum_projected_error = MAX(maximum_projected_error, _projected_error(*group, view));
	}
	const bool was_refined = p_state && p_state->refined_group_ids.has(group->stable_id);
	const real_t refine_threshold = MAX(real_t(0.0001), p_input.refine_threshold_pixels);
	const real_t coarsen_threshold = CLAMP(p_input.coarsen_threshold_pixels, real_t(0.0), refine_threshold);
	const bool wants_refinement = was_refined ? maximum_projected_error >= coarsen_threshold : maximum_projected_error > refine_threshold;
	if (p_state) {
		if (wants_refinement) {
			p_state->refined_group_ids.insert(group->stable_id);
		} else {
			p_state->refined_group_ids.erase(group->stable_id);
		}
	}
	if (wants_refinement) {
		_request_missing_fine_pages(*group, p_package, p_active_clusters, maximum_projected_error, r_selection);
	}
	const bool should_refine = wants_refinement && _group_fine_active(*group, p_active_clusters);
	if (!should_refine) {
		for (uint64_t cluster_id : group->coarse_cluster_ids) {
			_append_cluster(cluster_id, true, p_package, p_active_clusters, p_input, r_selection);
		}
		r_visiting.erase(p_group_id);
		return;
	}

	HashSet<uint64_t> child_coarse_clusters;
	for (uint64_t child_id : group->child_group_ids) {
		const RefinementGroupDescriptor *child = _find_group(p_package, child_id);
		if (child) {
			for (uint64_t cluster_id : child->coarse_cluster_ids) {
				child_coarse_clusters.insert(cluster_id);
			}
				_select_group(child_id, p_package, p_active_clusters, p_input, p_state, r_visiting, r_selection);
		}
	}
	for (uint64_t cluster_id : group->fine_cluster_ids) {
		if (!child_coarse_clusters.has(cluster_id)) {
			_append_cluster(cluster_id, false, p_package, p_active_clusters, p_input, r_selection);
		}
	}
	r_visiting.erase(p_group_id);
}

} // namespace

VirtualGeometryRasterSelection VirtualGeometryRasterSelector::select(const Package &p_package, const HashSet<uint64_t> &p_active_clusters, const VirtualGeometryRasterSelectionInput &p_input, VirtualGeometryRasterSelectionState *p_state) {
	VirtualGeometryRasterSelection result;
	result.diagnostics.compute_indirect = p_input.backend_compute_indirect_available;
	result.diagnostics.native_mesh_shader = false;
	result.diagnostics.backend_path = p_input.backend_compute_indirect_available ? "compute_indirect" : "unavailable";
	if (p_input.command_capacity == 0 || p_input.views.is_empty() || !p_input.backend_compute_indirect_available) {
		return result;
	}
	HashSet<uint64_t> visiting;
	for (uint64_t root_id : p_package.manifest.root_group_ids) {
		_select_group(root_id, p_package, p_active_clusters, p_input, p_state, visiting, result);
	}
	for (uint32_t mask : result.eye_visibility_masks) {
		if ((mask & (mask - 1)) == 0 && p_input.views.size() > 1) {
			result.diagnostics.per_eye_disagreement++;
		}
	}
	// Preserve a fixed command-buffer extent so callers can use one bounded
	// multi-draw call. Zero-count records are specified no-op commands.
	result.commands.resize(p_input.command_capacity);
	return result;
}

} // namespace RendererVirtualGeometry
