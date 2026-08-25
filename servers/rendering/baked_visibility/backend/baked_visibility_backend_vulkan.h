/**************************************************************************/
/*  baked_visibility_backend_vulkan.h                                     */
/**************************************************************************/

#pragma once

#include "core/templates/rid.h"
#include "baked_visibility_backend.h"

class RenderingDevice;

// Generic RenderingDevice contract for the Windows Vulkan/RTX adapter. It
// intentionally exposes no Vulkan SDK types, keeping BLAS/TLAS and pipeline
// ownership portable and capability-gated.
class BakedVisibilityVulkanBatchContract {
public:
	static bool is_supported(const RenderingDevice *p_rd);
	static String raygen_source();
	static String miss_source();
	static String closest_hit_source();
	// Schedules one ray per canonical candidate against blocker box geometry.
	// Candidate classification and the hit/miss payload are read back before
	// returning. The caller still retains CPU certification authority.
	static Error execute_batch(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
};
