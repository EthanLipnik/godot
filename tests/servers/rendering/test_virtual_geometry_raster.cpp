/**************************************************************************/
/*  test_virtual_geometry_raster.cpp                                      */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_geometry_raster)

#include "servers/rendering/virtual_geometry/virtual_geometry_raster.h"
#include "core/io/file_access.h"

namespace TestVirtualGeometryRaster {

using namespace RendererVirtualGeometry;

static Package _package() {
	Package package;
	package.manifest.root_group_ids.push_back(100);
	for (uint64_t id : { uint64_t(1), uint64_t(2), uint64_t(3) }) {
		ClusterDescriptor cluster;
		cluster.stable_id = id;
		cluster.page_id = id;
		cluster.vertex_count = 3;
		cluster.triangle_count = id == 1 ? 1 : 2;
		cluster.material_slot = id == 3 ? 1 : 0;
		cluster.bounds = AABB(Vector3(-1, -1, -2), Vector3(2, 2, 1));
		cluster.sphere_center = cluster.bounds.get_center();
		cluster.sphere_radius = 2.0;
		package.manifest.clusters.push_back(cluster);
		PageDescriptor page;
		page.stable_id = id;
		page.persistent = id == 1;
		page.cluster_ids.push_back(id);
		package.manifest.pages.push_back(page);
	}
	RefinementGroupDescriptor root;
	root.stable_id = 100;
	root.persistent_root = true;
	root.coarse_cluster_ids.push_back(1);
	root.fine_cluster_ids.push_back(2);
	root.fine_cluster_ids.push_back(3);
	root.bounds = AABB(Vector3(-1, -1, -2), Vector3(2, 2, 1));
	root.geometric_error = 1.0;
	package.manifest.groups.push_back(root);
	return package;
}

static VirtualGeometryRasterSelectionInput _input() {
	VirtualGeometryRasterSelectionInput input;
	input.command_capacity = 4;
	VirtualGeometryRasterView left;
	left.camera_position = Vector3(0, 0, 0);
	left.history_valid = true;
	left.occlusion_valid = true;
	left.projected_error_threshold_pixels = 0.1;
	input.views.push_back(left);
	VirtualGeometryRasterView right = left;
	right.camera_position.x = 0.064;
	input.views.push_back(right);
	return input;
}

TEST_CASE("[Rendering][VirtualGeometry] VG3 selection emits bounded indexed indirect commands") {
	HashSet<uint64_t> active;
	active.insert(1);
	active.insert(2);
	active.insert(3);
	VirtualGeometryRasterSelection result = VirtualGeometryRasterSelector::select(_package(), active, _input());
	CHECK(result.diagnostics.compute_indirect);
	CHECK_FALSE(result.diagnostics.native_mesh_shader);
	CHECK_EQ(result.diagnostics.backend_path, "compute_indirect");
	CHECK_EQ(result.cluster_ids.size(), 2);
	CHECK_EQ(result.commands.size(), 4);
	CHECK_EQ(result.commands[0].index_count, 6);
	CHECK_EQ(result.commands[0].instance_count, 1);
	CHECK_EQ(result.commands[0].first_instance, 0);
	CHECK_EQ(result.commands[2].index_count, 0); // fixed-capacity no-op command.
}

TEST_CASE("[Rendering][VirtualGeometry] VG3 preserves per-eye occlusion and fails open on a cut") {
	HashSet<uint64_t> active;
	active.insert(1);
	active.insert(2);
	active.insert(3);
	VirtualGeometryRasterSelectionInput input = _input();
	input.views.write[0].occluded_cluster_ids.insert(2);
	VirtualGeometryRasterSelection split = VirtualGeometryRasterSelector::select(_package(), active, input);
	CHECK(split.diagnostics.per_eye_disagreement > 0);
	CHECK(split.diagnostics.occlusion_rejected_clusters > 0);

	input.views.write[0].camera_cut = true;
	VirtualGeometryRasterSelection cut = VirtualGeometryRasterSelector::select(_package(), active, input);
	CHECK(cut.diagnostics.fail_open_occlusion);
	CHECK_EQ(cut.eye_visibility_masks[0] & 1u, 1u);
}

TEST_CASE("[Rendering][VirtualGeometry] VG3 falls back atomically when a fine cluster is absent") {
	HashSet<uint64_t> active;
	active.insert(1);
	active.insert(2);
	VirtualGeometryRasterSelection result = VirtualGeometryRasterSelector::select(_package(), active, _input());
	CHECK_EQ(result.cluster_ids.size(), 1);
	CHECK_EQ(result.cluster_ids[0], 1);
	CHECK_EQ(result.diagnostics.fallback_clusters, 1);
	REQUIRE_EQ(result.requested_page_ids.size(), 2);
	CHECK_EQ(result.requested_page_ids[0], 2);
	CHECK_EQ(result.requested_page_ids[1], 3);
	CHECK(result.requested_page_priorities[0] > 0);
}

TEST_CASE("[Rendering][VirtualGeometry] F6 demand uses stereo-union hysteresis and deterministic page priorities") {
	const Package package = _package();
	HashSet<uint64_t> coarse;
	coarse.insert(1);
	VirtualGeometryRasterSelectionInput input = _input();
	input.refine_threshold_pixels = 10.0;
	input.coarsen_threshold_pixels = 5.0;
	VirtualGeometryRasterSelectionState state;
	const VirtualGeometryRasterSelection demand = VirtualGeometryRasterSelector::select(package, coarse, input, &state);
	REQUIRE_EQ(demand.requested_page_ids.size(), 2);
	CHECK_EQ(demand.requested_page_ids[0], 2);
	CHECK_EQ(demand.requested_page_ids[1], 3);
	CHECK_EQ(demand.requested_page_priorities[0], demand.requested_page_priorities[1]);

	HashSet<uint64_t> all;
	all.insert(1);
	all.insert(2);
	all.insert(3);
	input.views.write[0].camera_position.z = 100.0;
	input.views.write[1].camera_position.z = 100.0;
	const VirtualGeometryRasterSelection retained = VirtualGeometryRasterSelector::select(package, all, input, &state);
	CHECK_EQ(retained.cluster_ids.size(), 2);
	VirtualGeometryRasterSelectionState fresh_state;
	const VirtualGeometryRasterSelection fresh = VirtualGeometryRasterSelector::select(package, all, input, &fresh_state);
	REQUIRE_EQ(fresh.cluster_ids.size(), 1);
	CHECK_EQ(fresh.cluster_ids[0], 1);
	input.views.write[0].camera_position.z = 200.0;
	input.views.write[1].camera_position.z = 200.0;
	const VirtualGeometryRasterSelection coarsened = VirtualGeometryRasterSelector::select(package, all, input, &state);
	REQUIRE_EQ(coarsened.cluster_ids.size(), 1);
	CHECK_EQ(coarsened.cluster_ids[0], 1);
}

TEST_CASE("[Rendering][VirtualGeometry] F6 rotated stereo frusta form a conservative eye union") {
	HashSet<uint64_t> active;
	active.insert(1);
	active.insert(2);
	active.insert(3);
	VirtualGeometryRasterSelectionInput input = _input();
	input.views.write[0].frustum_planes.push_back(Plane(Vector3(1, 0, 0), -100.0));
	input.views.write[0].view_direction = Vector3(1, 0, 0);
	input.views.write[1].view_direction = Vector3(-1, 0, 0);
	const VirtualGeometryRasterSelection result = VirtualGeometryRasterSelector::select(_package(), active, input);
	REQUIRE_EQ(result.cluster_ids.size(), 2);
	for (uint32_t mask : result.eye_visibility_masks) CHECK_EQ(mask, 2u);
	CHECK(result.diagnostics.per_eye_disagreement > 0);
}

TEST_CASE("[Rendering][VirtualGeometry] VG3 selection preserves material partitions for indirect batching") {
	HashSet<uint64_t> active;
	active.insert(1);
	active.insert(2);
	active.insert(3);
	const Package package = _package();
	const VirtualGeometryRasterSelection selection = VirtualGeometryRasterSelector::select(package, active, _input());
	HashSet<uint32_t> selected_materials;
	for (uint64_t selected_cluster : selection.cluster_ids) {
		for (const ClusterDescriptor &cluster : package.manifest.clusters) {
			if (cluster.stable_id == selected_cluster) {
				selected_materials.insert(cluster.material_slot);
				break;
			}
		}
	}
	CHECK_EQ(selected_materials.size(), 2);
}

TEST_CASE("[Rendering][VirtualGeometry] VG3 Flux submission batches authored materials and rejects unsupported variants") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("HashMap<uint32_t, Vector<VirtualGeometryCandidateGPU>> by_material"));
	CHECK(text.contains("material->shader_data->uses_alpha_pass()"));
	CHECK(text.contains("material->shader_data->uses_vertex"));
	CHECK(text.contains("Do not\n\t\t\t\t// substitute Flux's default material"));
	CHECK(text.contains("rd->barrier(RD::BARRIER_MASK_COMPUTE, RD::BARRIER_MASK_RASTER)"));
	CHECK(text.contains("rd->draw_list_draw_indirect"));
	CHECK(text.contains("case PASS_MODE_DEPTH:"));
	CHECK(text.contains("PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS"));
	CHECK(text.contains("PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI"));
	CHECK(text.contains("PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_HYBRID_MATERIAL"));
	CHECK(text.contains("expected_commands"));
	CHECK(text.contains("first_instance_matches"));
	CHECK(text.contains("last_readback_serial"));
	CHECK(text.contains("get_pipeline(key, key.hash(), true, RSE::PIPELINE_SOURCE_DRAW)"));
	CHECK(text.contains("virtual_geometry_raster.instance_data_base + count, true"));
}

