/**************************************************************************/
/*  path_tracing_scene_compiler.cpp                                       */
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

#include "path_tracing_scene_compiler.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"

#include <cstring>
#include <limits>

namespace RendererPathTracing {

static bool _finite(const Float4 &p_value) {
	return Math::is_finite(p_value.x) && Math::is_finite(p_value.y) && Math::is_finite(p_value.z) && Math::is_finite(p_value.w);
}

static bool _finite(const Matrix4 &p_value) {
	for (const Float4 &column : p_value.columns) {
		if (!_finite(column)) {
			return false;
		}
	}
	return true;
}

static Error _fail(Error p_error, const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

template <typename T>
static void _write_records(uint8_t *p_packet, uint32_t p_offset, const Vector<T> &p_records) {
	if (!p_records.is_empty()) {
		memcpy(p_packet + p_offset, p_records.ptr(), p_records.size() * sizeof(T));
	}
}

uint64_t SceneCompiler::hash_payload(const uint8_t *p_data, uint64_t p_size) {
	uint64_t hash = 14695981039346656037ULL;
	for (uint64_t i = 0; i < p_size; i++) {
		hash ^= p_data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

Error SceneCompiler::compile(const ScenePacketInput &p_input, PackedByteArray &r_packet, String *r_error) {
	if (p_input.cameras.is_empty() || p_input.cameras.size() > SCENE_PACKET_MAX_VIEWS) {
		return _fail(ERR_INVALID_PARAMETER, "A path-tracing packet requires one or two cameras.", r_error);
	}

	const uint64_t total_size = sizeof(ScenePacketHeader) + sizeof(GuideContract) +
			(uint64_t)p_input.cameras.size() * sizeof(CameraRecord) +
			(uint64_t)p_input.instances.size() * sizeof(InstanceRecord) +
			(uint64_t)p_input.materials.size() * sizeof(MaterialRecord) +
			(uint64_t)p_input.lights.size() * sizeof(LightRecord);
	if (total_size > std::numeric_limits<uint32_t>::max() || total_size > (uint64_t)std::numeric_limits<int>::max()) {
		return _fail(ERR_OUT_OF_MEMORY, "The path-tracing packet exceeds schema-1 address limits.", r_error);
	}

	ScenePacketHeader header = {};
	header.magic = SCENE_PACKET_MAGIC;
	header.schema_version = SCENE_PACKET_VERSION;
	header.endian_tag = SCENE_PACKET_ENDIAN_TAG;
	header.header_size = sizeof(ScenePacketHeader);
	header.total_size = total_size;
	header.guide_contract_offset = sizeof(ScenePacketHeader);
	header.camera_count = p_input.cameras.size();
	header.camera_offset = header.guide_contract_offset + sizeof(GuideContract);
	header.instance_count = p_input.instances.size();
	header.instance_offset = header.camera_offset + header.camera_count * sizeof(CameraRecord);
	header.material_count = p_input.materials.size();
	header.material_offset = header.instance_offset + header.instance_count * sizeof(InstanceRecord);
	header.light_count = p_input.lights.size();
	header.light_offset = header.material_offset + header.material_count * sizeof(MaterialRecord);

	r_packet.resize(total_size);
	uint8_t *packet = r_packet.ptrw();
	memset(packet, 0, total_size);
	memcpy(packet + header.guide_contract_offset, &p_input.guide_contract, sizeof(GuideContract));
	_write_records(packet, header.camera_offset, p_input.cameras);
	_write_records(packet, header.instance_offset, p_input.instances);
	_write_records(packet, header.material_offset, p_input.materials);
	_write_records(packet, header.light_offset, p_input.lights);
	header.payload_hash = hash_payload(packet + sizeof(ScenePacketHeader), total_size - sizeof(ScenePacketHeader));
	memcpy(packet, &header, sizeof(header));

	const Error validation_error = validate(r_packet, r_error);
	if (validation_error != OK) {
		r_packet.clear();
	}
	return validation_error;
}

Error SceneCompiler::validate(const PackedByteArray &p_packet, String *r_error) {
	ScenePacketHeader header = {};
	if (read_record(p_packet, 0, header) != OK) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing packet header is truncated.", r_error);
	}
	if (header.magic != SCENE_PACKET_MAGIC || header.schema_version != SCENE_PACKET_VERSION ||
			header.endian_tag != SCENE_PACKET_ENDIAN_TAG || header.header_size != sizeof(ScenePacketHeader)) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing packet header contract is invalid.", r_error);
	}
	if (header.total_size != (uint64_t)p_packet.size()) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing packet size does not match its header.", r_error);
	}
	if (header.payload_hash != hash_payload(p_packet.ptr() + sizeof(ScenePacketHeader), p_packet.size() - sizeof(ScenePacketHeader))) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing packet payload hash does not match.", r_error);
	}
	if (header.camera_count == 0 || header.camera_count > SCENE_PACKET_MAX_VIEWS) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing packet has an unsupported camera count.", r_error);
	}

	uint64_t expected_offset = sizeof(ScenePacketHeader);
	if (header.guide_contract_offset != expected_offset) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing guide section is not canonical.", r_error);
	}
	expected_offset += sizeof(GuideContract);
	if (header.camera_offset != expected_offset) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing camera section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.camera_count * sizeof(CameraRecord);
	if (header.instance_offset != expected_offset) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing instance section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.instance_count * sizeof(InstanceRecord);
	if (header.material_offset != expected_offset) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing material section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.material_count * sizeof(MaterialRecord);
	if (header.light_offset != expected_offset) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing light section is not canonical.", r_error);
	}
	expected_offset += (uint64_t)header.light_count * sizeof(LightRecord);
	if (expected_offset != header.total_size) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing packet sections do not cover the payload exactly.", r_error);
	}

	GuideContract guides = {};
	if (read_record(p_packet, header.guide_contract_offset, guides) != OK) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing guide contract is truncated.", r_error);
	}
	constexpr uint32_t known_guides = (1U << 10) - 1;
	if (guides.schema_version != SCENE_PACKET_VERSION || guides.motion_direction != MOTION_CURRENT_TO_PREVIOUS ||
			guides.motion_units != MOTION_NORMALIZED_UV || guides.depth_convention != DEPTH_REVERSED_ZERO_TO_ONE ||
			guides.normal_space != NORMAL_WORLD_SPACE || guides.roughness_convention != ROUGHNESS_PERCEPTUAL_ZERO_TO_ONE ||
			guides.invalid_pixel_convention != INVALID_PIXEL_QUIET_NAN || guides.uv_origin != UV_ORIGIN_TOP_LEFT ||
			guides.color_space != COLOR_LINEAR_REC709 || (guides.enabled_guides & ~known_guides) != 0 ||
			!Math::is_finite(guides.motion_to_pixel_scale_x) || !Math::is_finite(guides.motion_to_pixel_scale_y) ||
			guides.motion_to_pixel_scale_x <= 0.0f || guides.motion_to_pixel_scale_y <= 0.0f) {
		return _fail(ERR_FILE_CORRUPT, "The path-tracing guide semantics are invalid.", r_error);
	}

	bool seen_views[SCENE_PACKET_MAX_VIEWS] = {};
	for (uint32_t i = 0; i < header.camera_count; i++) {
		CameraRecord camera = {};
		if (read_record(p_packet, header.camera_offset + i * sizeof(CameraRecord), camera) != OK ||
				camera.view_index >= header.camera_count || seen_views[camera.view_index] || camera.render_width == 0 ||
				camera.render_height == 0 || camera.history_reset > 1 || !_finite(camera.view_from_world) ||
				!_finite(camera.clip_from_view) || !_finite(camera.previous_view_from_world) ||
				!_finite(camera.previous_clip_from_view) || !_finite(camera.camera_relative_origin_and_exposure)) {
			return _fail(ERR_FILE_CORRUPT, "A path-tracing camera record is invalid.", r_error);
		}
		seen_views[camera.view_index] = true;
	}

	for (uint32_t i = 0; i < header.instance_count; i++) {
		InstanceRecord instance = {};
		if (read_record(p_packet, header.instance_offset + i * sizeof(InstanceRecord), instance) != OK ||
				!_finite(instance.world_from_object) || !_finite(instance.previous_world_from_object) || instance.geometry_id == 0 ||
				instance.material_id > header.material_count) {
			return _fail(ERR_FILE_CORRUPT, "A path-tracing instance record is invalid.", r_error);
		}
	}

	for (uint32_t i = 0; i < header.material_count; i++) {
		MaterialRecord material = {};
		if (read_record(p_packet, header.material_offset + i * sizeof(MaterialRecord), material) != OK ||
				!_finite(material.base_color_and_opacity) || !_finite(material.emission_and_strength) ||
				!_finite(material.specular_f0_and_perceptual_roughness) || !_finite(material.transmission_ior_alpha_cutoff_unused) ||
				material.specular_f0_and_perceptual_roughness.w < 0.0f || material.specular_f0_and_perceptual_roughness.w > 1.0f) {
			return _fail(ERR_FILE_CORRUPT, "A path-tracing material record is invalid.", r_error);
		}
	}

	for (uint32_t i = 0; i < header.light_count; i++) {
		LightRecord light = {};
		if (read_record(p_packet, header.light_offset + i * sizeof(LightRecord), light) != OK ||
				!_finite(light.position_or_direction_and_type) || !_finite(light.linear_color_and_intensity) || !_finite(light.shape_parameters)) {
			return _fail(ERR_FILE_CORRUPT, "A path-tracing light record is invalid.", r_error);
		}
	}

	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error SceneCompiler::save_packet(const String &p_path, const PackedByteArray &p_packet, String *r_error) {
	const Error validation_error = validate(p_packet, r_error);
	if (validation_error != OK) {
		return validation_error;
	}
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &open_error);
	if (open_error != OK) {
		return _fail(open_error, "The path-tracing capture could not be opened for writing.", r_error);
	}
	file->store_buffer(p_packet);
	if (file->get_error() != OK) {
		return _fail(file->get_error(), "The path-tracing capture could not be written completely.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error SceneCompiler::load_packet(const String &p_path, PackedByteArray &r_packet, String *r_error) {
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (open_error != OK) {
		return _fail(open_error, "The path-tracing capture could not be opened for reading.", r_error);
	}
	const uint64_t length = file->get_length();
	if (length > (uint64_t)std::numeric_limits<int>::max()) {
		return _fail(ERR_OUT_OF_MEMORY, "The path-tracing capture exceeds schema-1 host limits.", r_error);
	}
	r_packet = file->get_buffer(length);
	if ((uint64_t)r_packet.size() != length) {
		r_packet.clear();
		return _fail(ERR_FILE_CORRUPT, "The path-tracing capture is truncated.", r_error);
	}
	const Error validation_error = validate(r_packet, r_error);
	if (validation_error != OK) {
		r_packet.clear();
	}
	return validation_error;
}

} // namespace RendererPathTracing
