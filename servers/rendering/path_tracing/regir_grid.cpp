/**************************************************************************/
/*  regir_grid.cpp                                                        */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "regir_grid.h"

#include "core/math/math_funcs.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace RendererPathTracing {

static Error _regir_fail(Error p_error, const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static bool _valid_revisions(const RegirGridRevisions &p_revisions) {
	return p_revisions.abi_version == REGIR_GRID_ABI_VERSION && p_revisions.light_distribution_generation != 0 && p_revisions.geometry_generation != 0 && p_revisions.visibility_residency_generation != 0 && p_revisions.environment_generation != 0;
}

static bool _valid_proposal(const RegirGridProposal &p_proposal) {
	return p_proposal.selected_source_id != 0 && p_proposal.selected_sample_id != 0 && Math::is_finite(p_proposal.selected_weight) && p_proposal.selected_weight > 0.0f && p_proposal.candidate_count != 0;
}

static int64_t _minimum_coordinate(int32_t p_center, uint32_t p_extent) {
	return int64_t(p_center) - int64_t(p_extent / 2);
}

static void _hash_byte(uint64_t &r_hash, uint8_t p_value) {
	r_hash ^= p_value;
	r_hash *= 1099511628211ull;
}

static void _hash_u32(uint64_t &r_hash, uint32_t p_value) {
	for (uint32_t i = 0; i < 4; i++) {
		_hash_byte(r_hash, uint8_t(p_value >> (i * 8)));
	}
}

static void _hash_u64(uint64_t &r_hash, uint64_t p_value) {
	for (uint32_t i = 0; i < 8; i++) {
		_hash_byte(r_hash, uint8_t(p_value >> (i * 8)));
	}
}

static void _hash_float(uint64_t &r_hash, float p_value) {
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(p_value));
	std::memcpy(&bits, &p_value, sizeof(bits));
	_hash_u32(r_hash, bits);
}

bool RegirGridCellKey::operator==(const RegirGridCellKey &p_other) const {
	return abi_version == REGIR_GRID_ABI_VERSION && p_other.abi_version == REGIR_GRID_ABI_VERSION && x == p_other.x && y == p_other.y && z == p_other.z;
}

bool RegirGridDescriptor::operator==(const RegirGridDescriptor &p_other) const {
	return abi_version == REGIR_GRID_ABI_VERSION && p_other.abi_version == REGIR_GRID_ABI_VERSION && cells_x == p_other.cells_x && cells_y == p_other.cells_y && cells_z == p_other.cells_z && cell_size == p_other.cell_size && world_origin == p_other.world_origin;
}

bool RegirGridRevisions::matches(const RegirGridRevisions &p_other) const {
	return _valid_revisions(*this) && _valid_revisions(p_other) && light_distribution_generation == p_other.light_distribution_generation && geometry_generation == p_other.geometry_generation && visibility_residency_generation == p_other.visibility_residency_generation && environment_generation == p_other.environment_generation;
}

Error RegirGrid::validate_descriptor(const RegirGridDescriptor &p_descriptor, String *r_error) {
	if (p_descriptor.abi_version != REGIR_GRID_ABI_VERSION) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR grid descriptor ABI version is incompatible.", r_error);
	}
	if (p_descriptor.cells_x == 0 || p_descriptor.cells_y == 0 || p_descriptor.cells_z == 0) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR grid dimensions must be nonzero.", r_error);
	}
	if (!Math::is_finite(p_descriptor.cell_size) || p_descriptor.cell_size <= CMP_EPSILON || !p_descriptor.world_origin.is_finite()) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR grid cell size and world origin must be finite and cell size positive.", r_error);
	}
	const uint64_t xy = uint64_t(p_descriptor.cells_x) * uint64_t(p_descriptor.cells_y);
	if (xy > REGIR_GRID_MAX_CELL_COUNT || uint64_t(p_descriptor.cells_z) > REGIR_GRID_MAX_CELL_COUNT / xy) {
		return _regir_fail(ERR_OUT_OF_MEMORY, "ReGIR grid dimensions exceed the bounded CPU reference storage limit.", r_error);
	}
	return OK;
}

Error RegirGrid::world_position_to_cell(const RegirGridDescriptor &p_descriptor, const Vector3 &p_world_position, RegirGridCellKey &r_key, String *r_error) {
	const Error descriptor_error = validate_descriptor(p_descriptor, r_error);
	if (descriptor_error != OK) {
		return descriptor_error;
	}
	if (!p_world_position.is_finite()) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR world position must be finite.", r_error);
	}
	const double lowest = double(std::numeric_limits<int32_t>::min());
	const double highest = double(std::numeric_limits<int32_t>::max());
	auto quantize = [&](float p_position, float p_origin, int32_t &r_coordinate) -> bool {
		const double coordinate = Math::floor((double(p_position) - double(p_origin)) / double(p_descriptor.cell_size));
		if (!std::isfinite(coordinate) || coordinate < lowest || coordinate > highest) {
			return false;
		}
		r_coordinate = int32_t(coordinate);
		return true;
	};
	RegirGridCellKey key;
	if (!quantize(p_world_position.x, p_descriptor.world_origin.x, key.x) || !quantize(p_world_position.y, p_descriptor.world_origin.y, key.y) || !quantize(p_world_position.z, p_descriptor.world_origin.z, key.z)) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR world position maps outside the signed cell-key range.", r_error);
	}
	r_key = key;
	return OK;
}

