/**************************************************************************/
/*  path_tracing_scene_capture.cpp                                        */
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

#include "path_tracing_scene_capture.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"

#include <cstring>
#include <limits>

namespace RendererPathTracing {

static Error _capture_fail(Error p_error, const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static bool _capture_finite(const Float4 &p_value) {
	return Math::is_finite(p_value.x) && Math::is_finite(p_value.y) && Math::is_finite(p_value.z) && Math::is_finite(p_value.w);
}

Error SceneCapture::compile(const SceneCaptureInput &p_input, PackedByteArray &r_capture, String *r_error) {
	PackedByteArray packet;
	const Error packet_error = SceneCompiler::compile(p_input.scene, packet, r_error);
	if (packet_error != OK) {
		return packet_error;
	}

	uint64_t vertex_count = 0;
	uint64_t index_count = 0;
	HashSet<uint64_t> geometry_ids;
	for (const GeometryInput &geometry : p_input.geometries) {
		if (geometry.geometry_id == 0 || geometry_ids.has(geometry.geometry_id) || geometry.vertices.is_empty() ||
				geometry.indices.is_empty() || geometry.indices.size() % 3 != 0 || !_capture_finite(geometry.bounds_min) ||
				!_capture_finite(geometry.bounds_max)) {
			return _capture_fail(ERR_INVALID_PARAMETER, "A path-tracing capture geometry record is invalid.", r_error);
		}
		geometry_ids.insert(geometry.geometry_id);
		for (uint32_t index : geometry.indices) {
			if (index >= (uint32_t)geometry.vertices.size()) {
				return _capture_fail(ERR_INVALID_PARAMETER, "A path-tracing capture index is out of bounds.", r_error);
			}
		}
		for (const GeometryVertex &vertex : geometry.vertices) {
			if (!_capture_finite(vertex.current_position) || !_capture_finite(vertex.previous_position) ||
					!_capture_finite(vertex.normal) || !_capture_finite(vertex.tangent) || !_capture_finite(vertex.uv)) {
				return _capture_fail(ERR_INVALID_PARAMETER, "A path-tracing capture vertex is not finite.", r_error);
			}
		}
		vertex_count += geometry.vertices.size();
		index_count += geometry.indices.size();
	}
	for (const InstanceRecord &instance : p_input.scene.instances) {
		if (!geometry_ids.has(instance.geometry_id)) {
			return _capture_fail(ERR_INVALID_PARAMETER, "A path-tracing instance references geometry absent from the capture.", r_error);
		}
	}

	const uint64_t total_size = sizeof(SceneCaptureHeader) + packet.size() +
			(uint64_t)p_input.geometries.size() * sizeof(GeometryRecord) + vertex_count * sizeof(GeometryVertex) + index_count * sizeof(uint32_t);
	if (vertex_count > std::numeric_limits<uint32_t>::max() || index_count > std::numeric_limits<uint32_t>::max() ||
			total_size > std::numeric_limits<uint32_t>::max() || total_size > (uint64_t)std::numeric_limits<int>::max()) {
		return _capture_fail(ERR_OUT_OF_MEMORY, "The path-tracing capture exceeds version-1 address limits.", r_error);
	}

	SceneCaptureHeader header = {};
	header.magic = SCENE_CAPTURE_MAGIC;
	header.capture_version = SCENE_CAPTURE_VERSION;
	header.endian_tag = SCENE_PACKET_ENDIAN_TAG;
	header.header_size = sizeof(SceneCaptureHeader);
	header.total_size = total_size;
	header.scene_packet_offset = sizeof(SceneCaptureHeader);
	header.scene_packet_size = packet.size();
	header.geometry_count = p_input.geometries.size();
	header.geometry_offset = header.scene_packet_offset + header.scene_packet_size;
	header.vertex_count = vertex_count;
	header.vertex_offset = header.geometry_offset + header.geometry_count * sizeof(GeometryRecord);
	header.index_count = index_count;
	header.index_offset = header.vertex_offset + header.vertex_count * sizeof(GeometryVertex);

	r_capture.resize(total_size);
	uint8_t *capture = r_capture.ptrw();
	memset(capture, 0, total_size);
	memcpy(capture + header.scene_packet_offset, packet.ptr(), packet.size());

	uint32_t next_vertex = 0;
	uint32_t next_index = 0;
	for (uint32_t i = 0; i < header.geometry_count; i++) {
		const GeometryInput &source = p_input.geometries[i];
		GeometryRecord record = {};
		record.geometry_id = source.geometry_id;
		record.vertex_offset = next_vertex;
		record.vertex_count = source.vertices.size();
		record.index_offset = next_index;
		record.index_count = source.indices.size();
		record.flags = source.flags;
		record.bounds_min = source.bounds_min;
		record.bounds_max = source.bounds_max;
		memcpy(capture + header.geometry_offset + i * sizeof(GeometryRecord), &record, sizeof(record));
		memcpy(capture + header.vertex_offset + next_vertex * sizeof(GeometryVertex), source.vertices.ptr(), source.vertices.size() * sizeof(GeometryVertex));
		memcpy(capture + header.index_offset + next_index * sizeof(uint32_t), source.indices.ptr(), source.indices.size() * sizeof(uint32_t));
		next_vertex += source.vertices.size();
		next_index += source.indices.size();
	}
	header.payload_hash = SceneCompiler::hash_payload(capture + sizeof(SceneCaptureHeader), total_size - sizeof(SceneCaptureHeader));
	memcpy(capture, &header, sizeof(header));
	return validate(r_capture, r_error);
}

Error SceneCapture::validate(const PackedByteArray &p_capture, String *r_error) {
	SceneCaptureHeader header = {};
	if (SceneCompiler::read_record(p_capture, 0, header) != OK || header.magic != SCENE_CAPTURE_MAGIC ||
			header.capture_version != SCENE_CAPTURE_VERSION || header.endian_tag != SCENE_PACKET_ENDIAN_TAG ||
			header.header_size != sizeof(SceneCaptureHeader) || header.total_size != (uint64_t)p_capture.size()) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture header is invalid.", r_error);
	}
	if (header.payload_hash != SceneCompiler::hash_payload(p_capture.ptr() + sizeof(SceneCaptureHeader), p_capture.size() - sizeof(SceneCaptureHeader))) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture payload hash does not match.", r_error);
	}
	uint64_t expected_offset = sizeof(SceneCaptureHeader);
	if (header.scene_packet_offset != expected_offset) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture scene section is not canonical.", r_error);
	}
	expected_offset += header.scene_packet_size;
	if (header.geometry_offset != expected_offset) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture geometry section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.geometry_count * sizeof(GeometryRecord);
	if (header.vertex_offset != expected_offset) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture vertex section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.vertex_count * sizeof(GeometryVertex);
	if (header.index_offset != expected_offset) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture index section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.index_count * sizeof(uint32_t);
	if (expected_offset != header.total_size) {
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture sections do not cover the payload exactly.", r_error);
	}

	PackedByteArray packet;
	packet.resize(header.scene_packet_size);
	memcpy(packet.ptrw(), p_capture.ptr() + header.scene_packet_offset, header.scene_packet_size);
	if (SceneCompiler::validate(packet, r_error) != OK) {
		return ERR_FILE_CORRUPT;
	}
	ScenePacketHeader packet_header = {};
	SceneCompiler::read_record(packet, 0, packet_header);
	HashSet<uint64_t> geometry_ids;
	uint32_t next_vertex = 0;
	uint32_t next_index = 0;
	for (uint32_t i = 0; i < header.geometry_count; i++) {
		GeometryRecord geometry = {};
		if (SceneCompiler::read_record(p_capture, header.geometry_offset + i * sizeof(GeometryRecord), geometry) != OK ||
				geometry.geometry_id == 0 || geometry_ids.has(geometry.geometry_id) || geometry.vertex_count == 0 ||
				geometry.index_count == 0 || geometry.index_count % 3 != 0 || geometry.vertex_offset != next_vertex ||
				geometry.index_offset != next_index ||
				(uint64_t)geometry.vertex_offset + geometry.vertex_count > header.vertex_count ||
				(uint64_t)geometry.index_offset + geometry.index_count > header.index_count || !_capture_finite(geometry.bounds_min) ||
				!_capture_finite(geometry.bounds_max)) {
			return _capture_fail(ERR_FILE_CORRUPT, "A path-tracing capture geometry record is invalid.", r_error);
		}
		geometry_ids.insert(geometry.geometry_id);
		for (uint32_t j = 0; j < geometry.vertex_count; j++) {
			GeometryVertex vertex = {};
			memcpy(&vertex, p_capture.ptr() + header.vertex_offset + (geometry.vertex_offset + j) * sizeof(GeometryVertex), sizeof(vertex));
			if (!_capture_finite(vertex.current_position) || !_capture_finite(vertex.previous_position) ||
					!_capture_finite(vertex.normal) || !_capture_finite(vertex.tangent) || !_capture_finite(vertex.uv)) {
				return _capture_fail(ERR_FILE_CORRUPT, "A path-tracing capture vertex is not finite.", r_error);
			}
		}
		for (uint32_t j = 0; j < geometry.index_count; j++) {
			uint32_t index = 0;
			memcpy(&index, p_capture.ptr() + header.index_offset + (geometry.index_offset + j) * sizeof(uint32_t), sizeof(index));
			if (index >= geometry.vertex_count) {
				return _capture_fail(ERR_FILE_CORRUPT, "A path-tracing capture index is out of bounds.", r_error);
			}
		}
		next_vertex += geometry.vertex_count;
		next_index += geometry.index_count;
	}
	for (uint32_t i = 0; i < packet_header.instance_count; i++) {
		InstanceRecord instance = {};
		SceneCompiler::read_record(packet, packet_header.instance_offset + i * sizeof(InstanceRecord), instance);
		if (!geometry_ids.has(instance.geometry_id)) {
			return _capture_fail(ERR_FILE_CORRUPT, "A path-tracing capture is missing referenced geometry.", r_error);
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error SceneCapture::get_scene_packet(const PackedByteArray &p_capture, PackedByteArray &r_packet, String *r_error) {
	const Error validation_error = validate(p_capture, r_error);
	if (validation_error != OK) {
		return validation_error;
	}
	SceneCaptureHeader header = {};
	SceneCompiler::read_record(p_capture, 0, header);
	r_packet.resize(header.scene_packet_size);
	memcpy(r_packet.ptrw(), p_capture.ptr() + header.scene_packet_offset, header.scene_packet_size);
	return OK;
}

Error SceneCapture::save(const String &p_path, const PackedByteArray &p_capture, String *r_error) {
	const Error validation_error = validate(p_capture, r_error);
	if (validation_error != OK) {
		return validation_error;
	}
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &open_error);
	if (open_error != OK) {
		return _capture_fail(open_error, "The path-tracing capture could not be opened for writing.", r_error);
	}
	file->store_buffer(p_capture);
	return file->get_error();
}

Error SceneCapture::load(const String &p_path, PackedByteArray &r_capture, String *r_error) {
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (open_error != OK) {
		return _capture_fail(open_error, "The path-tracing capture could not be opened for reading.", r_error);
	}
	const uint64_t length = file->get_length();
	if (length > (uint64_t)std::numeric_limits<int>::max()) {
		return _capture_fail(ERR_OUT_OF_MEMORY, "The path-tracing capture exceeds version-1 host limits.", r_error);
	}
	r_capture = file->get_buffer(length);
	if ((uint64_t)r_capture.size() != length) {
		r_capture.clear();
		return _capture_fail(ERR_FILE_CORRUPT, "The path-tracing capture is truncated.", r_error);
	}
	const Error validation_error = validate(r_capture, r_error);
	if (validation_error != OK) {
		r_capture.clear();
	}
	return validation_error;
}

} // namespace RendererPathTracing
