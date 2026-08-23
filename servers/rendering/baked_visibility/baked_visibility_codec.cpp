/**************************************************************************/
/*  baked_visibility_codec.cpp                                            */
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

#include "baked_visibility_codec.h"

#include "core/io/marshalls.h"
#include "core/io/resource_uid.h"
#include "core/math/math_funcs.h"
#include "core/templates/sort_array.h"

namespace {

static constexpr uint8_t MAGIC[] = { 'B', 'V', 'I', 'S' };
static constexpr uint32_t HEADER_BYTES = 4 + 4 + 4 + 4 + 4;

struct ByteWriter {
	PackedByteArray bytes;

	void u8(uint8_t p_value) { bytes.push_back(p_value); }
	void u32(uint32_t p_value) {
		const int offset = bytes.size();
		bytes.resize(offset + 4);
		encode_uint32(p_value, bytes.ptrw() + offset);
	}
	void u64(uint64_t p_value) {
		const int offset = bytes.size();
		bytes.resize(offset + 8);
		encode_uint64(p_value, bytes.ptrw() + offset);
	}
	void f32(float p_value) {
		const int offset = bytes.size();
		bytes.resize(offset + 4);
		encode_float(p_value, bytes.ptrw() + offset);
	}
	void f64(double p_value) {
		const int offset = bytes.size();
		bytes.resize(offset + 8);
		encode_double(p_value, bytes.ptrw() + offset);
	}
	void uleb(uint32_t p_value) {
		while (p_value >= 0x80) {
			u8(uint8_t(p_value) | 0x80);
			p_value >>= 7;
		}
		u8(uint8_t(p_value));
	}
	void string(const String &p_value) {
		const CharString utf8 = p_value.utf8();
		uleb(utf8.length());
		for (int i = 0; i < utf8.length(); i++) {
			u8(uint8_t(utf8[i]));
		}
	}
	void aabb(const AABB &p_value) {
		f32(p_value.position.x);
		f32(p_value.position.y);
		f32(p_value.position.z);
		f32(p_value.size.x);
		f32(p_value.size.y);
		f32(p_value.size.z);
	}
};

struct ByteReader {
	const PackedByteArray &bytes;
	uint64_t offset = 0;

	bool can_read(uint64_t p_count) const { return p_count <= uint64_t(bytes.size()) - offset; }
	bool u8(uint8_t &r_value) {
		if (!can_read(1)) {
			return false;
		}
		r_value = bytes[offset++];
		return true;
	}
	bool u32(uint32_t &r_value) {
		if (!can_read(4)) {
			return false;
		}
		r_value = decode_uint32(bytes.ptr() + offset);
		offset += 4;
		return true;
	}
	bool u64(uint64_t &r_value) {
		if (!can_read(8)) {
			return false;
		}
		r_value = decode_uint64(bytes.ptr() + offset);
		offset += 8;
		return true;
	}
	bool f32(float &r_value) {
		if (!can_read(4)) {
			return false;
		}
		r_value = decode_float(bytes.ptr() + offset);
		offset += 4;
		return true;
	}
	bool f64(double &r_value) {
		if (!can_read(8)) {
			return false;
		}
		r_value = decode_double(bytes.ptr() + offset);
		offset += 8;
		return true;
	}
	bool uleb(uint32_t &r_value) {
		r_value = 0;
		for (uint32_t shift = 0; shift < 35; shift += 7) {
			uint8_t byte = 0;
			if (!u8(byte)) {
				return false;
			}
			if (shift == 28 && byte > 0x0f) {
				return false;
			}
			r_value |= uint32_t(byte & 0x7f) << shift;
			if (!(byte & 0x80)) {
				return true;
			}
		}
		return false;
	}
	bool string(String &r_value) {
		uint32_t length = 0;
		if (!uleb(length) || !can_read(length)) {
			return false;
		}
		r_value = String::utf8((const char *)(bytes.ptr() + offset), length);
		offset += length;
		return true;
	}
	bool aabb(AABB &r_value) {
		return f32(r_value.position.x) && f32(r_value.position.y) && f32(r_value.position.z) && f32(r_value.size.x) && f32(r_value.size.y) && f32(r_value.size.z);
	}
};

static uint32_t crc32(const uint8_t *p_bytes, uint64_t p_size) {
	uint32_t crc = 0xffffffff;
	for (uint64_t i = 0; i < p_size; i++) {
		crc ^= p_bytes[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-(int32_t(crc & 1))));
		}
	}
	return ~crc;
}

