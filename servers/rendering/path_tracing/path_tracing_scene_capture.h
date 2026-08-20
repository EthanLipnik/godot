/**************************************************************************/
/*  path_tracing_scene_capture.h                                          */
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

#include "path_tracing_scene_compiler.h"

namespace RendererPathTracing {

static constexpr uint32_t SCENE_CAPTURE_MAGIC = 0x50435450; // "PTCP" in little-endian storage.
static constexpr uint32_t SCENE_CAPTURE_VERSION = 1;

enum GeometryFlags : uint32_t {
	GEOMETRY_DYNAMIC = 1 << 0,
	GEOMETRY_OPAQUE = 1 << 1,
};

struct alignas(16) GeometryVertex {
	Float4 current_position;
	Float4 previous_position;
	Float4 normal;
	Float4 tangent;
	Float4 uv;
};

struct alignas(16) GeometryRecord {
	uint64_t geometry_id;
	uint32_t vertex_offset;
	uint32_t vertex_count;
	uint32_t index_offset;
	uint32_t index_count;
	uint32_t flags;
	uint32_t reserved;
	Float4 bounds_min;
	Float4 bounds_max;
};

struct alignas(16) SceneCaptureHeader {
	uint32_t magic;
	uint32_t capture_version;
	uint32_t endian_tag;
	uint32_t header_size;
	uint64_t total_size;
	uint64_t payload_hash;
	uint32_t scene_packet_offset;
	uint32_t scene_packet_size;
	uint32_t geometry_count;
	uint32_t geometry_offset;
	uint32_t vertex_count;
	uint32_t vertex_offset;
	uint32_t index_count;
	uint32_t index_offset;
};

struct GeometryInput {
	uint64_t geometry_id = 0;
	uint32_t flags = 0;
	Float4 bounds_min = {};
	Float4 bounds_max = {};
	Vector<GeometryVertex> vertices;
	Vector<uint32_t> indices;
};

struct SceneCaptureInput {
	ScenePacketInput scene;
	Vector<GeometryInput> geometries;
};

class SceneCapture {
public:
	static Error compile(const SceneCaptureInput &p_input, PackedByteArray &r_capture, String *r_error = nullptr);
	static Error validate(const PackedByteArray &p_capture, String *r_error = nullptr);
	static Error get_scene_packet(const PackedByteArray &p_capture, PackedByteArray &r_packet, String *r_error = nullptr);
	static Error save(const String &p_path, const PackedByteArray &p_capture, String *r_error = nullptr);
	static Error load(const String &p_path, PackedByteArray &r_capture, String *r_error = nullptr);
};

static_assert(sizeof(GeometryVertex) == 80);
static_assert(sizeof(GeometryRecord) == 64);
static_assert(sizeof(SceneCaptureHeader) == 64);

} // namespace RendererPathTracing
