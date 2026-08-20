/**************************************************************************/
/*  path_tracing_backend.cpp                                              */
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

#include "path_tracing_backend.h"

namespace RendererPathTracing {

static Error _backend_fail(Error p_error, const String &p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static Error _validate_optional_targets(const Vector<RID> &p_targets, int p_view_count, const char *p_name, String *r_error) {
	if (!p_targets.is_empty() && p_targets.size() != p_view_count) {
		return _backend_fail(ERR_INVALID_PARAMETER, vformat("Path-tracing %s targets must be empty or match the view count.", p_name), r_error);
	}
	for (const RID &target : p_targets) {
		if (!target.is_valid()) {
			return _backend_fail(ERR_INVALID_PARAMETER, vformat("A path-tracing %s target is invalid.", p_name), r_error);
		}
	}
	return OK;
}

Error Backend::validate_request(const FrameRequest &p_request, String *r_error) const {
	const BackendCapabilities capabilities = get_capabilities();
	if (!capabilities.available) {
		return _backend_fail(ERR_UNAVAILABLE, capabilities.unavailable_reason, r_error);
	}
	const Error capture_error = SceneCapture::validate(p_request.capture, r_error);
	if (capture_error != OK) {
		return capture_error;
	}
	if (p_request.max_bounces == 0 || p_request.max_bounces > 16) {
		return _backend_fail(ERR_INVALID_PARAMETER, "Path-tracing bounce count must be between 1 and 16.", r_error);
	}
	SceneCaptureHeader capture_header = {};
	SceneCompiler::read_record(p_request.capture, 0, capture_header);
	ScenePacketHeader packet_header = {};
	SceneCompiler::read_record(p_request.capture, capture_header.scene_packet_offset, packet_header);
	if (packet_header.camera_count > capabilities.max_views) {
		return _backend_fail(ERR_UNAVAILABLE, "The path-tracing backend does not support the requested view count.", r_error);
	}
	if (p_request.output_color.size() != (int)packet_header.camera_count) {
		return _backend_fail(ERR_INVALID_PARAMETER, "Each path-tracing view requires one color target.", r_error);
	}
	for (const RID &target : p_request.output_color) {
		if (!target.is_valid()) {
			return _backend_fail(ERR_INVALID_PARAMETER, "A path-tracing color target is invalid.", r_error);
		}
	}
	struct NamedTargets {
		const Vector<RID> *targets;
		const char *name;
	};
	const NamedTargets optional_targets[] = {
		{ &p_request.output_depth, "depth" },
		{ &p_request.output_motion, "motion" },
		{ &p_request.output_normal, "normal" },
		{ &p_request.output_diffuse_albedo, "diffuse-albedo" },
		{ &p_request.output_specular_albedo, "specular-albedo" },
		{ &p_request.output_roughness, "roughness" },
		{ &p_request.output_denoise_strength, "denoise-strength" },
		{ &p_request.output_reactive_mask, "reactive-mask" },
		{ &p_request.output_specular_hit_distance, "specular-hit-distance" },
		{ &p_request.output_transparency_overlay, "transparency-overlay" },
	};
	for (const NamedTargets &named_targets : optional_targets) {
		const Error target_error = _validate_optional_targets(*named_targets.targets, p_request.output_color.size(), named_targets.name, r_error);
		if (target_error != OK) {
			return target_error;
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error Backend::collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error) const {
	r_timings.clear();
	return _backend_fail(ERR_UNAVAILABLE, "This path-tracing backend does not expose completed GPU timings.", r_error);
}

} // namespace RendererPathTracing