static bool is_finite_aabb(const AABB &p_aabb) {
	return Math::is_finite(p_aabb.position.x) && Math::is_finite(p_aabb.position.y) && Math::is_finite(p_aabb.position.z) && Math::is_finite(p_aabb.size.x) && Math::is_finite(p_aabb.size.y) && Math::is_finite(p_aabb.size.z) && p_aabb.size.x >= 0.0f && p_aabb.size.y >= 0.0f && p_aabb.size.z >= 0.0f;
}

static bool is_finite_vec3(const Vector3 &p_value) {
	return Math::is_finite(p_value.x) && Math::is_finite(p_value.y) && Math::is_finite(p_value.z);
}

static bool is_valid_relative_node_path(const String &p_path) {
	if (p_path.is_empty() || p_path.begins_with("/") || p_path.begins_with("\\") || p_path.contains("://") || p_path.contains("\\") || p_path.contains(":")) {
		return false;
	}
	const PackedStringArray components = p_path.split("/", false);
	if (components.is_empty()) {
		return false;
	}
	for (const String &component : components) {
		if (component.is_empty() || component == "." || component == "..") {
			return false;
		}
	}
	return true;
}

static bool is_valid_resource_path(const String &p_path) {
	if (!p_path.begins_with("res://")) {
		return false;
	}
	return is_valid_relative_node_path(p_path.trim_prefix("res://"));
}

static bool less_set(const Vector<uint32_t> &p_a, const Vector<uint32_t> &p_b) {
	const int common = MIN(p_a.size(), p_b.size());
	for (int i = 0; i < common; i++) {
		if (p_a[i] != p_b[i]) {
			return p_a[i] < p_b[i];
		}
	}
	return p_a.size() < p_b.size();
}

static bool equal_set(const Vector<uint32_t> &p_a, const Vector<uint32_t> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (int i = 0; i < p_a.size(); i++) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

struct InstanceIndexComparator {
	const Vector<BakedVisibilityData3DData::Instance> *instances = nullptr;
	bool operator()(int p_a, int p_b) const {
		return String((*instances)[p_a].path) < String((*instances)[p_b].path);
	}
};

struct SetIndexComparator {
	const Vector<Vector<uint32_t>> *sets = nullptr;
	bool operator()(int p_a, int p_b) const {
		return less_set((*sets)[p_a], (*sets)[p_b]);
	}
};

static void set_error(String *r_error, const String &p_error) {
	if (r_error) {
		*r_error = p_error;
	}
}

} // namespace

