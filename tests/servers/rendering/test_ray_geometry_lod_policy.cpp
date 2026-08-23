/**************************************************************************/
/*  test_ray_geometry_lod_policy.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_ray_geometry_lod_policy)

#include "servers/rendering/path_tracing/ray_geometry_lod_policy.h"

namespace TestRayGeometryLODPolicy {

using namespace RendererPathTracing;

static RayGeometryLODPolicyInput _base_input() {
	RayGeometryLODPolicyInput input;
	input.world_bounds = AABB(Vector3(20.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
	input.camera_position = Vector3();
	input.generated_lods_available = true;
	return input;
}

TEST_CASE("[PathTracing][RayGeometryLOD] Far static opaque geometry retains generated LODs") {
	const RayGeometryLODPolicyDecision decision = select_ray_geometry_lod(_base_input());
	CHECK(decision.use_generated_lod);
	CHECK(decision.reason == RAY_GEOMETRY_LOD_REASON_STATIC_FAR);
	CHECK(decision.camera_distance == doctest::Approx(20.0f));
}

TEST_CASE("[PathTracing][RayGeometryLOD] Deforming and alpha-masked geometry retains the base mesh") {
	RayGeometryLODPolicyInput dynamic = _base_input();
	dynamic.deforming = true;
	RayGeometryLODPolicyDecision decision = select_ray_geometry_lod(dynamic);
	CHECK_FALSE(decision.use_generated_lod);
	CHECK(decision.reason == RAY_GEOMETRY_LOD_REASON_DYNAMIC);

	RayGeometryLODPolicyInput alpha_masked = _base_input();
	alpha_masked.alpha_masked = true;
	decision = select_ray_geometry_lod(alpha_masked);
	CHECK_FALSE(decision.use_generated_lod);
	CHECK(decision.reason == RAY_GEOMETRY_LOD_REASON_ALPHA_MASK);
}

TEST_CASE("[PathTracing][RayGeometryLOD] Near field uses closest geometry bounds rather than the bounds center") {
	RayGeometryLODPolicyInput input = _base_input();
	input.world_bounds = AABB(Vector3(8.0f, -1.0f, -1.0f), Vector3(10.0f, 2.0f, 2.0f));
	const RayGeometryLODPolicyDecision decision = select_ray_geometry_lod(input);
	CHECK_FALSE(decision.use_generated_lod);
	CHECK(decision.reason == RAY_GEOMETRY_LOD_REASON_NEAR_FIELD);
	CHECK(decision.camera_distance == doctest::Approx(8.0f));
}

TEST_CASE("[PathTracing][RayGeometryLOD] Invalid or orthographic distance inputs conservatively retain the base mesh") {
	RayGeometryLODPolicyInput input = _base_input();
	input.camera_distance_is_meaningful = false;
	RayGeometryLODPolicyDecision decision = select_ray_geometry_lod(input);
	CHECK_FALSE(decision.use_generated_lod);
	CHECK(decision.reason == RAY_GEOMETRY_LOD_REASON_INVALID_BOUNDS);

	input = _base_input();
	input.world_bounds.size.x = -1.0f;
	decision = select_ray_geometry_lod(input);
	CHECK_FALSE(decision.use_generated_lod);
	CHECK(decision.reason == RAY_GEOMETRY_LOD_REASON_INVALID_BOUNDS);
}

} // namespace TestRayGeometryLODPolicy
