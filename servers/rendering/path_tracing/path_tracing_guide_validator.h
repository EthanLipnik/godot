/**************************************************************************/
/*  path_tracing_guide_validator.h                                        */
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

#include "path_tracing_scene_packet.h"

#include "core/variant/typed_array.h"

namespace RendererPathTracing {

struct GuidePlane {
	uint32_t guide = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t components = 0;
	PackedFloat32Array values;
};

struct GuideValidationResult {
	uint64_t valid_pixels = 0;
	uint64_t invalid_sentinel_pixels = 0;
	uint64_t unexpected_nan_pixels = 0;
	uint64_t infinite_pixels = 0;
	uint64_t out_of_range_pixels = 0;
	PackedByteArray debug_rgba8;

	bool is_valid() const {
		return unexpected_nan_pixels == 0 && infinite_pixels == 0 && out_of_range_pixels == 0;
	}
};

class GuideValidator {
public:
	static uint32_t expected_components(uint32_t p_guide);
	static Error validate_and_visualize(const GuidePlane &p_plane, GuideValidationResult &r_result, String *r_error = nullptr);
};

} // namespace RendererPathTracing
