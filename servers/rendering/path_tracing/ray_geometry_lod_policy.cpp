/**************************************************************************/
/*  ray_geometry_lod_policy.cpp                                           */
/**************************************************************************/

#include "ray_geometry_lod_policy.h"

#include "core/math/math_funcs.h"

namespace RendererPathTracing {

static bool _finite_aabb(const AABB &p_aabb) {
	return p_aabb.is_finite() && p_aabb.size.x >= 0.0f && p_aabb.size.y >= 0.0f && p_aabb.size.z >= 0.0f;
}

static float _distance_to_aabb(const Vector3 &p_point, const AABB &p_aabb) {
	const Vector3 closest = p_point.clamp(p_aabb.position, p_aabb.get_end());
	return p_point.distance_to(closest);
}

RayGeometryLODPolicyDecision select_ray_geometry_lod(const RayGeometryLODPolicyInput &p_input) {
	RayGeometryLODPolicyDecision decision;
	if (!p_input.generated_lods_available) {
		return decision;
	}
	if (p_input.deforming) {
		decision.reason = RAY_GEOMETRY_LOD_REASON_DYNAMIC;
		return decision;
	}
	if (p_input.alpha_masked) {
		decision.reason = RAY_GEOMETRY_LOD_REASON_ALPHA_MASK;
		return decision;
	}
	if (!p_input.camera_distance_is_meaningful || !_finite_aabb(p_input.world_bounds) || !p_input.camera_position.is_finite() || !Math::is_finite(p_input.near_field_distance) || p_input.near_field_distance < 0.0f) {
		decision.reason = RAY_GEOMETRY_LOD_REASON_INVALID_BOUNDS;
		return decision;
	}

	decision.camera_distance = _distance_to_aabb(p_input.camera_position, p_input.world_bounds);
	if (decision.camera_distance <= p_input.near_field_distance) {
		decision.reason = RAY_GEOMETRY_LOD_REASON_NEAR_FIELD;
		return decision;
	}

	decision.reason = RAY_GEOMETRY_LOD_REASON_STATIC_FAR;
	decision.use_generated_lod = true;
	return decision;
}

const char *ray_geometry_lod_reason_name(RayGeometryLODReason p_reason) {
	switch (p_reason) {
		case RAY_GEOMETRY_LOD_REASON_STATIC_FAR:
			return "static_far";
		case RAY_GEOMETRY_LOD_REASON_NO_GENERATED_LOD:
			return "no_generated_lod";
		case RAY_GEOMETRY_LOD_REASON_DYNAMIC:
			return "dynamic";
		case RAY_GEOMETRY_LOD_REASON_ALPHA_MASK:
			return "alpha_mask";
		case RAY_GEOMETRY_LOD_REASON_NEAR_FIELD:
			return "near_field";
		case RAY_GEOMETRY_LOD_REASON_INVALID_BOUNDS:
			return "invalid_bounds";
	}
	return "unknown";
}

} // namespace RendererPathTracing
