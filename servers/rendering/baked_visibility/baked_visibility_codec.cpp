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
				if (shift != 0 && (byte & 0x7f) == 0) {
					return false;
				}
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

static uint64_t tile_count_for_grid(const Vector3i &p_grid) {
	return uint64_t(p_grid.x) * uint64_t(p_grid.y) * uint64_t(p_grid.z);
}

} // namespace

Error BakedVisibilityCodec::build_tile_hierarchy(const Vector3i &p_grid_size, Vector3i &r_tile_grid_size, uint32_t &r_hierarchy_depth, Vector<BakedVisibilityData3DData::Tile> &r_tiles, Vector<uint32_t> &r_tile_cell_indices, String *r_error) {
	if (p_grid_size.x <= 0 || p_grid_size.y <= 0 || p_grid_size.z <= 0) {
		set_error(r_error, "Baked visibility tile hierarchy requires positive grid dimensions.");
		return ERR_INVALID_PARAMETER;
	}
	r_tile_grid_size = Vector3i((p_grid_size.x + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE), (p_grid_size.y + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE), (p_grid_size.z + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE));
	const uint64_t leaf_count = tile_count_for_grid(r_tile_grid_size);
	if (leaf_count == 0 || leaf_count > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility leaf tile layout exceeds its bound.");
		return ERR_OUT_OF_MEMORY;
	}

	r_tiles.clear();
	const uint64_t total_cells = uint64_t(p_grid_size.x) * uint64_t(p_grid_size.y) * uint64_t(p_grid_size.z);
	if (total_cells == 0 || total_cells > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility cell index pool exceeds its bound.");
		return ERR_OUT_OF_MEMORY;
	}
	r_tile_cell_indices.resize(int(total_cells));
	uint32_t cell_index_cursor = 0;
	r_tiles.resize(leaf_count);
	for (int tile_index = 0; tile_index < int(leaf_count); tile_index++) {
		const int tx = tile_index % r_tile_grid_size.x;
		const int ty = (tile_index / r_tile_grid_size.x) % r_tile_grid_size.y;
		const int tz = tile_index / (r_tile_grid_size.x * r_tile_grid_size.y);
		BakedVisibilityData3DData::Tile &tile = r_tiles.write[tile_index];
		tile.coordinate = Vector3i(tx, ty, tz);
		tile.level = 0;
		tile.flags = BakedVisibilityData3DData::Tile::FLAG_LEAF;
		tile.first_cell = cell_index_cursor;
		for (int z = tz * int(BakedVisibilityData3DData::TILE_SIZE); z < MIN(p_grid_size.z, (tz + 1) * int(BakedVisibilityData3DData::TILE_SIZE)); z++) {
			for (int y = ty * int(BakedVisibilityData3DData::TILE_SIZE); y < MIN(p_grid_size.y, (ty + 1) * int(BakedVisibilityData3DData::TILE_SIZE)); y++) {
				for (int x = tx * int(BakedVisibilityData3DData::TILE_SIZE); x < MIN(p_grid_size.x, (tx + 1) * int(BakedVisibilityData3DData::TILE_SIZE)); x++) {
					r_tile_cell_indices.write[cell_index_cursor++] = x + p_grid_size.x * (y + p_grid_size.y * z);
				}
			}
		}
		tile.cell_count = cell_index_cursor - tile.first_cell;
		tile.dependency_signature.resize(32);
		for (int signature_byte = 0; signature_byte < tile.dependency_signature.size(); signature_byte++) {
			tile.dependency_signature.write[signature_byte] = 0;
		}
	}
	if (cell_index_cursor != uint32_t(total_cells)) {
		set_error(r_error, "Baked visibility cell index pool does not cover its grid.");
		return ERR_INVALID_DATA;
	}

	Vector3i previous_grid = r_tile_grid_size;
	int previous_first = 0;
	uint32_t level = 0;
	while (tile_count_for_grid(previous_grid) > 1) {
		const Vector3i parent_grid((previous_grid.x + 1) / 2, (previous_grid.y + 1) / 2, (previous_grid.z + 1) / 2);
		const uint64_t parent_count = tile_count_for_grid(parent_grid);
		if (uint64_t(r_tiles.size()) + parent_count > uint64_t(BakedVisibilityData3DData::MAX_CELLS) * 2u) {
			set_error(r_error, "Baked visibility parent tile layout exceeds its bound.");
			return ERR_OUT_OF_MEMORY;
		}
		const int parent_first = r_tiles.size();
		r_tiles.resize(parent_first + parent_count);
		for (int parent_index = 0; parent_index < int(parent_count); parent_index++) {
			const int x = parent_index % parent_grid.x;
			const int y = (parent_index / parent_grid.x) % parent_grid.y;
			const int z = parent_index / (parent_grid.x * parent_grid.y);
			BakedVisibilityData3DData::Tile &parent = r_tiles.write[parent_first + parent_index];
			parent.coordinate = Vector3i(x, y, z);
			parent.level = level + 1;
			parent.dependency_signature.resize(32);
			for (int signature_byte = 0; signature_byte < parent.dependency_signature.size(); signature_byte++) {
				parent.dependency_signature.write[signature_byte] = 0;
			}
		}
		const int previous_count = int(tile_count_for_grid(previous_grid));
		for (int index = 0; index < previous_count; index++) {
			BakedVisibilityData3DData::Tile &child = r_tiles.write[previous_first + index];
			child.parent = parent_first + (child.coordinate.x / 2) + parent_grid.x * ((child.coordinate.y / 2) + parent_grid.y * (child.coordinate.z / 2));
		}
		previous_grid = parent_grid;
		previous_first = parent_first;
		level++;
	}
	r_hierarchy_depth = level + 1;
	return OK;
}

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
	if (p_data.grid_size.x > int(BakedVisibilityData3DData::MAX_CELLS) || p_data.grid_size.y > int(BakedVisibilityData3DData::MAX_CELLS) || p_data.grid_size.z > int(BakedVisibilityData3DData::MAX_CELLS) || p_data.tile_grid_size.x <= 0 || p_data.tile_grid_size.y <= 0 || p_data.tile_grid_size.z <= 0 || p_data.tile_grid_size.x > int(BakedVisibilityData3DData::MAX_CELLS) || p_data.tile_grid_size.y > int(BakedVisibilityData3DData::MAX_CELLS) || p_data.tile_grid_size.z > int(BakedVisibilityData3DData::MAX_CELLS) || p_data.tile_size != Vector3i(BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE) || p_data.tile_grid_size != Vector3i((p_data.grid_size.x + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE), (p_data.grid_size.y + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE), (p_data.grid_size.z + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE))) {
		set_error(r_error, "Baked visibility tile layout is invalid.");
		return ERR_INVALID_DATA;
	}
	const uint64_t grid_cells = uint64_t(p_data.grid_size.x) * uint64_t(p_data.grid_size.y) * uint64_t(p_data.grid_size.z);
	if (grid_cells > BakedVisibilityData3DData::MAX_CELLS || grid_cells != uint64_t(p_data.cells.size())) {
		set_error(r_error, "Baked visibility cell count does not match its bounded grid.");
		return ERR_INVALID_DATA;
	}
	Vector3i expected_tile_grid;
	uint32_t expected_depth = 0;
	Vector<BakedVisibilityData3DData::Tile> expected_tiles;
	Vector<uint32_t> expected_tile_cell_indices;
	if (build_tile_hierarchy(p_data.grid_size, expected_tile_grid, expected_depth, expected_tiles, expected_tile_cell_indices, r_error) != OK || expected_tile_grid != p_data.tile_grid_size || expected_depth != p_data.hierarchy_depth || expected_tiles.size() != p_data.tiles.size()) {
		set_error(r_error, "Baked visibility tile count does not match its bounded layout.");
		return ERR_INVALID_DATA;
	}
	for (int i = 0; i < p_data.tiles.size(); i++) {
		const BakedVisibilityData3DData::Tile &tile = p_data.tiles[i];
		const BakedVisibilityData3DData::Tile &expected = expected_tiles[i];
		if (tile.coordinate != expected.coordinate || tile.parent != expected.parent || tile.level != expected.level || tile.flags != expected.flags || tile.first_cell != expected.first_cell || tile.cell_count != expected.cell_count || tile.dependency_signature.size() != 32) {
			set_error(r_error, "Baked visibility tile certificate is invalid.");
			return ERR_INVALID_DATA;
		}
	}
	if (p_data.tile_cell_indices != expected_tile_cell_indices) {
		set_error(r_error, "Baked visibility tile cell index pool exceeds its bound.");
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

static bool _validate_metadata(const BakedVisibilityData3DData &p_data, String *r_error) {
	if (p_data.source_uid == 0 || p_data.source_uid == ResourceUID::INVALID_ID || !is_valid_resource_path(p_data.source_path) || p_data.source_sha256.size() != 32 || !is_finite_aabb(p_data.local_bounds) || !is_finite_vec3(p_data.cell_size) || p_data.cell_size.x <= 0.0f || p_data.cell_size.y <= 0.0f || p_data.cell_size.z <= 0.0f) {
		set_error(r_error, "Baked visibility metadata has an invalid source, bounds, or cell size.");
		return false;
	}
	if (p_data.grid_size.x <= 0 || p_data.grid_size.y <= 0 || p_data.grid_size.z <= 0 || uint64_t(p_data.grid_size.x) * uint64_t(p_data.grid_size.y) * uint64_t(p_data.grid_size.z) > BakedVisibilityData3DData::MAX_CELLS || p_data.tile_size != Vector3i(BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE)) {
		set_error(r_error, "Baked visibility metadata has an invalid bounded grid.");
		return false;
	}
	const Vector3i expected_leaf_grid((p_data.grid_size.x + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE), (p_data.grid_size.y + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE), (p_data.grid_size.z + int(BakedVisibilityData3DData::TILE_SIZE) - 1) / int(BakedVisibilityData3DData::TILE_SIZE));
	if (p_data.tile_grid_size != expected_leaf_grid || p_data.tiles.is_empty() || !Math::is_finite(p_data.transport_distance) || !Math::is_finite(p_data.lookup_margin) || !Math::is_finite(p_data.coverage_tolerance) || !Math::is_finite(p_data.transport_tolerance) || p_data.transport_distance < -1.0f || p_data.lookup_margin < 0.0f || p_data.coverage_tolerance < 0.0f || p_data.transport_tolerance < 0.0f) {
		set_error(r_error, "Baked visibility metadata has invalid tile or transport fields.");
		return false;
	}
	String previous_path;
	for (int i = 0; i < p_data.instances.size(); i++) {
		const BakedVisibilityData3DData::Instance &instance = p_data.instances[i];
		const String path = String(instance.path);
		if (!is_valid_relative_node_path(path) || (i > 0 && path <= previous_path) || instance.kind > BakedVisibilityData3DData::INSTANCE_KIND_DIRECTIONAL_LIGHT || (instance.flags & ~BakedVisibilityData3DData::INSTANCE_FLAGS_MASK) != 0 || instance.signature_sha256.size() != 32 || !is_finite_aabb(instance.local_bounds)) {
			set_error(r_error, "Baked visibility metadata has an invalid instance.");
			return false;
		}
		previous_path = path;
	}
	Vector3i level_grid = expected_leaf_grid;
	int level_first = 0;
	uint32_t level = 0;
	uint32_t expected_first_cell = 0;
	while (true) {
		const uint64_t level_count = tile_count_for_grid(level_grid);
		if (level_count == 0 || level_count > uint64_t(p_data.tiles.size()) || level_first + int(level_count) > p_data.tiles.size()) {
			set_error(r_error, "Baked visibility tile hierarchy is truncated.");
			return false;
		}
		for (int index = 0; index < int(level_count); index++) {
			const BakedVisibilityData3DData::Tile &tile = p_data.tiles[level_first + index];
			const Vector3i coordinate(index % level_grid.x, (index / level_grid.x) % level_grid.y, index / (level_grid.x * level_grid.y));
			if (tile.coordinate != coordinate || tile.level != level || tile.dependency_signature.size() != 32) {
				set_error(r_error, "Baked visibility tile metadata is invalid.");
				return false;
			}
			if (level == 0) {
				const uint32_t cell_count = uint32_t(MIN(p_data.grid_size.x, (coordinate.x + 1) * int(BakedVisibilityData3DData::TILE_SIZE)) - coordinate.x * int(BakedVisibilityData3DData::TILE_SIZE)) * uint32_t(MIN(p_data.grid_size.y, (coordinate.y + 1) * int(BakedVisibilityData3DData::TILE_SIZE)) - coordinate.y * int(BakedVisibilityData3DData::TILE_SIZE)) * uint32_t(MIN(p_data.grid_size.z, (coordinate.z + 1) * int(BakedVisibilityData3DData::TILE_SIZE)) - coordinate.z * int(BakedVisibilityData3DData::TILE_SIZE));
				if (tile.flags != BakedVisibilityData3DData::Tile::FLAG_LEAF || tile.first_cell != expected_first_cell || tile.cell_count != cell_count) {
					set_error(r_error, "Baked visibility leaf metadata is invalid.");
					return false;
				}
				expected_first_cell += cell_count;
			} else {
				if (tile.flags != 0 || tile.first_cell != 0 || tile.cell_count != 0) {
					set_error(r_error, "Baked visibility parent tile metadata is invalid.");
					return false;
				}
			}
		}
		if (level_grid == Vector3i(1, 1, 1)) {
			break;
		}
		const Vector3i parent_grid((level_grid.x + 1) / 2, (level_grid.y + 1) / 2, (level_grid.z + 1) / 2);
		for (int child = 0; child < int(level_count); child++) {
			const BakedVisibilityData3DData::Tile &child_tile = p_data.tiles[level_first + child];
			const int expected_parent = level_first + int(level_count) + (child_tile.coordinate.x / 2) + parent_grid.x * ((child_tile.coordinate.y / 2) + parent_grid.y * (child_tile.coordinate.z / 2));
			if (child_tile.parent != expected_parent) {
				set_error(r_error, "Baked visibility tile parent metadata is invalid.");
				return false;
			}
		}
		level_first += level_count;
		level_grid = parent_grid;
		level++;
	}
	if (level_first + 1 != p_data.tiles.size() || p_data.tiles[level_first].parent != -1 || p_data.hierarchy_depth != level + 1 || expected_first_cell != uint32_t(p_data.grid_size.x * p_data.grid_size.y * p_data.grid_size.z)) {
		set_error(r_error, "Baked visibility tile hierarchy metadata is invalid.");
		return false;
	}
	for (double stage_time : p_data.stage_times_ms) {
		if (!Math::is_finite(stage_time) || stage_time < 0.0) {
			set_error(r_error, "Baked visibility stage timing is invalid.");
			return false;
		}
	}
	return true;
}

static bool _cell_belongs_to_leaf(const BakedVisibilityData3DData &p_data, const BakedVisibilityData3DData::Tile &p_tile, uint32_t p_cell_index) {
	const uint32_t plane = uint32_t(p_data.grid_size.x) * uint32_t(p_data.grid_size.y);
	const uint32_t z = p_cell_index / plane;
	const uint32_t remainder = p_cell_index % plane;
	const uint32_t y = remainder / uint32_t(p_data.grid_size.x);
	const uint32_t x = remainder % uint32_t(p_data.grid_size.x);
	return int(x) / int(BakedVisibilityData3DData::TILE_SIZE) == p_tile.coordinate.x && int(y) / int(BakedVisibilityData3DData::TILE_SIZE) == p_tile.coordinate.y && int(z) / int(BakedVisibilityData3DData::TILE_SIZE) == p_tile.coordinate.z;
}

static void _append_leaf_chunk(const BakedVisibilityData3DData &p_data, const BakedVisibilityData3DData::Tile &p_tile, ByteWriter &r_chunk) {
	Vector<uint32_t> local_set_sources;
	for (uint32_t offset = 0; offset < p_tile.cell_count; offset++) {
		const BakedVisibilityData3DData::Cell &cell = p_data.cells[p_data.tile_cell_indices[p_tile.first_cell + offset]];
		for (uint32_t global_set : { cell.primary_set, cell.transport_set }) {
			bool found = false;
			for (uint32_t existing : local_set_sources) {
				if (existing == global_set) {
					found = true;
					break;
				}
			}
			if (!found) {
				local_set_sources.push_back(global_set);
			}
		}
	}
	r_chunk.uleb(p_tile.cell_count);
	r_chunk.uleb(local_set_sources.size());
	for (uint32_t global_set : local_set_sources) {
		const Vector<uint32_t> &set = p_data.sets[global_set];
		r_chunk.uleb(set.size());
		uint32_t previous = 0;
		for (uint32_t member : set) {
			r_chunk.uleb(member - previous);
			previous = member;
		}
	}
	for (uint32_t offset = 0; offset < p_tile.cell_count; offset++) {
		const uint32_t cell_index = p_data.tile_cell_indices[p_tile.first_cell + offset];
		const BakedVisibilityData3DData::Cell &cell = p_data.cells[cell_index];
		r_chunk.uleb(cell_index);
		r_chunk.u32(cell.flags);
		for (uint32_t global_set : { cell.primary_set, cell.transport_set }) {
			uint32_t local_set = 0;
			while (local_set_sources[local_set] != global_set) {
				local_set++;
			}
			r_chunk.uleb(local_set);
		}
	}
	const uint32_t chunk_crc = crc32(r_chunk.bytes.ptr(), r_chunk.bytes.size());
	r_chunk.u32(chunk_crc);
}

// Decoded payloads keep leaf cells in their per-tile chunks so runtime loads do
// not materialize the complete grid.  Offline restaging can legitimately pass
// such a lazy value back to encode after updating instance signatures.  Expand
// it through the same checked leaf decoder before applying the canonical grid
// invariant; never infer or pad missing cells.
static Error _materialize_lazy_cells(BakedVisibilityData3DData &r_data, String *r_error) {
	if (!r_data.cells.is_empty()) {
		return OK;
	}
	const uint64_t grid_cells = uint64_t(r_data.grid_size.x) * uint64_t(r_data.grid_size.y) * uint64_t(r_data.grid_size.z);
	if (grid_cells == 0 || grid_cells > BakedVisibilityData3DData::MAX_CELLS || r_data.tiles.is_empty()) {
		return OK;
	}
	for (const BakedVisibilityData3DData::Tile &tile : r_data.tiles) {
		if ((tile.flags & BakedVisibilityData3DData::Tile::FLAG_LEAF) && tile.payload.is_empty()) {
			set_error(r_error, "Baked visibility lazy leaf payload is missing.");
			return ERR_FILE_CORRUPT;
		}
	}

	Vector<BakedVisibilityData3DData::Tile> expected_tiles;
	Vector<uint32_t> expected_indices;
	Vector3i expected_tile_grid;
	uint32_t expected_depth = 0;
	if (BakedVisibilityCodec::build_tile_hierarchy(r_data.grid_size, expected_tile_grid, expected_depth, expected_tiles, expected_indices, r_error) != OK || expected_tile_grid != r_data.tile_grid_size || expected_depth != r_data.hierarchy_depth || expected_tiles.size() != r_data.tiles.size()) {
		if (r_error && r_error->is_empty()) {
			*r_error = "Baked visibility lazy tile hierarchy is invalid.";
		}
		return ERR_FILE_CORRUPT;
	}
	r_data.tile_cell_indices = expected_indices;
	r_data.cells.resize(int(grid_cells));
	Vector<uint8_t> seen;
	seen.resize(int(grid_cells));
	for (int index = 0; index < seen.size(); index++) {
		seen.write[index] = 0;
	}
	r_data.sets.clear();
	r_data.sets.push_back(Vector<uint32_t>());

	auto intern_set = [&r_data](const Vector<uint32_t> &p_set) -> uint32_t {
		for (int index = 0; index < r_data.sets.size(); index++) {
			if (equal_set(r_data.sets[index], p_set)) {
				return uint32_t(index);
			}
		}
		r_data.sets.push_back(p_set);
		return uint32_t(r_data.sets.size() - 1);
	};
	for (uint32_t tile_index = 0; tile_index < uint32_t(r_data.tiles.size()); tile_index++) {
		const BakedVisibilityData3DData::Tile &tile = r_data.tiles[tile_index];
		if (!(tile.flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) {
			continue;
		}
		Vector<uint32_t> cell_indices;
		Vector<BakedVisibilityData3DData::Cell> cells;
		Vector<Vector<uint32_t>> local_sets;
		if (BakedVisibilityCodec::decode_leaf_payload(r_data, tile_index, cell_indices, cells, local_sets, r_error) != OK || cell_indices.size() != cells.size()) {
			return ERR_FILE_CORRUPT;
		}
		Vector<uint32_t> local_to_global;
		local_to_global.resize(local_sets.size());
		for (int set = 0; set < local_sets.size(); set++) {
			local_to_global.write[set] = intern_set(local_sets[set]);
		}
		for (int cell = 0; cell < cell_indices.size(); cell++) {
			const uint32_t index = cell_indices[cell];
			if (index >= grid_cells || seen[index]) {
				set_error(r_error, vformat("Baked visibility lazy leaf cells are duplicated or out of bounds (tile=%d index=%d).", tile_index, index));
				return ERR_FILE_CORRUPT;
			}
			BakedVisibilityData3DData::Cell materialized = cells[cell];
			if (materialized.primary_set >= uint32_t(local_to_global.size()) || materialized.transport_set >= uint32_t(local_to_global.size())) {
				set_error(r_error, "Baked visibility lazy leaf set index is invalid.");
				return ERR_FILE_CORRUPT;
			}
			materialized.primary_set = local_to_global[materialized.primary_set];
			materialized.transport_set = local_to_global[materialized.transport_set];
			r_data.cells.write[index] = materialized;
			seen.write[index] = 1;
		}
	}
	for (uint8_t cell_seen : seen) {
		if (!cell_seen) {
			set_error(r_error, "Baked visibility lazy leaf cells do not cover the bounded grid.");
			return ERR_FILE_CORRUPT;
		}
	}
	return OK;
}

Error BakedVisibilityCodec::encode(const BakedVisibilityData3DData &p_data, PackedByteArray &r_bytes, String *r_error) {
	BakedVisibilityData3DData data = p_data;
	if (_materialize_lazy_cells(data, r_error) != OK) {
		return ERR_FILE_CORRUPT;
	}
	if (data.grid_size.x <= 0 || data.grid_size.y <= 0 || data.grid_size.z <= 0 || data.grid_size.x > int(BakedVisibilityData3DData::MAX_CELLS) || data.grid_size.y > int(BakedVisibilityData3DData::MAX_CELLS) || data.grid_size.z > int(BakedVisibilityData3DData::MAX_CELLS)) {
		set_error(r_error, "Baked visibility grid dimensions exceed their bound.");
		return ERR_INVALID_DATA;
	}
	const uint64_t grid_cells = uint64_t(data.grid_size.x) * uint64_t(data.grid_size.y) * uint64_t(data.grid_size.z);
	if (grid_cells > BakedVisibilityData3DData::MAX_CELLS || grid_cells != uint64_t(data.cells.size())) {
		set_error(r_error, "Baked visibility cell count exceeds its bounded grid.");
		return ERR_INVALID_DATA;
	}
	if (data.sets.is_empty()) {
		data.sets.push_back(Vector<uint32_t>());
	}
	if (data.tiles.is_empty()) {
		// Keep hand-authored test payloads and programmatic callers source
		// compatible while always emitting the current tiled representation.
		if (build_tile_hierarchy(data.grid_size, data.tile_grid_size, data.hierarchy_depth, data.tiles, data.tile_cell_indices, r_error) != OK) {
			return ERR_INVALID_DATA;
		}
		data.tile_size = Vector3i(BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE);
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

	ByteWriter metadata;
	metadata.u64(data.source_uid);
	metadata.string(data.source_path);
	for (uint8_t byte : data.source_sha256) metadata.u8(byte);
	metadata.aabb(data.local_bounds);
	metadata.f32(data.cell_size.x);
	metadata.f32(data.cell_size.y);
	metadata.f32(data.cell_size.z);
	metadata.uleb(data.grid_size.x);
	metadata.uleb(data.grid_size.y);
	metadata.uleb(data.grid_size.z);
	metadata.uleb(data.tile_grid_size.x);
	metadata.uleb(data.tile_grid_size.y);
	metadata.uleb(data.tile_grid_size.z);
	metadata.uleb(data.tile_size.x);
	metadata.uleb(data.tile_size.y);
	metadata.uleb(data.tile_size.z);
	metadata.uleb(data.hierarchy_depth);
	metadata.u32(data.bake_mask);
	metadata.f32(data.transport_distance);
	metadata.f32(data.lookup_margin);
	metadata.f32(data.coverage_tolerance);
	metadata.f32(data.transport_tolerance);
	metadata.uleb(data.instances.size());
	for (const BakedVisibilityData3DData::Instance &instance : data.instances) {
		metadata.string(String(instance.path));
		metadata.u8(instance.kind);
		metadata.u32(instance.flags);
		metadata.aabb(instance.local_bounds);
		for (uint8_t byte : instance.signature_sha256) metadata.u8(byte);
	}
	metadata.uleb(data.tiles.size());
	for (const BakedVisibilityData3DData::Tile &tile : data.tiles) {
		metadata.uleb(tile.coordinate.x);
		metadata.uleb(tile.coordinate.y);
		metadata.uleb(tile.coordinate.z);
		metadata.u32(uint32_t(tile.parent));
		metadata.uleb(tile.level);
		metadata.uleb(tile.first_cell);
		metadata.uleb(tile.cell_count);
		metadata.u32(tile.flags);
		for (uint8_t byte : tile.dependency_signature) metadata.u8(byte);
	}
	metadata.string(data.report);
	metadata.uleb(data.stage_times_ms.size());
	for (double stage_time : data.stage_times_ms) metadata.f64(stage_time);
	if (metadata.bytes.size() > BakedVisibilityData3DData::MAX_SERIALIZED_BYTES - int(HEADER_BYTES)) {
		set_error(r_error, "Baked visibility metadata exceeds the configured output limit.");
		return ERR_OUT_OF_MEMORY;
	}

	r_bytes.resize(HEADER_BYTES + metadata.bytes.size());
	uint8_t *write = r_bytes.ptrw();
	memcpy(write, MAGIC, sizeof(MAGIC));
	encode_uint32(BakedVisibilityData3DData::FORMAT_VERSION, write + 4);
	encode_uint32(BakedVisibilityData3DData::ALGORITHM_VERSION, write + 8);
	encode_uint32(metadata.bytes.size(), write + 12);
	encode_uint32(crc32(metadata.bytes.ptr(), metadata.bytes.size()), write + 16);
	memcpy(write + HEADER_BYTES, metadata.bytes.ptr(), metadata.bytes.size());
	for (const BakedVisibilityData3DData::Tile &tile : data.tiles) {
		if (!(tile.flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) {
			continue;
		}
		ByteWriter chunk;
		_append_leaf_chunk(data, tile, chunk);
		if (chunk.bytes.size() > int(BakedVisibilityData3DData::MAX_SERIALIZED_BYTES) || r_bytes.size() > int(BakedVisibilityData3DData::MAX_SERIALIZED_BYTES) - 4 - chunk.bytes.size()) {
			set_error(r_error, "Baked visibility leaf chunks exceed the configured output limit.");
			return ERR_OUT_OF_MEMORY;
		}
		const int old_size = r_bytes.size();
		r_bytes.resize(old_size + 4 + chunk.bytes.size());
		encode_uint32(chunk.bytes.size(), r_bytes.ptrw() + old_size);
		memcpy(r_bytes.ptrw() + old_size + 4, chunk.bytes.ptr(), chunk.bytes.size());
	}
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
	const uint32_t metadata_size = decode_uint32(read + 12);
	if (metadata_size > uint32_t(p_bytes.size() - HEADER_BYTES) || decode_uint32(read + 16) != crc32(read + HEADER_BYTES, metadata_size)) {
		set_error(r_error, "Baked visibility metadata failed its integrity check.");
		return ERR_FILE_CORRUPT;
	}
	PackedByteArray metadata;
	metadata.resize(metadata_size);
	memcpy(metadata.ptrw(), read + HEADER_BYTES, metadata_size);
	ByteReader reader { metadata };
	BakedVisibilityData3DData data;
	if (!reader.u64(data.source_uid) || !reader.string(data.source_path) || !reader.can_read(32)) {
		set_error(r_error, "Baked visibility metadata is truncated.");
		return ERR_FILE_CORRUPT;
	}
	data.source_sha256.resize(32);
	for (int i = 0; i < 32; i++) data.source_sha256.write[i] = metadata[reader.offset++];
	uint32_t grid_x = 0, grid_y = 0, grid_z = 0, tile_grid_x = 0, tile_grid_y = 0, tile_grid_z = 0, tile_size_x = 0, tile_size_y = 0, tile_size_z = 0;
	if (!reader.aabb(data.local_bounds) || !reader.f32(data.cell_size.x) || !reader.f32(data.cell_size.y) || !reader.f32(data.cell_size.z) || !reader.uleb(grid_x) || !reader.uleb(grid_y) || !reader.uleb(grid_z) || !reader.uleb(tile_grid_x) || !reader.uleb(tile_grid_y) || !reader.uleb(tile_grid_z) || !reader.uleb(tile_size_x) || !reader.uleb(tile_size_y) || !reader.uleb(tile_size_z) || !reader.uleb(data.hierarchy_depth) || !reader.u32(data.bake_mask) || !reader.f32(data.transport_distance) || !reader.f32(data.lookup_margin) || !reader.f32(data.coverage_tolerance) || !reader.f32(data.transport_tolerance) || grid_x > INT32_MAX || grid_y > INT32_MAX || grid_z > INT32_MAX || tile_grid_x > INT32_MAX || tile_grid_y > INT32_MAX || tile_grid_z > INT32_MAX || tile_size_x > INT32_MAX || tile_size_y > INT32_MAX || tile_size_z > INT32_MAX) {
		set_error(r_error, "Baked visibility metadata dimensions are invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.grid_size = Vector3i(grid_x, grid_y, grid_z);
	data.tile_grid_size = Vector3i(tile_grid_x, tile_grid_y, tile_grid_z);
	data.tile_size = Vector3i(tile_size_x, tile_size_y, tile_size_z);
	uint32_t count = 0;
	if (!reader.uleb(count) || count > BakedVisibilityData3DData::MAX_CELLS * 1024u || uint64_t(count) > (uint64_t(metadata.size()) - reader.offset) / 62) {
		set_error(r_error, "Baked visibility instance count is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.instances.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		String path;
		if (!reader.string(path) || !reader.u8(data.instances.write[i].kind) || !reader.u32(data.instances.write[i].flags) || !reader.aabb(data.instances.write[i].local_bounds) || !reader.can_read(32)) {
			set_error(r_error, "Baked visibility instance metadata is truncated.");
			return ERR_FILE_CORRUPT;
		}
		data.instances.write[i].path = NodePath(path);
		data.instances.write[i].signature_sha256.resize(32);
		for (int byte = 0; byte < 32; byte++) data.instances.write[i].signature_sha256.write[byte] = metadata[reader.offset++];
	}
	if (!reader.uleb(count) || count == 0 || count > BakedVisibilityData3DData::MAX_CELLS * 2u) {
		set_error(r_error, "Baked visibility tile count is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.tiles.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		BakedVisibilityData3DData::Tile &tile = data.tiles.write[i];
		uint32_t x = 0, y = 0, z = 0, parent = 0;
		if (!reader.uleb(x) || !reader.uleb(y) || !reader.uleb(z) || !reader.u32(parent) || !reader.uleb(tile.level) || !reader.uleb(tile.first_cell) || !reader.uleb(tile.cell_count) || !reader.u32(tile.flags) || !reader.can_read(32) || x > INT32_MAX || y > INT32_MAX || z > INT32_MAX) {
			set_error(r_error, "Baked visibility tile metadata is truncated.");
			return ERR_FILE_CORRUPT;
		}
		tile.coordinate = Vector3i(x, y, z);
		tile.parent = int32_t(parent);
		tile.dependency_signature.resize(32);
		for (int byte = 0; byte < 32; byte++) tile.dependency_signature.write[byte] = metadata[reader.offset++];
	}
	if (!reader.string(data.report) || !reader.uleb(count) || count > 1024) {
		set_error(r_error, "Baked visibility report metadata is invalid.");
		return ERR_FILE_CORRUPT;
	}
	data.stage_times_ms.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		if (!reader.f64(data.stage_times_ms.write[i]) || !Math::is_finite(data.stage_times_ms[i]) || data.stage_times_ms[i] < 0.0) {
			set_error(r_error, "Baked visibility stage timing is invalid.");
			return ERR_FILE_CORRUPT;
		}
	}
	if (reader.offset != uint64_t(metadata.size()) || !_validate_metadata(data, r_error)) {
		if (r_error && r_error->is_empty()) set_error(r_error, "Baked visibility metadata has trailing data.");
		return ERR_FILE_CORRUPT;
	}
	uint64_t offset = HEADER_BYTES + metadata_size;
	for (int tile_index = 0; tile_index < data.tiles.size(); tile_index++) {
		BakedVisibilityData3DData::Tile &tile = data.tiles.write[tile_index];
		if (!(tile.flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) {
			continue;
		}
		if (offset > uint64_t(p_bytes.size()) || uint64_t(p_bytes.size()) - offset < 4) {
			set_error(r_error, "Baked visibility leaf chunk table is truncated.");
			return ERR_FILE_CORRUPT;
		}
		const uint32_t length = decode_uint32(p_bytes.ptr() + offset);
		offset += 4;
		if (length < 4 || length > BakedVisibilityData3DData::MAX_SERIALIZED_BYTES || length > uint64_t(p_bytes.size()) - offset) {
			set_error(r_error, "Baked visibility leaf chunk length is invalid.");
			return ERR_FILE_CORRUPT;
		}
		tile.payload.resize(length);
		memcpy(tile.payload.ptrw(), p_bytes.ptr() + offset, length);
		offset += length;
	}
	if (offset != uint64_t(p_bytes.size())) {
		set_error(r_error, "Baked visibility payload has trailing data.");
		return ERR_FILE_CORRUPT;
	}
	r_data = data;
	return OK;
}

Error BakedVisibilityCodec::decode_leaf_payload(const BakedVisibilityData3DData &p_data, uint32_t p_tile_index, Vector<uint32_t> &r_cell_indices, Vector<BakedVisibilityData3DData::Cell> &r_cells, Vector<Vector<uint32_t>> &r_sets, String *r_error) {
	r_cell_indices.clear();
	r_cells.clear();
	r_sets.clear();
	if (p_tile_index >= uint32_t(p_data.tiles.size()) || !(p_data.tiles[p_tile_index].flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) {
		set_error(r_error, "Baked visibility leaf index is invalid.");
		return ERR_INVALID_PARAMETER;
	}
	const BakedVisibilityData3DData::Tile &tile = p_data.tiles[p_tile_index];
	const PackedByteArray &chunk = tile.payload;
	if (chunk.size() < 4 || chunk.size() > int(BakedVisibilityData3DData::MAX_SERIALIZED_BYTES) || decode_uint32(chunk.ptr() + chunk.size() - 4) != crc32(chunk.ptr(), chunk.size() - 4)) {
		set_error(r_error, "Baked visibility leaf chunk failed its integrity check.");
		return ERR_FILE_CORRUPT;
	}
	PackedByteArray body;
	body.resize(chunk.size() - 4);
	memcpy(body.ptrw(), chunk.ptr(), body.size());
	ByteReader reader { body };
	uint32_t cell_count = 0, set_count = 0;
	if (!reader.uleb(cell_count) || !reader.uleb(set_count) || cell_count != tile.cell_count || set_count > cell_count * 2u + 1u) {
		set_error(r_error, "Baked visibility leaf chunk header is invalid.");
		return ERR_FILE_CORRUPT;
	}
	r_sets.resize(set_count);
	for (uint32_t set = 0; set < set_count; set++) {
		uint32_t member_count = 0;
		if (!reader.uleb(member_count) || member_count > uint32_t(p_data.instances.size())) {
			set_error(r_error, "Baked visibility leaf set length is invalid.");
			return ERR_FILE_CORRUPT;
		}
		r_sets.write[set].resize(member_count);
		uint32_t previous = 0;
		for (uint32_t member = 0; member < member_count; member++) {
			uint32_t delta = 0;
			if (!reader.uleb(delta) || (member > 0 && delta == 0) || delta > UINT32_MAX - previous) {
				set_error(r_error, "Baked visibility leaf set delta is invalid.");
				return ERR_FILE_CORRUPT;
			}
			previous += delta;
			if (previous >= uint32_t(p_data.instances.size())) {
				set_error(r_error, "Baked visibility leaf set member is invalid.");
				return ERR_FILE_CORRUPT;
			}
			r_sets.write[set].write[member] = previous;
		}
	}
	const uint64_t all_cells = uint64_t(p_data.grid_size.x) * uint64_t(p_data.grid_size.y) * uint64_t(p_data.grid_size.z);
	for (uint32_t cell = 0; cell < cell_count; cell++) {
		uint32_t index = 0;
		BakedVisibilityData3DData::Cell decoded;
		if (!reader.uleb(index) || !reader.u32(decoded.flags) || !reader.uleb(decoded.primary_set) || !reader.uleb(decoded.transport_set) || index >= all_cells || !_cell_belongs_to_leaf(p_data, tile, index) || (decoded.flags & ~BakedVisibilityData3DData::CELL_FLAGS_MASK) != 0 || decoded.primary_set >= set_count || decoded.transport_set >= set_count) {
			set_error(r_error, "Baked visibility leaf cell record is invalid.");
			return ERR_FILE_CORRUPT;
		}
		for (uint32_t existing : r_cell_indices) {
			if (existing == index) {
				set_error(r_error, "Baked visibility leaf cell records are duplicated.");
				return ERR_FILE_CORRUPT;
			}
		}
		r_cell_indices.push_back(index);
		r_cells.push_back(decoded);
	}
	if (reader.offset != uint64_t(body.size())) {
		set_error(r_error, "Baked visibility leaf chunk has trailing data.");
		return ERR_FILE_CORRUPT;
	}
	return OK;
}