Error BakedVisibilityCodec::validate(const BakedVisibilityData3DData &p_data, String *r_error) {
	if (p_data.source_uid == 0 || p_data.source_uid == ResourceUID::INVALID_ID) {
		set_error(r_error, "Baked visibility source UID is invalid.");
		return ERR_INVALID_DATA;
	}
	if (!is_valid_resource_path(p_data.source_path)) {
		set_error(r_error, "Baked visibility source path must be a nonempty res:// path.");
		return ERR_INVALID_DATA;
	}
	if (p_data.source_sha256.size() != 32) {
		set_error(r_error, "Baked visibility source digest must be SHA-256.");
		return ERR_INVALID_DATA;
	}
	if (!is_finite_aabb(p_data.local_bounds) || !is_finite_vec3(p_data.cell_size) || p_data.cell_size.x <= 0.0f || p_data.cell_size.y <= 0.0f || p_data.cell_size.z <= 0.0f) {
		set_error(r_error, "Baked visibility bounds or cell size is invalid.");
		return ERR_INVALID_DATA;
	}
	if (p_data.grid_size.x <= 0 || p_data.grid_size.y <= 0 || p_data.grid_size.z <= 0) {
		set_error(r_error, "Baked visibility grid dimensions must be positive.");
		return ERR_INVALID_DATA;
	}
	const uint64_t grid_cells = uint64_t(p_data.grid_size.x) * uint64_t(p_data.grid_size.y) * uint64_t(p_data.grid_size.z);
	if (grid_cells > BakedVisibilityData3DData::MAX_CELLS || grid_cells != uint64_t(p_data.cells.size())) {
		set_error(r_error, "Baked visibility cell count does not match its bounded grid.");
		return ERR_INVALID_DATA;
	}
	if (!Math::is_finite(p_data.transport_distance) || !Math::is_finite(p_data.lookup_margin) || !Math::is_finite(p_data.coverage_tolerance) || !Math::is_finite(p_data.transport_tolerance) || p_data.transport_distance < -1.0f || p_data.lookup_margin < 0.0f || p_data.coverage_tolerance < 0.0f || p_data.transport_tolerance < 0.0f) {
		set_error(r_error, "Baked visibility transport settings are invalid.");
		return ERR_INVALID_DATA;
	}
	if (p_data.sets.is_empty()) {
		set_error(r_error, "Baked visibility data requires an empty interned set.");
		return ERR_INVALID_DATA;
	}
	String previous_path;
	for (int i = 0; i < p_data.instances.size(); i++) {
		const BakedVisibilityData3DData::Instance &instance = p_data.instances[i];
		const String path = String(instance.path);
		if (!is_valid_relative_node_path(path) || (i > 0 && path <= previous_path) || instance.kind > BakedVisibilityData3DData::INSTANCE_KIND_DIRECTIONAL_LIGHT || (instance.flags & ~BakedVisibilityData3DData::INSTANCE_FLAGS_MASK) != 0 || instance.signature_sha256.size() != 32 || !is_finite_aabb(instance.local_bounds)) {
			set_error(r_error, "Baked visibility instance has an invalid relative path or bounds.");
			return ERR_INVALID_DATA;
		}
		previous_path = path;
	}
	for (int i = 0; i < p_data.sets.size(); i++) {
		if (i > 0 && !less_set(p_data.sets[i - 1], p_data.sets[i])) {
			set_error(r_error, "Baked visibility interned sets are not unique and lexically sorted.");
			return ERR_INVALID_DATA;
		}
		uint32_t previous = 0;
		for (int j = 0; j < p_data.sets[i].size(); j++) {
			const uint32_t index = p_data.sets[i][j];
			if (index >= uint32_t(p_data.instances.size()) || (j > 0 && index <= previous)) {
				set_error(r_error, "Baked visibility interned set is out of range or not strictly sorted.");
				return ERR_INVALID_DATA;
			}
			previous = index;
		}
	}
	for (int i = 0; i < p_data.cells.size(); i++) {
		if ((p_data.cells[i].flags & ~BakedVisibilityData3DData::CELL_FLAGS_MASK) != 0 || p_data.cells[i].primary_set >= uint32_t(p_data.sets.size()) || p_data.cells[i].transport_set >= uint32_t(p_data.sets.size())) {
			set_error(r_error, "Baked visibility cell references an invalid interned set.");
			return ERR_INVALID_DATA;
		}
	}
	for (double stage_time : p_data.stage_times_ms) {
		if (!Math::is_finite(stage_time) || stage_time < 0.0) {
			set_error(r_error, "Baked visibility stage timing is invalid.");
			return ERR_INVALID_DATA;
		}
	}
	return OK;
}

