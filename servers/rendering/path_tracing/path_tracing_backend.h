/**************************************************************************/
/*  path_tracing_backend.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "path_tracing_scene_capture.h"

#include "core/string/string_name.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"

namespace RendererPathTracing {

enum class RenderMode : uint32_t {
	INTERACTIVE,
	PROGRESSIVE_REFERENCE,
	HYBRID_INTERACTIVE,
	XR_INTERACTIVE,
};

struct BackendCapabilities {
	bool available = false;
	bool acceleration_structures = false;
	bool dynamic_blas_update = false;
	bool temporal_reconstruction = false;
	bool hardware_ray_query = false;
	bool ray_tracing_pipeline = false;
	uint32_t max_views = 0;
	uint32_t supported_guides = 0;
	String unavailable_reason;
};

struct FrameRequest {
	PackedByteArray capture;
	RenderMode mode = RenderMode::INTERACTIVE;
	Vector<RID> output_color;
	Vector<RID> output_depth;
	Vector<RID> output_motion;
	Vector<RID> output_normal;
	Vector<RID> output_diffuse_albedo;
	Vector<RID> output_specular_albedo;
	Vector<RID> output_roughness;
	Vector<RID> output_denoise_strength;
	Vector<RID> output_reactive_mask;
	Vector<RID> output_specular_hit_distance;
	Vector<RID> output_transparency_overlay;
	uint32_t sample_index = 0;
	uint32_t max_bounces = 2;
};

struct StageTiming {
	StringName stage;
	double milliseconds = 0.0;
};

struct FrameResult {
	Vector<StageTiming> timings;
	uint32_t rendered_views = 0;
	uint32_t emitted_guides = 0;
	uint32_t blas_rebuilt = 0;
	uint32_t blas_refit = 0;
	uint32_t blas_reused = 0;
	bool history_reset = false;
};

class Backend {
public:
	virtual ~Backend() = default;
	virtual StringName get_name() const = 0;
	virtual BackendCapabilities get_capabilities() const = 0;
	virtual Error render(const FrameRequest &p_request, FrameResult &r_result, String *r_error = nullptr) = 0;
	virtual Error collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error = nullptr) const;

	Error validate_request(const FrameRequest &p_request, String *r_error = nullptr) const;
};

} // namespace RendererPathTracing
