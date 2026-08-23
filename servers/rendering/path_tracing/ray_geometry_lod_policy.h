/**************************************************************************/
/*  ray_geometry_lod_policy.h                                             */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"

namespace RendererPathTracing {

// Keep the base mesh for geometry whose simplification can change ray-hit
// material semantics. The value is intentionally a policy input so a shared
// renderer can expose a project setting without changing this contract.
constexpr float RAY_GEOMETRY_LOD_DEFAULT_NEAR_FIELD_DISTANCE = 10.0f;

enum RayGeometryLODReason {
	RAY_GEOMETRY_LOD_REASON_STATIC_FAR,
	RAY_GEOMETRY_LOD_REASON_NO_GENERATED_LOD,
	RAY_GEOMETRY_LOD_REASON_DYNAMIC,
	RAY_GEOMETRY_LOD_REASON_ALPHA_MASK,
	RAY_GEOMETRY_LOD_REASON_NEAR_FIELD,
	RAY_GEOMETRY_LOD_REASON_INVALID_BOUNDS,
};

struct RayGeometryLODPolicyInput {
	AABB world_bounds;
	Vector3 camera_position;
	float near_field_distance = RAY_GEOMETRY_LOD_DEFAULT_NEAR_FIELD_DISTANCE;
	bool generated_lods_available = false;
	bool deforming = false;
	bool alpha_masked = false;
	// Orthographic cameras do not have a useful camera-to-bounds distance for
	// this policy, so they conservatively keep the base mesh.
	bool camera_distance_is_meaningful = true;
};

struct RayGeometryLODPolicyDecision {
	RayGeometryLODReason reason = RAY_GEOMETRY_LOD_REASON_NO_GENERATED_LOD;
	float camera_distance = 0.0f;
	bool use_generated_lod = false;
};

RayGeometryLODPolicyDecision select_ray_geometry_lod(const RayGeometryLODPolicyInput &p_input);
const char *ray_geometry_lod_reason_name(RayGeometryLODReason p_reason);

} // namespace RendererPathTracing
