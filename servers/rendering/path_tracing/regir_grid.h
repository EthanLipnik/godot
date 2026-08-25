/**************************************************************************/
/*  regir_grid.h                                                          */
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

#pragma once

#include "core/error/error_list.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <cstdint>

namespace RendererPathTracing {

// This owns world-space proposal state only. It deliberately has no view,
// eye, screen coordinate, camera rotation, or cached-visibility contract.
static constexpr uint32_t REGIR_GRID_ABI_VERSION = 1;
static constexpr uint64_t REGIR_GRID_MAX_CELL_COUNT = 1024ull * 1024ull;

// Shared direct-light payload contract for backend adapters. `source_id` and
// `sample_id` are stable renderer identities; `source_index_cache` is never an
// identity and must be re-resolved after an upload reorder. It intentionally
// contains no visibility state.
struct alignas(8) RegirDirectLightSample {
	uint32_t source_type = 0;
	uint32_t source_index_cache = 0xffffffffu;
	uint64_t source_id = 0;
	uint64_t sample_id = 0;
	Vector2 sample_parameter;
	float proposal_pdf = 0.0f;
	float target = 0.0f;
	float weight_sum = 0.0f;
	uint32_t candidate_count = 0;
	uint32_t age = 0;
	uint32_t flags = 0;
	uint64_t light_distribution_generation = 0;
};
static_assert(sizeof(RegirDirectLightSample) == 64, "ReGIR direct-light sample ABI must remain 64 bytes.");

struct RegirGridCellKey {
	uint32_t abi_version = REGIR_GRID_ABI_VERSION;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;

	bool operator==(const RegirGridCellKey &p_other) const;
};

struct RegirGridDescriptor {
	uint32_t abi_version = REGIR_GRID_ABI_VERSION;
	uint32_t cells_x = 0;
	uint32_t cells_y = 0;
	uint32_t cells_z = 0;
	float cell_size = 0.0f;
	// Key (0, 0, 0) starts at this world-space origin.
	Vector3 world_origin;

	bool operator==(const RegirGridDescriptor &p_other) const;
};

struct RegirGridRevisions {
	uint32_t abi_version = REGIR_GRID_ABI_VERSION;
	uint64_t light_distribution_generation = 0;
	uint64_t geometry_generation = 0;
	uint64_t visibility_residency_generation = 0;
	uint64_t environment_generation = 0;

	bool matches(const RegirGridRevisions &p_other) const;
};

// A stable selected light proposal. Source/sample identities correspond to
// LightSamplingGpuRecord identities, never to a distribution array index.
struct RegirGridProposal {
	uint64_t selected_source_id = 0;
	uint64_t selected_sample_id = 0;
	float selected_weight = 0.0f;
	uint32_t candidate_count = 0;
	uint32_t age = 0;
};

struct RegirGridCellState {
	RegirGridCellKey key;
	RegirGridProposal proposal;
	RegirGridRevisions revisions;
	bool valid = false;
};

struct RegirGridDiagnostics {
	uint64_t scroll_count = 0;
	uint64_t preserved_cell_count = 0;
	uint64_t invalidated_cell_count = 0;
	uint64_t cleared_cell_count = 0;
	uint64_t update_count = 0;
	uint64_t query_count = 0;
	uint64_t hit_count = 0;
	uint64_t miss_count = 0;
	uint64_t revision_rejection_count = 0;
	uint64_t last_scroll_preserved_cell_count = 0;
	uint64_t last_scroll_invalidated_cell_count = 0;
	uint64_t checksum = 0;
};

struct RegirGridQuery {
	bool hit = false;
	// This remains true even for a valid proposal. A consumer must re-evaluate
	// the current receiver and visibility; ReGIR never supplies cached occlusion.
	bool requires_current_visibility_evaluation = true;
	RegirGridProposal proposal;
};

class RegirGrid {
	RegirGridDescriptor descriptor;
	RegirGridCellKey center_key;
	Vector<RegirGridCellState> cells;
	RegirGridDiagnostics diagnostics;
	bool configured = false;

	Error _key_to_index(const RegirGridCellKey &p_key, uint32_t &r_index) const;
	void _initialize_cells();
	void _refresh_checksum();

public:
	bool is_configured() const { return configured; }
	const RegirGridDescriptor &get_descriptor() const { return descriptor; }
	const RegirGridCellKey &get_center_key() const { return center_key; }
	const Vector<RegirGridCellState> &get_cells() const { return cells; }
	const RegirGridDiagnostics &get_diagnostics() const { return diagnostics; }

	static Error validate_descriptor(const RegirGridDescriptor &p_descriptor, String *r_error = nullptr);
	static Error world_position_to_cell(const RegirGridDescriptor &p_descriptor, const Vector3 &p_world_position, RegirGridCellKey &r_key, String *r_error = nullptr);

	Error configure(const RegirGridDescriptor &p_descriptor, const Vector3 &p_camera_world_position, String *r_error = nullptr);
	Error recenter(const Vector3 &p_camera_world_position, String *r_error = nullptr);
	Error update(const Vector3 &p_world_position, const RegirGridProposal &p_proposal, const RegirGridRevisions &p_revisions, String *r_error = nullptr);
	RegirGridQuery query(const Vector3 &p_world_position, const RegirGridRevisions &p_revisions, String *r_error = nullptr);
};

} // namespace RendererPathTracing