Error RegirGrid::_key_to_index(const RegirGridCellKey &p_key, uint32_t &r_index) const {
	if (!configured || p_key.abi_version != REGIR_GRID_ABI_VERSION) {
		return ERR_UNCONFIGURED;
	}
	const int64_t x = int64_t(p_key.x) - _minimum_coordinate(center_key.x, descriptor.cells_x);
	const int64_t y = int64_t(p_key.y) - _minimum_coordinate(center_key.y, descriptor.cells_y);
	const int64_t z = int64_t(p_key.z) - _minimum_coordinate(center_key.z, descriptor.cells_z);
	if (x < 0 || y < 0 || z < 0 || x >= descriptor.cells_x || y >= descriptor.cells_y || z >= descriptor.cells_z) {
		return ERR_DOES_NOT_EXIST;
	}
	r_index = uint32_t((z * descriptor.cells_y + y) * descriptor.cells_x + x);
	return OK;
}

void RegirGrid::_initialize_cells() {
	const uint64_t cell_count = uint64_t(descriptor.cells_x) * descriptor.cells_y * descriptor.cells_z;
	cells.resize(int(cell_count));
	const int64_t min_x = _minimum_coordinate(center_key.x, descriptor.cells_x);
	const int64_t min_y = _minimum_coordinate(center_key.y, descriptor.cells_y);
	const int64_t min_z = _minimum_coordinate(center_key.z, descriptor.cells_z);
	for (uint32_t z = 0; z < descriptor.cells_z; z++) {
		for (uint32_t y = 0; y < descriptor.cells_y; y++) {
			for (uint32_t x = 0; x < descriptor.cells_x; x++) {
				const uint32_t index = (z * descriptor.cells_y + y) * descriptor.cells_x + x;
				RegirGridCellState &cell = cells.write[index];
				cell = RegirGridCellState();
				cell.key.x = int32_t(min_x + x);
				cell.key.y = int32_t(min_y + y);
				cell.key.z = int32_t(min_z + z);
			}
		}
	}
}

void RegirGrid::_refresh_checksum() {
	uint64_t hash = 1469598103934665603ull;
	_hash_u32(hash, descriptor.abi_version);
	_hash_u32(hash, descriptor.cells_x);
	_hash_u32(hash, descriptor.cells_y);
	_hash_u32(hash, descriptor.cells_z);
	_hash_float(hash, descriptor.cell_size);
	_hash_float(hash, descriptor.world_origin.x);
	_hash_float(hash, descriptor.world_origin.y);
	_hash_float(hash, descriptor.world_origin.z);
	_hash_u32(hash, uint32_t(center_key.x));
	_hash_u32(hash, uint32_t(center_key.y));
	_hash_u32(hash, uint32_t(center_key.z));
	for (const RegirGridCellState &cell : cells) {
		_hash_u32(hash, uint32_t(cell.key.x));
		_hash_u32(hash, uint32_t(cell.key.y));
		_hash_u32(hash, uint32_t(cell.key.z));
		_hash_u64(hash, cell.proposal.selected_source_id);
		_hash_u64(hash, cell.proposal.selected_sample_id);
		_hash_float(hash, cell.proposal.selected_weight);
		_hash_u32(hash, cell.proposal.candidate_count);
		_hash_u32(hash, cell.proposal.age);
		_hash_u64(hash, cell.revisions.light_distribution_generation);
		_hash_u64(hash, cell.revisions.geometry_generation);
		_hash_u64(hash, cell.revisions.visibility_residency_generation);
		_hash_u64(hash, cell.revisions.environment_generation);
		_hash_byte(hash, cell.valid ? 1 : 0);
	}
	diagnostics.checksum = hash;
}

Error RegirGrid::configure(const RegirGridDescriptor &p_descriptor, const Vector3 &p_camera_world_position, String *r_error) {
	const Error descriptor_error = validate_descriptor(p_descriptor, r_error);
	if (descriptor_error != OK) {
		return descriptor_error;
	}
	RegirGridCellKey requested_center;
	const Error key_error = world_position_to_cell(p_descriptor, p_camera_world_position, requested_center, r_error);
	if (key_error != OK) {
		return key_error;
	}
	if (configured && descriptor == p_descriptor) {
		return recenter(p_camera_world_position, r_error);
	}
	if (configured) {
		diagnostics.cleared_cell_count += cells.size();
	}
	descriptor = p_descriptor;
	center_key = requested_center;
	configured = true;
	_initialize_cells();
	_refresh_checksum();
	return OK;
}