TEST_CASE("[Rendering][VirtualGeometry] F6 Flux maps a pure VG instance buffer before append") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	const int append = text.find("void RenderFlux::_append_virtual_geometry_instance_data");
	REQUIRE(append >= 0);
	const int prepare = text.find("void RenderFlux::_prepare_virtual_geometry", append);
	REQUIRE(prepare > append);
	const String body = text.substr(append, prepare - append);
	// A pure virtual-geometry viewport has no conventional opaque element, so
	// _fill_instance_data() does not map its UMA buffer. Keep the explicit map
	// after the optional conventional refill: a 5-resource/15-instance scene
	// otherwise dereferences a null destination on its first submission.
	CHECK(body.contains("if (virtual_geometry_raster.instance_data_base > 0)"));
	CHECK(body.contains("scene_state.instance_buffer[RENDER_LIST_OPAQUE].map_raw_for_upload(0u)"));
	CHECK(body.contains("virtual_geometry_raster.failed_submissions += count"));
	CHECK(body.find("map_raw_for_upload(0u)") < body.find("SceneState::InstanceData *dst"));
	CHECK(body.contains("data.flags = uint32_t(255) << INSTANCE_DATA_FLAGS_FADE_SHIFT"));
}

TEST_CASE("[Rendering][VirtualGeometry] Flux primary surface preserves unshaded authored albedo") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/shaders/scene_flux.glsl", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	const int primary_surface = text.find("#ifdef MODE_FLUX_PRIMARY_SURFACE");
	REQUIRE(primary_surface >= 0);
	const int ordinary_color = text.find("#else\n\t// multiply by albedo", primary_surface);
	REQUIRE(ordinary_color > primary_surface);
	const String primary_surface_body = text.substr(primary_surface, ordinary_color - primary_surface);
	CHECK(primary_surface_body.contains("#ifdef MODE_UNSHADED"));
	CHECK(primary_surface_body.contains("frag_color = vec4(albedo, clamp(ao, 0.0, 1.0));"));
	CHECK(primary_surface_body.contains("frag_color = vec4(emission, clamp(ao, 0.0, 1.0));"));
}

} // namespace TestVirtualGeometryRaster
