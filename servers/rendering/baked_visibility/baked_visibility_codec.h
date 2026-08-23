/**************************************************************************/
/*  baked_visibility_codec.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/string/node_path.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

// The payload is deliberately independent from scene instances, rendering RIDs,
// and object IDs. It can therefore be checked before a scene is instantiated.
struct BakedVisibilityData3DData {
	static constexpr uint32_t FORMAT_VERSION = 1;
	static constexpr uint32_t ALGORITHM_VERSION = 1;
	static constexpr uint32_t MAX_SERIALIZED_BYTES = 512 * 1024 * 1024;
	static constexpr uint32_t MAX_CELLS = 65536;
	enum InstanceKind : uint8_t {
		INSTANCE_KIND_GEOMETRY = 0,
		INSTANCE_KIND_POSITIONAL_LIGHT = 1,
		INSTANCE_KIND_DIRECTIONAL_LIGHT = 2,
	};
	enum InstanceFlags : uint32_t {
		INSTANCE_FLAG_CERTIFIED_BLOCKER = 1 << 0,
		INSTANCE_FLAG_DYNAMIC = 1 << 1,
		INSTANCE_FLAG_UNMAPPED = 1 << 2,
	};
	enum CellFlags : uint32_t {
		CELL_FLAG_DEGRADED = 1 << 0,
		CELL_FLAG_FAIL_OPEN = 1 << 1,
	};
	static constexpr uint32_t INSTANCE_FLAGS_MASK = INSTANCE_FLAG_CERTIFIED_BLOCKER | INSTANCE_FLAG_DYNAMIC | INSTANCE_FLAG_UNMAPPED;
	static constexpr uint32_t CELL_FLAGS_MASK = CELL_FLAG_DEGRADED | CELL_FLAG_FAIL_OPEN;

	struct Instance {
		NodePath path;
		uint8_t kind = INSTANCE_KIND_GEOMETRY;
		uint32_t flags = 0;
		AABB local_bounds;
		PackedByteArray signature_sha256;
	};

	struct Cell {
		uint32_t flags = 0;
		uint32_t primary_set = 0;
		uint32_t transport_set = 0;
	};

	uint64_t source_uid = 0;
	String source_path;
	PackedByteArray source_sha256;
	AABB local_bounds;
	Vector3 cell_size = Vector3(1.0f, 1.0f, 1.0f);
	Vector3i grid_size = Vector3i(1, 1, 1);
	uint32_t bake_mask = 0xfffff;
	float transport_distance = -1.0f;
	float lookup_margin = 0.1f;
	float coverage_tolerance = 0.0f;
	float transport_tolerance = 0.0f;
	Vector<Instance> instances;
	Vector<Cell> cells;
	Vector<Vector<uint32_t>> sets;
	String report;
	Vector<double> stage_times_ms;
};

class BakedVisibilityCodec {
public:
	static Error encode(const BakedVisibilityData3DData &p_data, PackedByteArray &r_bytes, String *r_error = nullptr);
	static Error decode(const PackedByteArray &p_bytes, BakedVisibilityData3DData &r_data, String *r_error = nullptr);
	static Error validate(const BakedVisibilityData3DData &p_data, String *r_error = nullptr);
};