Error RegirGrid::recenter(const Vector3 &p_camera_world_position, String *r_error) {
	if (!configured) {
		return _regir_fail(ERR_UNCONFIGURED, "ReGIR grid must be configured before recentering.", r_error);
	}
	RegirGridCellKey requested_center;
	const Error key_error = world_position_to_cell(descriptor, p_camera_world_position, requested_center, r_error);
	if (key_error != OK) {
		return key_error;
	}
	if (requested_center == center_key) {
		return OK;
	}
	const int64_t delta_x = int64_t(requested_center.x) - center_key.x;
	const int64_t delta_y = int64_t(requested_center.y) - center_key.y;
	const int64_t delta_z = int64_t(requested_center.z) - center_key.z;
	const bool has_overlap = std::abs(delta_x) < int64_t(descriptor.cells_x) && std::abs(delta_y) < int64_t(descriptor.cells_y) && std::abs(delta_z) < int64_t(descriptor.cells_z);
	const Vector<RegirGridCellState> previous_cells = cells;
	center_key = requested_center;
	_initialize_cells();
	uint64_t preserved = 0;
	if (has_overlap) {
		for (const RegirGridCellState &old_cell : previous_cells) {
			uint32_t new_index = 0;
			if (_key_to_index(old_cell.key, new_index) == OK) {
				cells.write[new_index] = old_cell;
				preserved++;
			}
		}
	}
	const uint64_t invalidated = uint64_t(cells.size()) - preserved;
	diagnostics.scroll_count++;
	diagnostics.preserved_cell_count += preserved;
	diagnostics.invalidated_cell_count += invalidated;
	diagnostics.last_scroll_preserved_cell_count = preserved;
	diagnostics.last_scroll_invalidated_cell_count = invalidated;
	if (!has_overlap) {
		diagnostics.cleared_cell_count += cells.size();
	}
	_refresh_checksum();
	return OK;
}

Error RegirGrid::update(const Vector3 &p_world_position, const RegirGridProposal &p_proposal, const RegirGridRevisions &p_revisions, String *r_error) {
	if (!configured) {
		return _regir_fail(ERR_UNCONFIGURED, "ReGIR grid must be configured before updates.", r_error);
	}
	if (!_valid_proposal(p_proposal)) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR proposals require stable nonzero light identities, positive finite weight, and a nonzero candidate count.", r_error);
	}
	if (!_valid_revisions(p_revisions)) {
		return _regir_fail(ERR_INVALID_PARAMETER, "ReGIR updates require nonzero current light, geometry, visibility-residency, and environment generations.", r_error);
	}
	RegirGridCellKey key;
	const Error key_error = world_position_to_cell(descriptor, p_world_position, key, r_error);
	if (key_error != OK) {
		return key_error;
	}
	uint32_t index = 0;
	if (_key_to_index(key, index) != OK) {
		return _regir_fail(ERR_DOES_NOT_EXIST, "ReGIR update lies outside the current bounded world grid.", r_error);
	}
	RegirGridCellState &cell = cells.write[index];
	cell.proposal = p_proposal;
	cell.revisions = p_revisions;
	cell.valid = true;
	diagnostics.update_count++;
	_refresh_checksum();
	return OK;
}

RegirGridQuery RegirGrid::query(const Vector3 &p_world_position, const RegirGridRevisions &p_revisions, String *r_error) {
	RegirGridQuery result;
	diagnostics.query_count++;
	if (!configured) {
		_regir_fail(ERR_UNCONFIGURED, "ReGIR grid must be configured before queries.", r_error);
		diagnostics.miss_count++;
		return result;
	}
	if (!_valid_revisions(p_revisions)) {
		_regir_fail(ERR_INVALID_PARAMETER, "ReGIR queries require nonzero current light, geometry, visibility-residency, and environment generations.", r_error);
		diagnostics.revision_rejection_count++;
		diagnostics.miss_count++;
		return result;
	}
	RegirGridCellKey key;
	if (world_position_to_cell(descriptor, p_world_position, key, r_error) != OK) {
		diagnostics.miss_count++;
		return result;
	}
	uint32_t index = 0;
	if (_key_to_index(key, index) != OK) {
		diagnostics.miss_count++;
		return result;
	}
	const RegirGridCellState &cell = cells[index];
	if (!cell.valid) {
		diagnostics.miss_count++;
		return result;
	}
	if (!cell.revisions.matches(p_revisions)) {
		diagnostics.revision_rejection_count++;
		diagnostics.miss_count++;
		return result;
	}
	result.hit = true;
	result.proposal = cell.proposal;
	diagnostics.hit_count++;
	return result;
}

} // namespace RendererPathTracing
