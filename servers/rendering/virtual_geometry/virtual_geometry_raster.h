/**************************************************************************/
/*  virtual_geometry_raster.h                                             */
/**************************************************************************/

#pragma once

#include "servers/rendering/virtual_geometry/virtual_geometry_format.h"

#include "core/math/math_funcs.h"
#include "core/math/plane.h"

namespace RendererVirtualGeometry {

// Matches DrawIndexedIndirectCommand on Metal/Vulkan. The selector writes a
// fixed-capacity array of these records; commands beyond selected_count have
// index_count == 0 and are intentionally valid no-op draws.
struct VirtualGeometryIndexedIndirectCommand {
	uint32_t index_count = 0;
	uint32_t instance_count = 0;
	uint32_t first_index = 0;
	int32_t vertex_offset = 0;
	uint32_t first_instance = 0;
};
static_assert(sizeof(VirtualGeometryIndexedIndirectCommand) == 20);

struct VirtualGeometryRasterView {
	Vector<Plane> frustum_planes;
	Vector3 camera_position;
	Vector3 view_direction = Vector3(0, 0, -1);
	uint32_t viewport_height = 1080;
	real_t vertical_fov_radians = Math::deg_to_rad(real_t(70.0));
	real_t projected_error_threshold_pixels = 1.0;
	bool camera_cut = false;
	bool near_plane_uncertain = false;
	bool history_valid = false;
	bool stereo_boundary_uncertain = false;
	bool occlusion_valid = false;
	HashSet<uint64_t> occluded_cluster_ids;
};

struct VirtualGeometryRasterSelectionInput {
	Vector<VirtualGeometryRasterView> views;
	uint32_t command_capacity = 0;
	// Hysteresis is stateful per scene instance. A group refines above the high
	// threshold and remains refined until every active eye falls below the low
	// threshold. The defaults retain the historical one-pixel policy.
	real_t refine_threshold_pixels = 1.0;
	real_t coarsen_threshold_pixels = 0.75;
	// Native mesh/object shading remains a future capability. The reference
	// path is intentionally selected even on Metal until a real mesh pipeline
	// and its output-limit validation exist.
	bool backend_compute_indirect_available = true;
	bool backend_native_mesh_shader_available = false;
};

struct VirtualGeometryRasterSelectionState {
	HashSet<uint64_t> refined_group_ids;
};

struct VirtualGeometryRasterDiagnostics {
	uint32_t candidate_groups = 0;
	uint32_t selected_clusters = 0;
	uint32_t fallback_clusters = 0;
	uint32_t frustum_rejected_clusters = 0;
	uint32_t cone_rejected_clusters = 0;
	uint32_t occlusion_rejected_clusters = 0;
	uint32_t overflow_clusters = 0;
	uint32_t per_eye_disagreement = 0;
	uint32_t per_eye_selected[2] = {};
	bool compute_indirect = false;
	bool native_mesh_shader = false;
	bool fail_open_occlusion = false;
	String backend_path = "unavailable";
};

struct VirtualGeometryRasterSelection {
	Vector<uint64_t> cluster_ids;
	Vector<uint32_t> eye_visibility_masks;
	// Missing pages required by the desired stereo-union cut. The current frame
	// still emits the complete active coarse cut while the renderer requests
	// these pages for a later atomic publication.
	Vector<uint64_t> requested_page_ids;
	Vector<uint32_t> requested_page_priorities;
	Vector<VirtualGeometryIndexedIndirectCommand> commands;
	VirtualGeometryRasterDiagnostics diagnostics;
};

// CPU reference for the GPU selector. It defines the same atomic group and
// fail-open rules as virtual_geometry_select.glsl and is deliberately kept
// independent of Flux/Metal types for deterministic validation.
class VirtualGeometryRasterSelector {
public:
	static VirtualGeometryRasterSelection select(const Package &p_package, const HashSet<uint64_t> &p_active_clusters, const VirtualGeometryRasterSelectionInput &p_input, VirtualGeometryRasterSelectionState *p_state = nullptr);
};

} // namespace RendererVirtualGeometry