Error BakedVisibilityCodec::encode(const BakedVisibilityData3DData &p_data, PackedByteArray &r_bytes, String *r_error) {
	BakedVisibilityData3DData data = p_data;
	if (data.sets.is_empty()) {
		data.sets.push_back(Vector<uint32_t>());
	}

	Vector<int> sorted_instances;
	sorted_instances.resize(data.instances.size());
	for (int i = 0; i < sorted_instances.size(); i++) {
		sorted_instances.write[i] = i;
	}
	sorted_instances.sort_custom<InstanceIndexComparator>(InstanceIndexComparator { &data.instances });
	Vector<uint32_t> old_to_new;
	old_to_new.resize(data.instances.size());
	Vector<BakedVisibilityData3DData::Instance> canonical_instances;
	canonical_instances.resize(data.instances.size());
	for (int i = 0; i < sorted_instances.size(); i++) {
		old_to_new.write[sorted_instances[i]] = i;
		canonical_instances.write[i] = data.instances[sorted_instances[i]];
	}
	data.instances = canonical_instances;
	for (int i = 0; i < data.sets.size(); i++) {
		for (int j = 0; j < data.sets[i].size(); j++) {
			if (data.sets[i][j] >= uint32_t(old_to_new.size())) {
				set_error(r_error, "Baked visibility set references a missing instance.");
				return ERR_INVALID_DATA;
			}
			data.sets.write[i].write[j] = old_to_new[data.sets[i][j]];
		}
		data.sets.write[i].sort();
		for (int j = data.sets[i].size() - 1; j > 0; j--) {
			if (data.sets[i][j] == data.sets[i][j - 1]) {
				data.sets.write[i].remove_at(j);
			}
		}
	}
	Vector<int> sorted_sets;
	sorted_sets.resize(data.sets.size());
	for (int i = 0; i < sorted_sets.size(); i++) {
		sorted_sets.write[i] = i;
	}
	sorted_sets.sort_custom<SetIndexComparator>(SetIndexComparator { &data.sets });
	Vector<uint32_t> old_set_to_new;
	old_set_to_new.resize(data.sets.size());
	Vector<Vector<uint32_t>> canonical_sets;
	for (int i = 0; i < sorted_sets.size(); i++) {
		const int original = sorted_sets[i];
		if (canonical_sets.is_empty() || !equal_set(canonical_sets[canonical_sets.size() - 1], data.sets[original])) {
			canonical_sets.push_back(data.sets[original]);
		}
		old_set_to_new.write[original] = canonical_sets.size() - 1;
	}
	for (int i = 0; i < data.cells.size(); i++) {
		if (data.cells[i].primary_set >= uint32_t(old_set_to_new.size()) || data.cells[i].transport_set >= uint32_t(old_set_to_new.size())) {
			set_error(r_error, "Baked visibility cell references a missing set.");
			return ERR_INVALID_DATA;
		}
		data.cells.write[i].primary_set = old_set_to_new[data.cells[i].primary_set];
		data.cells.write[i].transport_set = old_set_to_new[data.cells[i].transport_set];
	}
	data.sets = canonical_sets;
	if (Error err = validate(data, r_error); err != OK) {
		return err;
	}

	ByteWriter payload;
	payload.u64(data.source_uid);
	payload.string(data.source_path);
	for (int i = 0; i < data.source_sha256.size(); i++) {
		payload.u8(data.source_sha256[i]);
	}
	payload.aabb(data.local_bounds);
	payload.f32(data.cell_size.x);
	payload.f32(data.cell_size.y);
	payload.f32(data.cell_size.z);
	payload.uleb(data.grid_size.x);
	payload.uleb(data.grid_size.y);
	payload.uleb(data.grid_size.z);
	payload.u32(data.bake_mask);
	payload.f32(data.transport_distance);
	payload.f32(data.lookup_margin);
	payload.f32(data.coverage_tolerance);
	payload.f32(data.transport_tolerance);
	payload.uleb(data.instances.size());
	for (int i = 0; i < data.instances.size(); i++) {
		payload.string(String(data.instances[i].path));
		payload.u8(data.instances[i].kind);
		payload.u32(data.instances[i].flags);
		payload.aabb(data.instances[i].local_bounds);
		for (int j = 0; j < data.instances[i].signature_sha256.size(); j++) {
			payload.u8(data.instances[i].signature_sha256[j]);
		}
	}
	payload.uleb(data.sets.size());
	for (int i = 0; i < data.sets.size(); i++) {
		payload.uleb(data.sets[i].size());
		uint32_t previous = 0;
		for (int j = 0; j < data.sets[i].size(); j++) {
			payload.uleb(data.sets[i][j] - previous);
			previous = data.sets[i][j];
		}
	}
	for (int i = 0; i < data.cells.size(); i++) {
		payload.u32(data.cells[i].flags);
		payload.uleb(data.cells[i].primary_set);
		payload.uleb(data.cells[i].transport_set);
	}
	payload.string(data.report);
	payload.uleb(data.stage_times_ms.size());
	for (int i = 0; i < data.stage_times_ms.size(); i++) {
		payload.f64(data.stage_times_ms[i]);
	}
	if (payload.bytes.size() > BakedVisibilityData3DData::MAX_SERIALIZED_BYTES - int(HEADER_BYTES)) {
		set_error(r_error, "Baked visibility payload exceeds the configured output limit.");
		return ERR_OUT_OF_MEMORY;
	}

	r_bytes.resize(HEADER_BYTES + payload.bytes.size());
	uint8_t *write = r_bytes.ptrw();
	for (int i = 0; i < 4; i++) {
		write[i] = MAGIC[i];
	}
	encode_uint32(BakedVisibilityData3DData::FORMAT_VERSION, write + 4);
	encode_uint32(BakedVisibilityData3DData::ALGORITHM_VERSION, write + 8);
	encode_uint32(payload.bytes.size(), write + 12);
	encode_uint32(crc32(payload.bytes.ptr(), payload.bytes.size()), write + 16);
	memcpy(write + HEADER_BYTES, payload.bytes.ptr(), payload.bytes.size());
	return OK;
}

