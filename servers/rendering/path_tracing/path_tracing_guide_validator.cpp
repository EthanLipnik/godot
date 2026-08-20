/**************************************************************************/
/*  path_tracing_guide_validator.cpp                                      */
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

#include "path_tracing_guide_validator.h"

#include "core/math/math_funcs.h"

namespace RendererPathTracing {

static Error _guide_fail(const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return ERR_INVALID_PARAMETER;
}

uint32_t GuideValidator::expected_components(uint32_t p_guide) {
	switch (p_guide) {
		case GUIDE_DEPTH:
		case GUIDE_ROUGHNESS:
		case GUIDE_DENOISE_STRENGTH:
		case GUIDE_REACTIVE_MASK:
		case GUIDE_SPECULAR_HIT_DISTANCE:
			return 1;
		case GUIDE_MOTION:
			return 2;
		case GUIDE_NORMAL:
		case GUIDE_DIFFUSE_ALBEDO:
		case GUIDE_SPECULAR_ALBEDO:
			return 3;
		case GUIDE_TRANSPARENCY_OVERLAY:
			return 4;
		default:
			return 0;
	}
}

Error GuideValidator::validate_and_visualize(const GuidePlane &p_plane, GuideValidationResult &r_result, String *r_error) {
	r_result = {};
	const uint32_t expected = expected_components(p_plane.guide);
	if (expected == 0 || p_plane.components != expected || p_plane.width == 0 || p_plane.height == 0 ||
			p_plane.values.size() != int64_t(p_plane.width) * p_plane.height * p_plane.components) {
		return _guide_fail("The path-tracing guide plane has invalid dimensions or component count.", r_error);
	}
	r_result.debug_rgba8.resize(p_plane.width * p_plane.height * 4);
	const bool allows_nan = p_plane.guide == GUIDE_DEPTH || p_plane.guide == GUIDE_NORMAL ||
			p_plane.guide == GUIDE_ROUGHNESS || p_plane.guide == GUIDE_SPECULAR_HIT_DISTANCE;
	for (uint64_t pixel = 0; pixel < uint64_t(p_plane.width) * p_plane.height; pixel++) {
		const float *values = p_plane.values.ptr() + pixel * p_plane.components;
		bool has_nan = false;
		bool has_inf = false;
		for (uint32_t component = 0; component < p_plane.components; component++) {
			has_nan |= Math::is_nan(values[component]);
			has_inf |= Math::is_inf(values[component]);
		}
		uint8_t *debug = r_result.debug_rgba8.ptrw() + pixel * 4;
		debug[3] = 255;
		if (has_inf) {
			r_result.infinite_pixels++;
			debug[0] = 255;
			debug[1] = 255;
			debug[2] = 0;
			continue;
		}
		if (has_nan) {
			if (allows_nan) {
				r_result.invalid_sentinel_pixels++;
			} else {
				r_result.unexpected_nan_pixels++;
			}
			debug[0] = 255;
			debug[1] = 0;
			debug[2] = 255;
			continue;
		}

		bool out_of_range = false;
		if (p_plane.guide == GUIDE_DEPTH || p_plane.guide == GUIDE_ROUGHNESS ||
				p_plane.guide == GUIDE_DENOISE_STRENGTH || p_plane.guide == GUIDE_REACTIVE_MASK) {
			out_of_range = values[0] < 0.0f || values[0] > 1.0f;
		} else if (p_plane.guide == GUIDE_NORMAL) {
			const float length_squared = values[0] * values[0] + values[1] * values[1] + values[2] * values[2];
			out_of_range = length_squared < 0.99f * 0.99f || length_squared > 1.01f * 1.01f;
		} else if (p_plane.guide == GUIDE_DIFFUSE_ALBEDO || p_plane.guide == GUIDE_SPECULAR_ALBEDO ||
				p_plane.guide == GUIDE_TRANSPARENCY_OVERLAY) {
			for (uint32_t component = 0; component < p_plane.components; component++) {
				out_of_range |= values[component] < 0.0f || values[component] > 1.0f;
			}
		} else if (p_plane.guide == GUIDE_SPECULAR_HIT_DISTANCE) {
			out_of_range = values[0] < 0.0f;
		}
		if (out_of_range) {
			r_result.out_of_range_pixels++;
		}
		r_result.valid_pixels++;
		float red = values[0];
		float green = p_plane.components > 1 ? values[1] : red;
		float blue = p_plane.components > 2 ? values[2] : red;
		if (p_plane.guide == GUIDE_NORMAL) {
			red = red * 0.5f + 0.5f;
			green = green * 0.5f + 0.5f;
			blue = blue * 0.5f + 0.5f;
		} else if (p_plane.guide == GUIDE_MOTION) {
			red = red * 8.0f + 0.5f;
			green = green * 8.0f + 0.5f;
			blue = 0.5f;
		}
		debug[0] = uint8_t(CLAMP(red, 0.0f, 1.0f) * 255.0f + 0.5f);
		debug[1] = uint8_t(CLAMP(green, 0.0f, 1.0f) * 255.0f + 0.5f);
		debug[2] = uint8_t(CLAMP(blue, 0.0f, 1.0f) * 255.0f + 0.5f);
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

} // namespace RendererPathTracing
