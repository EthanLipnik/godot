/**************************************************************************/
/*  test_virtual_geometry_ray.cpp                                         */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_geometry_ray)

#include "servers/rendering/virtual_geometry/virtual_geometry_ray.h"

namespace TestVirtualGeometryRay {

using namespace RendererVirtualGeometry;

static Manifest _manifest() {
	Manifest manifest;
	for (uint32_t tier = 0; tier < 3; tier++) {
		RayGroupDescriptor group;
		group.stable_id = 100 + tier;
		group.transport_region_id = 7;
		group.revision = 10 + tier;
		group.tier = RayTransportTier(tier);
		group.cluster_ids.push_back(1000 + tier);
		group.bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
		group.persistent_coarse = tier == uint32_t(RayTransportTier::TIER_FAR);
		manifest.ray_groups.push_back(group);
	}
	return manifest;
}

static void _activate(VirtualGeometryRayHierarchy &p_hierarchy, uint64_t p_group, uint64_t p_version, uint64_t p_serial) {
	REQUIRE_EQ(p_hierarchy.queue_build(p_group, p_version), OK);
	REQUIRE_EQ(p_hierarchy.begin_build(p_group, p_serial, 4096, false), OK);
	REQUIRE_EQ(p_hierarchy.complete_build(p_group, p_serial, 0), OK);
	p_hierarchy.activate_frame_boundary(p_serial, p_serial + 1);
}

TEST_CASE("[Rendering][VirtualGeometry] VG4 role selection is independent of raster state") {
	RayTransportSelectionInput input;
	input.role = RayTransportRole::SHARP_REFLECTION;
	input.distance = 4.0f;
	input.ray_footprint = 0.001f;
	input.roughness = 0.01f;
	CHECK_EQ(VirtualGeometryRayHierarchy::select_desired_tier(input), RayTransportTier::TIER_NEAR);
	input.role = RayTransportRole::ROUGH_REFLECTION;
	input.roughness = 0.4f;
	CHECK_EQ(VirtualGeometryRayHierarchy::select_desired_tier(input), RayTransportTier::TIER_MIDDLE);
	input.role = RayTransportRole::DIFFUSE_INDIRECT;
	input.path_depth = 3;
	CHECK_EQ(VirtualGeometryRayHierarchy::select_desired_tier(input), RayTransportTier::TIER_FAR);
}

TEST_CASE("[Rendering][VirtualGeometry] VG4 pending detail retains persistent off-screen coarse transport") {
	VirtualGeometryRayHierarchy hierarchy;
	REQUIRE_EQ(hierarchy.set_manifest(_manifest()), OK);
	_activate(hierarchy, 100, 1, 1);
	REQUIRE_EQ(hierarchy.queue_build(102, 1), OK);
	REQUIRE_EQ(hierarchy.begin_build(102, 2, 8192, true), OK);

	RayTransportSelectionInput input;
	input.role = RayTransportRole::SHARP_REFLECTION;
	input.distance = 2.0f;
	input.ray_footprint = 0.001f;
	input.roughness = 0.0f;
	input.off_screen_influence = true;
	const RayTransportSelection pending = hierarchy.select(input);
	REQUIRE_EQ(pending.regions.size(), 1);
	CHECK_EQ(pending.regions[0].active_group_id, 100);
	CHECK(pending.regions[0].coarse_fallback);
	CHECK(pending.regions[0].off_screen_retained);
	CHECK_EQ(pending.off_screen_retained, 1);
}

TEST_CASE("[Rendering][VirtualGeometry] VG4 build compaction and frame-boundary swap are atomic") {
	VirtualGeometryRayHierarchy hierarchy;
	REQUIRE_EQ(hierarchy.set_manifest(_manifest()), OK);
	_activate(hierarchy, 100, 1, 1);
	REQUIRE_EQ(hierarchy.queue_build(102, 1), OK);
	REQUIRE_EQ(hierarchy.begin_build(102, 2, 8192, true), OK);
	CHECK_EQ(hierarchy.complete_build(102, 1, 4096), ERR_BUSY);
	REQUIRE_EQ(hierarchy.complete_build(102, 2, 4096), OK);
	REQUIRE_EQ(hierarchy.begin_compaction(102, 3), OK);
	hierarchy.activate_frame_boundary(3, 4);
	CHECK_EQ(hierarchy.get_active_version(102), 0); // copy has not completed.
	REQUIRE_EQ(hierarchy.complete_compaction(102, 3), OK);
	hierarchy.activate_frame_boundary(3, 4);
	CHECK_EQ(hierarchy.get_active_version(102), 1);
	CHECK_EQ(hierarchy.get_diagnostics().builds, 2);
	CHECK_EQ(hierarchy.get_diagnostics().compactions, 1);
	CHECK_EQ(hierarchy.get_diagnostics().swaps, 2);
}

TEST_CASE("[Rendering][VirtualGeometry] VG4 failure and retirement never remove the valid active cut") {
	VirtualGeometryRayHierarchy hierarchy;
	REQUIRE_EQ(hierarchy.set_manifest(_manifest()), OK);
	_activate(hierarchy, 100, 1, 1);
	_activate(hierarchy, 101, 1, 2);
	hierarchy.mark_used(101, 8);
	REQUIRE_EQ(hierarchy.queue_build(101, 2), OK);
	REQUIRE_EQ(hierarchy.begin_build(101, 9, 4096, false), OK);
	CHECK_EQ(hierarchy.get_active_version(101), 1);
	REQUIRE_EQ(hierarchy.fail_build(101), OK);
	CHECK_EQ(hierarchy.get_active_version(101), 1);
	CHECK_EQ(hierarchy.get_state(101), RayGroupBuildState::ACTIVE);

	REQUIRE_EQ(hierarchy.queue_build(101, 2), OK);
	REQUIRE_EQ(hierarchy.begin_build(101, 10, 4096, false), OK);
	REQUIRE_EQ(hierarchy.complete_build(101, 10, 0), OK);
	hierarchy.activate_frame_boundary(10, 11);
	CHECK_EQ(hierarchy.get_active_version(101), 2);
	hierarchy.process_retirements(10);
	CHECK_EQ(hierarchy.get_diagnostics().retirements, 0);
	hierarchy.process_retirements(11);
	CHECK_EQ(hierarchy.get_diagnostics().retirements, 1);
}

} // namespace TestVirtualGeometryRay
