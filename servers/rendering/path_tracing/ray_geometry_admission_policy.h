/**************************************************************************/
/*  ray_geometry_admission_policy.h                                       */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"

namespace RendererPathTracing {

struct RayGeometryAdmissionPriority {
	uint64_t stable_id = 0;
	float camera_distance = 1.0e30f;
	bool dynamic = false;
	bool emissive = false;
};

// Returns true when the left candidate should be considered before the right
// candidate for a bounded new-BLAS budget. Reuse/refit remains unbudgeted in
// the backend, so this only controls which missing geometry converges first.
bool ray_geometry_admission_precedes(const RayGeometryAdmissionPriority &p_left, const RayGeometryAdmissionPriority &p_right);

} // namespace RendererPathTracing
