/**************************************************************************/
/*  baked_visibility_checkpoint.cpp                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "baked_visibility_checkpoint.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"

namespace {

static constexpr uint8_t MAGIC[] = { 'B', 'V', 'C', 'K' };
static constexpr uint32_t HEADER_BYTES = 4 + 4 + 4 + 4;
static constexpr uint32_t MAX_CHECKPOINT_BYTES = 512 * 1024 * 1024;

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

struct Writer {
	PackedByteArray bytes;
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
	void bytes32(const PackedByteArray &p_value) {
		for (int i = 0; i < 32; i++) {
			bytes.push_back(p_value.size() == 32 ? p_value[i] : 0);
		}
	}
	void vector3i(const Vector3i &p_value) {
		u32(uint32_t(p_value.x));
		u32(uint32_t(p_value.y));
		u32(uint32_t(p_value.z));
	}
	void string(const String &p_value) {
		const CharString utf8 = p_value.utf8();
		u32(utf8.length());
		const int offset = bytes.size();
		bytes.resize(offset + utf8.length());
		if (utf8.length() > 0) {
			memcpy(bytes.ptrw() + offset, utf8.ptr(), utf8.length());
		}
	}
};

struct Reader {
	const PackedByteArray &bytes;
	uint64_t offset = HEADER_BYTES;
	bool can_read(uint64_t p_count) const { return p_count <= uint64_t(bytes.size()) - offset; }
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
	bool bytes32(PackedByteArray &r_value) {
		if (!can_read(32)) {
			return false;
		}
		r_value.resize(32);
		for (int i = 0; i < 32; i++) {
			r_value.write[i] = bytes[offset++];
		}
		return true;
	}
	bool vector3i(Vector3i &r_value) {
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t z = 0;
		if (!u32(x) || !u32(y) || !u32(z) || x > INT32_MAX || y > INT32_MAX || z > INT32_MAX) {
			return false;
		}
		r_value = Vector3i(x, y, z);
		return true;
	}
	bool string(String &r_value) {
		uint32_t length = 0;
		if (!u32(length) || !can_read(length)) {
			return false;
		}
		r_value = String::utf8(reinterpret_cast<const char *>(bytes.ptr() + offset), length);
		offset += length;
		return true;
	}
};

static void set_error(String *r_error, const String &p_error) {
	if (r_error) {
		*r_error = p_error;
	}
}

static Error atomic_replace(const String &p_temporary_path, const String &p_target_path) {
	const String target = ProjectSettings::get_singleton()->globalize_path(p_target_path);
	const String temporary = ProjectSettings::get_singleton()->globalize_path(p_temporary_path);
	const String previous = target + ".previous";
	if (FileAccess::exists(previous)) {
		DirAccess::remove_absolute(previous);
	}
	const bool had_previous = FileAccess::exists(p_target_path);
	if (had_previous && DirAccess::rename_absolute(target, previous) != OK) {
		return ERR_CANT_CREATE;
	}
	if (DirAccess::rename_absolute(temporary, target) != OK) {
		if (had_previous) {
			DirAccess::rename_absolute(previous, target);
		}
		return ERR_CANT_CREATE;
	}
	if (had_previous) {
		DirAccess::remove_absolute(previous);
	}
	return OK;
}

} // namespace

Error BakedVisibilityBakeCheckpointStore::save(const String &p_path, const BakedVisibilityBakeCheckpoint &p_checkpoint, String *r_error) {
	const bool valid_digests = (p_checkpoint.source_sha256.is_empty() || p_checkpoint.source_sha256.size() == 32) && p_checkpoint.settings_sha256.size() == 32;
	const uint64_t leaf_count = uint64_t(p_checkpoint.tile_grid_size.x) * uint64_t(p_checkpoint.tile_grid_size.y) * uint64_t(p_checkpoint.tile_grid_size.z);
	const uint64_t cell_count = uint64_t(p_checkpoint.grid_size.x) * uint64_t(p_checkpoint.grid_size.y) * uint64_t(p_checkpoint.grid_size.z);
	if (p_path.is_empty() || p_checkpoint.format_version != BakedVisibilityData3DData::FORMAT_VERSION || p_checkpoint.grid_size.x <= 0 || p_checkpoint.grid_size.y <= 0 || p_checkpoint.grid_size.z <= 0 || leaf_count == 0 || leaf_count > BakedVisibilityData3DData::MAX_CELLS || cell_count > BakedVisibilityData3DData::MAX_CELLS || p_checkpoint.completed_tiles.size() != int(leaf_count) || p_checkpoint.completed_cell_bitmap.size() != int(cell_count) || p_checkpoint.tiles.is_empty() || p_checkpoint.cells.size() != int(cell_count) || !valid_digests) {
		set_error(r_error, "Baked visibility checkpoint is not a complete immutable bake snapshot.");
		return ERR_INVALID_DATA;
	}
	Writer payload;
	payload.vector3i(p_checkpoint.grid_size);
	payload.vector3i(p_checkpoint.tile_grid_size);
	payload.u32(p_checkpoint.hierarchy_depth);
	payload.u32(p_checkpoint.completed_cells);
	payload.u32(p_checkpoint.completed_tile_count);
	payload.bytes32(p_checkpoint.source_sha256);
	payload.bytes32(p_checkpoint.settings_sha256);
	payload.u32(p_checkpoint.geometry_paths.size());
	for (const String &path : p_checkpoint.geometry_paths) {
		payload.string(path);
	}
	payload.u32(p_checkpoint.light_paths.size());
	for (const String &path : p_checkpoint.light_paths) {
		payload.string(path);
	}
	payload.u32(p_checkpoint.tiles.size());
	for (const BakedVisibilityData3DData::Tile &tile : p_checkpoint.tiles) {
		if (tile.dependency_signature.size() != 32) {
			set_error(r_error, "Baked visibility checkpoint has an invalid tile signature.");
			return ERR_INVALID_DATA;
		}
		for (const Vector<uint32_t> *dependencies : { &tile.candidate_dependencies, &tile.certificate_dependencies }) {
			for (int dependency = 0; dependency < dependencies->size(); dependency++) {
				if ((*dependencies)[dependency] >= uint32_t(p_checkpoint.geometry_paths.size()) || (dependency > 0 && (*dependencies)[dependency] <= (*dependencies)[dependency - 1])) {
					set_error(r_error, "Baked visibility checkpoint has invalid geometry dependencies.");
					return ERR_INVALID_DATA;
				}
			}
		}
		for (int dependency = 0; dependency < tile.light_dependencies.size(); dependency++) {
			if (tile.light_dependencies[dependency] >= uint32_t(p_checkpoint.light_paths.size()) || (dependency > 0 && tile.light_dependencies[dependency] <= tile.light_dependencies[dependency - 1])) {
				set_error(r_error, "Baked visibility checkpoint has invalid certificate dependencies.");
				return ERR_INVALID_DATA;
			}
		}
		payload.vector3i(tile.coordinate);
		payload.u32(uint32_t(tile.parent));
		payload.u32(tile.level);
		payload.u32(tile.first_cell);
		payload.u32(tile.cell_count);
		payload.u32(tile.flags);
		payload.bytes32(tile.dependency_signature);
		for (const Vector<uint32_t> *dependencies : { &tile.candidate_dependencies, &tile.light_dependencies, &tile.certificate_dependencies }) {
			payload.u32(dependencies->size());
			for (uint32_t dependency : *dependencies) {
				payload.u32(dependency);
			}
		}
	}
	payload.u32(p_checkpoint.tile_cell_indices.size());
	for (uint32_t index : p_checkpoint.tile_cell_indices) {
		payload.u32(index);
	}
	payload.u32(p_checkpoint.completed_tiles.size());
	for (uint8_t complete : p_checkpoint.completed_tiles) {
		payload.bytes.push_back(complete ? 1 : 0);
	}
	payload.u32(p_checkpoint.completed_cell_bitmap.size());
	for (uint8_t complete : p_checkpoint.completed_cell_bitmap) {
		payload.bytes.push_back(complete ? 1 : 0);
	}
	payload.u32(p_checkpoint.cells.size());
	for (const BakedVisibilityBakeCell &cell : p_checkpoint.cells) {
		payload.vector3i(cell.coordinate);
		payload.u32(cell.flags);
		for (const PackedInt32Array *members : { &cell.primary.geometry, &cell.primary.lights, &cell.transport.geometry, &cell.transport.lights }) {
			payload.u32(members->size());
			for (int value : *members) {
				payload.u32(uint32_t(value));
			}
		}
	}
	if (payload.bytes.size() > int(MAX_CHECKPOINT_BYTES - HEADER_BYTES)) {
		set_error(r_error, "Baked visibility checkpoint exceeds its configured bound.");
		return ERR_OUT_OF_MEMORY;
	}
	PackedByteArray bytes;
	bytes.resize(HEADER_BYTES + payload.bytes.size());
	uint8_t *write = bytes.ptrw();
	for (int i = 0; i < 4; i++) {
		write[i] = MAGIC[i];
	}
	encode_uint32(BakedVisibilityData3DData::FORMAT_VERSION, write + 4);
	encode_uint32(payload.bytes.size(), write + 8);
	encode_uint32(crc32(payload.bytes.ptr(), payload.bytes.size()), write + 12);
	memcpy(write + HEADER_BYTES, payload.bytes.ptr(), payload.bytes.size());
	const String temporary_path = p_path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temporary_path, FileAccess::WRITE);
	if (file.is_null()) {
		set_error(r_error, vformat("Could not create baked visibility checkpoint '%s'.", temporary_path));
		return ERR_CANT_CREATE;
	}
	file->store_buffer(bytes.ptr(), bytes.size());
	file->flush();
	if (file->get_error() != OK || atomic_replace(temporary_path, p_path) != OK) {
		set_error(r_error, vformat("Could not atomically replace baked visibility checkpoint '%s'.", p_path));
		return ERR_CANT_CREATE;
	}
	return OK;
}

Error BakedVisibilityBakeCheckpointStore::load(const String &p_path, BakedVisibilityBakeCheckpoint &r_checkpoint, String *r_error) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		set_error(r_error, vformat("Could not open baked visibility checkpoint '%s'.", p_path));
		return ERR_FILE_NOT_FOUND;
	}
	const uint64_t length = file->get_length();
	if (length < HEADER_BYTES || length > MAX_CHECKPOINT_BYTES) {
		set_error(r_error, "Baked visibility checkpoint has an invalid size.");
		return ERR_FILE_CORRUPT;
	}
	PackedByteArray bytes = file->get_buffer(length);
	if (bytes.size() != length || memcmp(bytes.ptr(), MAGIC, sizeof(MAGIC)) != 0 || decode_uint32(bytes.ptr() + 4) != BakedVisibilityData3DData::FORMAT_VERSION || decode_uint32(bytes.ptr() + 8) != uint32_t(bytes.size() - HEADER_BYTES) || decode_uint32(bytes.ptr() + 12) != crc32(bytes.ptr() + HEADER_BYTES, bytes.size() - HEADER_BYTES)) {
		set_error(r_error, "Baked visibility checkpoint failed its integrity check.");
		return ERR_FILE_CORRUPT;
	}
	Reader reader { bytes };
	BakedVisibilityBakeCheckpoint checkpoint;
	checkpoint.format_version = BakedVisibilityData3DData::FORMAT_VERSION;
	uint32_t count = 0;
	if (!reader.vector3i(checkpoint.grid_size) || !reader.vector3i(checkpoint.tile_grid_size) || !reader.u32(checkpoint.hierarchy_depth) || !reader.u32(checkpoint.completed_cells) || !reader.u32(checkpoint.completed_tile_count) || !reader.bytes32(checkpoint.source_sha256) || !reader.bytes32(checkpoint.settings_sha256) || !reader.u32(count) || count > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility checkpoint header is invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.geometry_paths.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		if (!reader.string(checkpoint.geometry_paths.write[i])) {
			set_error(r_error, "Baked visibility checkpoint geometry keys are invalid.");
			return ERR_FILE_CORRUPT;
		}
	}
	if (!reader.u32(count) || count > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility checkpoint light keys are invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.light_paths.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		if (!reader.string(checkpoint.light_paths.write[i])) {
			set_error(r_error, "Baked visibility checkpoint light keys are invalid.");
			return ERR_FILE_CORRUPT;
		}
	}
	if (!reader.u32(count) || count == 0 || count > BakedVisibilityData3DData::MAX_CELLS * 2u) {
		set_error(r_error, "Baked visibility checkpoint tile count is invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.tiles.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		BakedVisibilityData3DData::Tile &tile = checkpoint.tiles.write[i];
		uint32_t parent = 0;
		if (!reader.vector3i(tile.coordinate) || !reader.u32(parent) || !reader.u32(tile.level) || !reader.u32(tile.first_cell) || !reader.u32(tile.cell_count) || !reader.u32(tile.flags) || !reader.bytes32(tile.dependency_signature)) {
			set_error(r_error, "Baked visibility checkpoint tile table is truncated.");
			return ERR_FILE_CORRUPT;
		}
		tile.parent = int32_t(parent);
		for (Vector<uint32_t> *dependencies : { &tile.candidate_dependencies, &tile.light_dependencies, &tile.certificate_dependencies }) {
			uint32_t dependency_count = 0;
			if (!reader.u32(dependency_count) || dependency_count > BakedVisibilityData3DData::MAX_CELLS || !reader.can_read(uint64_t(dependency_count) * 4u)) {
				set_error(r_error, "Baked visibility checkpoint certificate dependencies are invalid.");
				return ERR_FILE_CORRUPT;
			}
			dependencies->resize(dependency_count);
			for (uint32_t dependency = 0; dependency < dependency_count; dependency++) {
				if (!reader.u32(dependencies->write[dependency]) || (dependency > 0 && (*dependencies)[dependency] <= (*dependencies)[dependency - 1])) {
					set_error(r_error, "Baked visibility checkpoint certificate dependencies are invalid.");
					return ERR_FILE_CORRUPT;
				}
			}
		}
	}
	if (!reader.u32(count) || count > BakedVisibilityData3DData::MAX_CELLS || !reader.can_read(uint64_t(count) * 4u)) {
		set_error(r_error, "Baked visibility checkpoint tile cells are invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.tile_cell_indices.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		if (!reader.u32(checkpoint.tile_cell_indices.write[i])) {
			return ERR_FILE_CORRUPT;
		}
	}
	if (!reader.u32(count) || count > BakedVisibilityData3DData::MAX_CELLS || !reader.can_read(count)) {
		set_error(r_error, "Baked visibility checkpoint completion bitmap is invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.completed_tiles.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t complete = bytes[reader.offset++];
		if (complete > 1) {
			set_error(r_error, "Baked visibility checkpoint completion bitmap is invalid.");
			return ERR_FILE_CORRUPT;
		}
		checkpoint.completed_tiles.write[i] = complete;
	}
	if (!reader.u32(count) || count > BakedVisibilityData3DData::MAX_CELLS || !reader.can_read(count)) {
		set_error(r_error, "Baked visibility checkpoint cell completion bitmap is invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.completed_cell_bitmap.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t complete = bytes[reader.offset++];
		if (complete > 1) {
			set_error(r_error, "Baked visibility checkpoint cell completion bitmap is invalid.");
			return ERR_FILE_CORRUPT;
		}
		checkpoint.completed_cell_bitmap.write[i] = complete;
	}
	if (!reader.u32(count) || count > BakedVisibilityData3DData::MAX_CELLS) {
		set_error(r_error, "Baked visibility checkpoint cell table is invalid.");
		return ERR_FILE_CORRUPT;
	}
	checkpoint.cells.resize(count);
	for (uint32_t cell_index = 0; cell_index < count; cell_index++) {
		BakedVisibilityBakeCell &cell = checkpoint.cells.write[cell_index];
		if (!reader.vector3i(cell.coordinate) || !reader.u32(cell.flags)) {
			set_error(r_error, "Baked visibility checkpoint cell table is truncated.");
			return ERR_FILE_CORRUPT;
		}
		for (PackedInt32Array *members : { &cell.primary.geometry, &cell.primary.lights, &cell.transport.geometry, &cell.transport.lights }) {
			uint32_t member_count = 0;
			if (!reader.u32(member_count) || member_count > BakedVisibilityData3DData::MAX_CELLS || !reader.can_read(uint64_t(member_count) * 4u)) {
				set_error(r_error, "Baked visibility checkpoint set is invalid.");
				return ERR_FILE_CORRUPT;
			}
			members->resize(member_count);
			for (uint32_t member = 0; member < member_count; member++) {
				uint32_t value = 0;
				if (!reader.u32(value) || value > INT32_MAX) {
					set_error(r_error, "Baked visibility checkpoint set member is invalid.");
					return ERR_FILE_CORRUPT;
				}
				members->set(member, value);
			}
		}
	}
	if (reader.offset != uint64_t(bytes.size())) {
		set_error(r_error, "Baked visibility checkpoint has trailing data.");
		return ERR_FILE_CORRUPT;
	}
	Vector3i expected_grid;
	uint32_t expected_depth = 0;
	Vector<BakedVisibilityData3DData::Tile> expected_tiles;
	Vector<uint32_t> expected_indices;
	if (BakedVisibilityCodec::build_tile_hierarchy(checkpoint.grid_size, expected_grid, expected_depth, expected_tiles, expected_indices, r_error) != OK || checkpoint.tile_grid_size != expected_grid || checkpoint.hierarchy_depth != expected_depth || checkpoint.tiles.size() != expected_tiles.size() || checkpoint.tile_cell_indices != expected_indices || checkpoint.completed_tiles.size() != int(uint64_t(expected_grid.x) * expected_grid.y * expected_grid.z) || checkpoint.completed_cell_bitmap.size() != checkpoint.grid_size.x * checkpoint.grid_size.y * checkpoint.grid_size.z || checkpoint.cells.size() != checkpoint.grid_size.x * checkpoint.grid_size.y * checkpoint.grid_size.z) {
		set_error(r_error, "Baked visibility checkpoint hierarchy is invalid.");
		return ERR_FILE_CORRUPT;
	}
	for (int i = 0; i < checkpoint.tiles.size(); i++) {
		const BakedVisibilityData3DData::Tile &tile = checkpoint.tiles[i];
		const BakedVisibilityData3DData::Tile &expected = expected_tiles[i];
		if (tile.coordinate != expected.coordinate || tile.parent != expected.parent || tile.level != expected.level || tile.first_cell != expected.first_cell || tile.cell_count != expected.cell_count || tile.flags != expected.flags) {
			set_error(r_error, "Baked visibility checkpoint hierarchy is invalid.");
			return ERR_FILE_CORRUPT;
		}
		for (const Vector<uint32_t> *dependencies : { &tile.candidate_dependencies, &tile.certificate_dependencies }) {
			for (uint32_t dependency : *dependencies) {
				if (dependency >= uint32_t(checkpoint.geometry_paths.size())) {
					set_error(r_error, "Baked visibility checkpoint certificate dependency is invalid.");
					return ERR_FILE_CORRUPT;
				}
			}
		}
		for (uint32_t dependency : tile.light_dependencies) {
			if (dependency >= uint32_t(checkpoint.light_paths.size())) {
				set_error(r_error, "Baked visibility checkpoint certificate dependency is invalid.");
				return ERR_FILE_CORRUPT;
			}
		}
	}
	r_checkpoint = checkpoint;
	return OK;
}