Error BakedVisibilityCodec::decode(const PackedByteArray &p_bytes, BakedVisibilityData3DData &r_data, String *r_error) {
	if (p_bytes.size() < int(HEADER_BYTES) || p_bytes.size() > int(BakedVisibilityData3DData::MAX_SERIALIZED_BYTES)) {
		set_error(r_error, "Baked visibility payload has an invalid size.");
		return ERR_FILE_CORRUPT;
	}
	const uint8_t *read = p_bytes.ptr();
	if (memcmp(read, MAGIC, sizeof(MAGIC)) != 0 || decode_uint32(read + 4) != BakedVisibilityData3DData::FORMAT_VERSION || decode_uint32(read + 8) != BakedVisibilityData3DData::ALGORITHM_VERSION) {
		set_error(r_error, "Baked visibility payload has an unsupported magic or version.");
		return ERR_FILE_UNRECOGNIZED;
	}
	const uint32_t payload_size = decode_uint32(read + 12);
	if (payload_size != uint32_t(p_bytes.size() - HEADER_BYTES) || decode_uint32(read + 16) != crc32(read + HEADER_BYTES, payload_size)) {
		set_error(r_error, "Baked visibility payload failed its integrity check.");
		return ERR_FILE_CORRUPT;
	}

	ByteReader reader { p_bytes, HEADER_BYTES };
	BakedVisibilityData3DData data;
	if (!reader.u64(data.source_uid) || !reader.string(data.source_path) || !reader.can_read(32)) {
		set_error(r_error, "Baked visibility payload is truncated.");
		return ERR_FILE_CORRUPT;
	}
	data.source_sha256.resize(32);
	for (int i = 0; i < 32; i++) {
		data.source_sha256.write[i] = p_bytes[reader.offset++];
	}
	uint32_t grid_x = 0;
	uint32_t grid_y = 0;
	uint32_t grid_z = 0;
	if (!reader.aabb(data.local_bounds) || !reader.f32(data.cell_size.x) || !reader.f32(data.cell_size.y) || !reader.f32(data.cell_size.z) || !reader.uleb(grid_x) || !reader.uleb(grid_y) || !reader.uleb(grid_z) || !reader.u32(data.bake_mask) || !reader.f32(data.transport_distance) || !reader.f32(data.lookup_margin) || !reader.f32(data.coverage_tolerance) || !reader.f32(data.transport_tolerance)) {
		set_error(r_error, "Baked visibility header is truncated.");
		return ERR_FILE_CORRUPT;
	}
	if (grid_x > INT32_MAX || grid_y > INT32_MAX || grid_z > INT32_MAX) {
		set_error(r_error, "Baked visibility grid dimensions overflow.");
		return ERR_FILE_CORRUPT;
	}
	data.grid_size = Vector3i(grid_x, grid_y, grid_z);
	uint32_t count = 0;
	if (!reader.uleb(count) || count > BakedVisibilityData3DData::MAX_CELLS * 1024u || uint64_t(count) > (uint64_t(p_bytes.size()) - reader.offset) / 62) {
		set_error(r_error, "Baked visibility instance count is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.instances.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		String path;
		if (!reader.string(path) || !reader.u8(data.instances.write[i].kind) || !reader.u32(data.instances.write[i].flags) || !reader.aabb(data.instances.write[i].local_bounds) || !reader.can_read(32)) {
			set_error(r_error, "Baked visibility instance table is truncated.");
			return ERR_FILE_CORRUPT;
		}
		data.instances.write[i].path = NodePath(path);
		data.instances.write[i].signature_sha256.resize(32);
		for (int j = 0; j < 32; j++) {
			data.instances.write[i].signature_sha256.write[j] = p_bytes[reader.offset++];
		}
	}
	if (!reader.uleb(count) || count == 0 || count > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility interned set count is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.sets.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t member_count = 0;
		if (!reader.uleb(member_count) || member_count > uint32_t(data.instances.size())) {
			set_error(r_error, "Baked visibility interned set length is invalid.");
			return ERR_FILE_CORRUPT;
		}
		data.sets.write[i].resize(member_count);
		uint32_t previous = 0;
		for (uint32_t j = 0; j < member_count; j++) {
			uint32_t delta = 0;
			if (!reader.uleb(delta) || (j > 0 && delta == 0) || delta > UINT32_MAX - previous) {
				set_error(r_error, "Baked visibility interned set delta is invalid.");
				return ERR_FILE_CORRUPT;
			}
			previous += delta;
			data.sets.write[i].write[j] = previous;
		}
	}
	const uint64_t cell_count = uint64_t(data.grid_size.x) * uint64_t(data.grid_size.y) * uint64_t(data.grid_size.z);
	if (cell_count > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility cell count is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.cells.resize(cell_count);
	for (uint64_t i = 0; i < cell_count; i++) {
		if (!reader.u32(data.cells.write[i].flags) || !reader.uleb(data.cells.write[i].primary_set) || !reader.uleb(data.cells.write[i].transport_set)) {
			set_error(r_error, "Baked visibility cell table is truncated.");
			return ERR_FILE_CORRUPT;
		}
	}
	if (!reader.string(data.report) || !reader.uleb(count) || count > 1024) {
		set_error(r_error, "Baked visibility report is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.stage_times_ms.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		if (!reader.f64(data.stage_times_ms.write[i]) || !Math::is_finite(data.stage_times_ms[i]) || data.stage_times_ms[i] < 0.0) {
			set_error(r_error, "Baked visibility stage time is invalid.");
			return ERR_FILE_CORRUPT;
		}
	}
	if (reader.offset != uint64_t(p_bytes.size())) {
		set_error(r_error, "Baked visibility payload has trailing data.");
		return ERR_FILE_CORRUPT;
	}
	if (Error err = validate(data, r_error); err != OK) {
		return ERR_FILE_CORRUPT;
	}
	r_data = data;
	return OK;
}
