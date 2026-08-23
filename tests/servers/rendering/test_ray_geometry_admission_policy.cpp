/**************************************************************************/
/*  test_ray_geometry_admission_policy.cpp                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_ray_geometry_admission_policy)

#include "servers/rendering/path_tracing/ray_geometry_admission_policy.h"

#include <limits>

namespace TestRayGeometryAdmissionPolicy {

using namespace RendererPathTracing;

TEST_CASE("[PathTracing][RayGeometryAdmission] Dynamic and emissive geometry converge before ordinary static geometry") {
	RayGeometryAdmissionPriority ordinary;
	ordinary.stable_id = 1;
	ordinary.camera_distance = 1.0f;

	RayGeometryAdmissionPriority emissive;
	emissive.stable_id = 2;
	emissive.camera_distance = 100.0f;
	emissive.emissive = true;
	CHECK(ray_geometry_admission_precedes(emissive, ordinary));

	RayGeometryAdmissionPriority dynamic;
	dynamic.stable_id = 3;
	dynamic.camera_distance = 1000.0f;
	dynamic.dynamic = true;
	CHECK(ray_geometry_admission_precedes(dynamic, emissive));
}

TEST_CASE("[PathTracing][RayGeometryAdmission] Distance and stable identity provide deterministic fallback order") {
	RayGeometryAdmissionPriority near;
	near.stable_id = 20;
	near.camera_distance = 2.0f;
	RayGeometryAdmissionPriority far;
	far.stable_id = 1;
	far.camera_distance = 3.0f;
	CHECK(ray_geometry_admission_precedes(near, far));

	far.camera_distance = near.camera_distance;
	CHECK(ray_geometry_admission_precedes(far, near));

	near.camera_distance = std::numeric_limits<float>::infinity();
	CHECK(ray_geometry_admission_precedes(far, near));
}

} // namespace TestRayGeometryAdmissionPolicy
