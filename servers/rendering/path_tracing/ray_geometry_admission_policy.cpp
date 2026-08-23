/**************************************************************************/
/*  ray_geometry_admission_policy.cpp                                     */
/**************************************************************************/

#include "ray_geometry_admission_policy.h"

#include "core/math/math_funcs.h"

namespace RendererPathTracing {

bool ray_geometry_admission_precedes(const RayGeometryAdmissionPriority &p_left, const RayGeometryAdmissionPriority &p_right) {
	if (p_left.dynamic != p_right.dynamic) {
		return p_left.dynamic;
	}
	if (p_left.emissive != p_right.emissive) {
		return p_left.emissive;
	}
	const float left_distance = Math::is_finite(p_left.camera_distance) && p_left.camera_distance >= 0.0f ? p_left.camera_distance : 1.0e30f;
	const float right_distance = Math::is_finite(p_right.camera_distance) && p_right.camera_distance >= 0.0f ? p_right.camera_distance : 1.0e30f;
	if (!Math::is_equal_approx(left_distance, right_distance)) {
		return left_distance < right_distance;
	}
	return p_left.stable_id < p_right.stable_id;
}

} // namespace RendererPathTracing
