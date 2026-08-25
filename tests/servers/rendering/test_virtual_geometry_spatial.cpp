/**************************************************************************/
/*  test_virtual_geometry_spatial.cpp                                    */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_geometry_spatial)

#include "servers/rendering/virtual_geometry/virtual_geometry_spatial.h"

namespace TestVirtualGeometrySpatial {

using namespace RendererVirtualGeometry;

TEST_CASE("[Rendering][VirtualGeometry] VG5 spatial identities survive floating-origin rebases") {
	VirtualGeometrySpatialRuntime runtime;
	const uint64_t package_id = 0x1234;
	const Vector3 world_position(4097.0, 128.0, -8193.0);
	const SpatialCellIdentity before = runtime.identify_cell(package_id, world_position, 128.0);

	const SpatialRebaseResult rebase = runtime.rebase(Vector3(4096.0, 128.0, -8192.0));
	CHECK(rebase.changed);
	CHECK_EQ(rebase.revision, uint64_t(1));
	const SpatialCellIdentity after = runtime.identify_cell(package_id, runtime.to_world_position(runtime.to_camera_relative(world_position)), 128.0);
	CHECK_EQ(before.stable_id, after.stable_id);
	CHECK(before.key == after.key);
	CHECK_EQ(runtime.get_origin_state().revision, uint64_t(1));

	Transform3D transform(Basis(), world_position);
	CHECK(runtime.to_camera_relative(transform).origin == Vector3(1.0, 0.0, -1.0));
}

TEST_CASE("[Rendering][VirtualGeometry] VG5 traversal prediction retains an omnidirectional reserve during a 180 degree turn") {
	VirtualGeometrySpatialRuntime runtime;
	SpatialTraversalInput input;
	input.package_identity = 7;
	input.cell_size = 10.0;
	input.position = Vector3(0, 0, 0);
	input.velocity = Vector3(0, 0, -30);
	input.view_direction = Vector3(0, 0, -1);
	input.angular_velocity = Vector3(0, Math::PI, 0);
	input.horizon_seconds = 1.0;
	input.maximum_cells = 128;
	const SpatialTraversalPrediction prediction = runtime.predict_traversal(input);
	CHECK(prediction.omnidirectional_coarse_reserve);
	CHECK(prediction.coarse_reserve_cells.size() >= 2);
	CHECK(prediction.cells.size() <= int(input.maximum_cells));
	CHECK(prediction.sampled_positions >= 16);
}

TEST_CASE("[Rendering][VirtualGeometry] VG5 traversal prediction covers vertical launches and diagonal cell crossings") {
	VirtualGeometrySpatialRuntime runtime;
	SpatialTraversalInput vertical;
	vertical.package_identity = 9;
	vertical.cell_size = 10.0;
	vertical.position = Vector3(1, 1, 1);
	vertical.velocity = Vector3(0, 8, 0);
	// The trajectory reaches several 10 m cells in one horizon; this models a
	// launch rather than a slow elevator transition and checks the predictor's
	// vertical extent rather than only its sampled start cell.
	vertical.acceleration = Vector3(0, 100, 0);
	vertical.horizon_seconds = 1.0;
	vertical.maximum_cells = 128;
	const SpatialTraversalPrediction vertical_prediction = runtime.predict_traversal(vertical);
	bool reached_upper_cell = false;
	for (const SpatialCellIdentity &cell : vertical_prediction.cells) {
		reached_upper_cell |= cell.key.y >= 4;
	}
	CHECK(reached_upper_cell);

	SpatialTraversalInput diagonal = vertical;
	diagonal.position = Vector3(1, 1, 1);
	diagonal.velocity = Vector3(25, 0, 25);
	diagonal.acceleration = Vector3();
	const SpatialTraversalPrediction diagonal_prediction = runtime.predict_traversal(diagonal);
	bool crossed_x = false;
	bool crossed_z = false;
	for (const SpatialCellIdentity &cell : diagonal_prediction.cells) {
		crossed_x |= cell.key.x >= 2;
		crossed_z |= cell.key.z >= 2;
	}
	CHECK(crossed_x);
	CHECK(crossed_z);
}

TEST_CASE("[Rendering][VirtualGeometry] VG5 repeated families share accounting without scene-node expansion") {
	VirtualGeometrySpatialRuntime runtime;
	const uint64_t family = VirtualGeometrySpatialIdentity::repeated_family_id(100, 5);
	SpatialRepeatedInstance first;
	first.family_id = family;
	first.instance_id = VirtualGeometrySpatialIdentity::repeated_instance_id(family, 1);
	first.semantic_class_id = 11;
	first.resident_bytes = 64;
	SpatialRepeatedInstance second = first;
	second.instance_id = VirtualGeometrySpatialIdentity::repeated_instance_id(family, 2);
	second.resident_bytes = 96;
	CHECK(runtime.register_repeated_instance(first));
	CHECK(runtime.register_repeated_instance(second));
	const Vector<SpatialRepeatedFamilyDiagnostics> families = runtime.get_repeated_family_diagnostics();
	REQUIRE_EQ(families.size(), 1);
	CHECK_EQ(families[0].family_id, family);
	CHECK_EQ(families[0].instance_count, uint32_t(2));
	CHECK_EQ(families[0].resident_bytes, uint64_t(160));
	CHECK(runtime.unregister_repeated_instance(first.instance_id));
	CHECK_EQ(runtime.get_repeated_family_diagnostics()[0].instance_count, uint32_t(1));
}

TEST_CASE("[Rendering][VirtualGeometry] VG5 independent working-set classes and semantic quotas prevent starvation") {
	VirtualGeometrySpatialRuntime runtime;
	runtime.set_budget(SpatialWorkingSetClass::GEOMETRY, { 100, 20 });
	runtime.set_budget(SpatialWorkingSetClass::EMISSION, { 10, 0 });
	runtime.set_semantic_budget(77, SpatialWorkingSetClass::GEOMETRY, 4);

	SpatialWorkingSetReservation geometry;
	SpatialWorkingSetRequest geometry_request;
	geometry_request.stable_id = 1;
	geometry_request.working_set_class = SpatialWorkingSetClass::GEOMETRY;
	geometry_request.bytes = 70;
	geometry_request.semantic_class_id = 1;
	CHECK(runtime.reserve(geometry_request, geometry));
	SpatialWorkingSetRequest blocked_geometry = geometry_request;
	blocked_geometry.stable_id = 2;
	blocked_geometry.bytes = 20;
	CHECK_FALSE(runtime.reserve(blocked_geometry, geometry)); // protected coarse reserve remains untouched.

	SpatialWorkingSetRequest emission_request;
	emission_request.stable_id = 3;
	emission_request.working_set_class = SpatialWorkingSetClass::EMISSION;
	emission_request.bytes = 8;
	SpatialWorkingSetReservation emission;
	CHECK(runtime.reserve(emission_request, emission));

	SpatialWorkingSetRequest semantic_request;
	semantic_request.stable_id = 4;
	semantic_request.working_set_class = SpatialWorkingSetClass::GEOMETRY;
	semantic_request.semantic_class_id = 77;
	semantic_request.bytes = 5;
	SpatialWorkingSetReservation semantic;
	CHECK_FALSE(runtime.reserve(semantic_request, semantic));
	CHECK_EQ(runtime.get_budget_snapshot(SpatialWorkingSetClass::EMISSION).used_bytes, uint64_t(8));
}

TEST_CASE("[Rendering][VirtualGeometry] VG5 coarse eligibility fails open and preserves persistent coverage") {
	VirtualGeometrySpatialRuntime runtime;
	SpatialCoarseEligibilityInput input;
	input.fine_page_resident = false;
	SpatialCoarseEligibility fallback = runtime.evaluate_coarse_eligibility(input);
	CHECK(fallback.eligible);
	CHECK(fallback.use_coarse_fallback);
	CHECK_FALSE(fallback.fail_open);

	input.visibility_data_valid = false;
	fallback = runtime.evaluate_coarse_eligibility(input);
	CHECK(fallback.use_coarse_fallback);
	CHECK(fallback.fail_open);

	input.persistent_raster_coarse_available = false;
	const SpatialCoarseEligibility unavailable = runtime.evaluate_coarse_eligibility(input);
	CHECK_FALSE(unavailable.eligible);
	CHECK(unavailable.fail_open);
}

} // namespace TestVirtualGeometrySpatial
