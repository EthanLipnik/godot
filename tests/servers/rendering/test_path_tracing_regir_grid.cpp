/**************************************************************************/
/*  test_path_tracing_regir_grid.cpp                                      */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_path_tracing_regir_grid)

#include "servers/rendering/path_tracing/regir_grid.h"

#include <limits>

namespace TestPathTracingRegirGrid {

using namespace RendererPathTracing;

static RegirGridDescriptor make_descriptor(uint32_t p_x = 5, uint32_t p_y = 5, uint32_t p_z = 5) {
	RegirGridDescriptor descriptor;
	descriptor.cells_x = p_x;
	descriptor.cells_y = p_y;
	descriptor.cells_z = p_z;
	descriptor.cell_size = 1.0f;
	return descriptor;
}

static RegirGridRevisions make_revisions() {
	RegirGridRevisions revisions;
	revisions.light_distribution_generation = 11;
	revisions.geometry_generation = 12;
	revisions.visibility_residency_generation = 13;
	revisions.environment_generation = 14;
	return revisions;
}

static RegirGridProposal make_proposal(uint64_t p_sample_id = 101) {
	RegirGridProposal proposal;
	proposal.selected_source_id = 100;
	proposal.selected_sample_id = p_sample_id;
	proposal.selected_weight = 2.5f;
	proposal.candidate_count = 7;
	proposal.age = 3;
	return proposal;
}

static const RegirGridCellState *find_cell(const RegirGrid &p_grid, const RegirGridCellKey &p_key) {
	for (const RegirGridCellState &cell : p_grid.get_cells()) {
		if (cell.key == p_key) {
			return &cell;
		}
	}
	return nullptr;
}

static bool caller_may_contribute(const RegirGridQuery &p_query, bool p_current_visibility_evaluated) {
	// ReGIR supplies a proposal only. The consuming estimator owns and must
	// supply the receiver-specific visibility result before contributing it.
	return p_query.hit && p_query.requires_current_visibility_evaluation && p_current_visibility_evaluated;
}

TEST_CASE("[PathTracing][ReGIR] World mapping is deterministic, signed, and descriptor anchored") {
	CHECK_EQ(sizeof(RegirDirectLightSample), 64);
	RegirDirectLightSample sample;
	CHECK_EQ(sample.source_index_cache, 0xffffffffu);
	CHECK_EQ(sample.source_id, 0);
	CHECK_EQ(sample.sample_id, 0);
	const RegirGridDescriptor descriptor = make_descriptor();
	RegirGridCellKey key;
	CHECK_EQ(RegirGrid::world_position_to_cell(descriptor, Vector3(-0.01f, -1.0f, -2.01f), key), OK);
	CHECK_EQ(key.x, -1);
	CHECK_EQ(key.y, -1);
	CHECK_EQ(key.z, -3);

	RegirGridDescriptor anchored = descriptor;
	anchored.world_origin = Vector3(10.0f, -2.0f, 4.0f);
	CHECK_EQ(RegirGrid::world_position_to_cell(anchored, Vector3(9.99f, -2.0f, 5.99f), key), OK);
	CHECK_EQ(key.x, -1);
	CHECK_EQ(key.y, 0);
	CHECK_EQ(key.z, 1);
}

TEST_CASE("[PathTracing][ReGIR] Same-cell camera translation is a no-op and the API has no rotation input") {
	RegirGrid grid;
	const RegirGridDescriptor descriptor = make_descriptor();
	CHECK_EQ(grid.configure(descriptor, Vector3(0.1f, 0.1f, 0.1f)), OK);
	const uint64_t initial_checksum = grid.get_diagnostics().checksum;
	CHECK_EQ(grid.recenter(Vector3(0.9f, 0.4f, 0.2f)), OK);
	CHECK_EQ(grid.get_center_key().x, 0);
	CHECK_EQ(grid.get_diagnostics().scroll_count, 0);
	CHECK_EQ(grid.get_diagnostics().checksum, initial_checksum);
}

TEST_CASE("[PathTracing][ReGIR] One-cell and multi-cell scrolling preserve overlapping world cells in every direction") {
	const RegirGridDescriptor descriptor = make_descriptor();
	const RegirGridRevisions revisions = make_revisions();
	const RegirGridProposal proposal = make_proposal();
	const Vector3 directions[] = {
		Vector3(1, 0, 0),
		Vector3(-1, 0, 0),
		Vector3(0, 1, 0),
		Vector3(0, -1, 0),
		Vector3(0, 0, 1),
		Vector3(0, 0, -1),
	};
	for (const Vector3 &direction : directions) {
		for (int distance : { 1, 2 }) {
			RegirGrid grid;
			CHECK_EQ(grid.configure(descriptor, Vector3()), OK);
			CHECK_EQ(grid.update(Vector3(), proposal, revisions), OK);
			RegirGridCellKey origin_key;
			CHECK_EQ(RegirGrid::world_position_to_cell(descriptor, Vector3(), origin_key), OK);
			const RegirGridCellState *before = find_cell(grid, origin_key);
			REQUIRE(before != nullptr);
			const RegirGridCellState before_copy = *before;

			CHECK_EQ(grid.recenter(direction * float(distance)), OK);
			const RegirGridCellState *after = find_cell(grid, origin_key);
			REQUIRE(after != nullptr);
			CHECK(after->valid);
			CHECK_EQ(after->proposal.selected_source_id, before_copy.proposal.selected_source_id);
			CHECK_EQ(after->proposal.selected_sample_id, before_copy.proposal.selected_sample_id);
			CHECK_EQ(after->proposal.selected_weight, before_copy.proposal.selected_weight);
			CHECK_EQ(after->proposal.candidate_count, before_copy.proposal.candidate_count);
			CHECK_EQ(after->proposal.age, before_copy.proposal.age);
			CHECK(after->revisions.matches(before_copy.revisions));
			CHECK_EQ(grid.get_diagnostics().last_scroll_preserved_cell_count, 125 - uint64_t(distance * 25));
			CHECK_EQ(grid.get_diagnostics().last_scroll_invalidated_cell_count, uint64_t(distance * 25));
		}
	}
}

TEST_CASE("[PathTracing][ReGIR] Newly exposed cells are invalidated while overlap survives") {
	RegirGrid grid;
	const RegirGridDescriptor descriptor = make_descriptor();
	CHECK_EQ(grid.configure(descriptor, Vector3()), OK);
	CHECK_EQ(grid.update(Vector3(2.0f, 0.0f, 0.0f), make_proposal(), make_revisions()), OK);
	CHECK_EQ(grid.recenter(Vector3(1.0f, 0.0f, 0.0f)), OK);

	RegirGridCellKey new_key;
	CHECK_EQ(RegirGrid::world_position_to_cell(descriptor, Vector3(3.0f, 0.0f, 0.0f), new_key), OK);
	const RegirGridCellState *new_cell = find_cell(grid, new_key);
	REQUIRE(new_cell != nullptr);
	CHECK_FALSE(new_cell->valid);

	RegirGridCellKey retained_key;
	CHECK_EQ(RegirGrid::world_position_to_cell(descriptor, Vector3(2.0f, 0.0f, 0.0f), retained_key), OK);
	const RegirGridCellState *retained_cell = find_cell(grid, retained_key);
	REQUIRE(retained_cell != nullptr);
	CHECK(retained_cell->valid);
}

TEST_CASE("[PathTracing][ReGIR] A no-overlap jump and descriptor change clear retained state") {
	RegirGrid grid;
	RegirGridDescriptor descriptor = make_descriptor();
	CHECK_EQ(grid.configure(descriptor, Vector3()), OK);
	CHECK_EQ(grid.update(Vector3(), make_proposal(), make_revisions()), OK);
	CHECK_EQ(grid.recenter(Vector3(5.0f, 0.0f, 0.0f)), OK);
	CHECK_EQ(grid.get_diagnostics().last_scroll_preserved_cell_count, 0);
	CHECK_EQ(grid.get_diagnostics().last_scroll_invalidated_cell_count, 125);
	CHECK_EQ(grid.get_diagnostics().cleared_cell_count, 125);
	for (const RegirGridCellState &cell : grid.get_cells()) {
		CHECK_FALSE(cell.valid);
	}

	descriptor.cell_size = 2.0f;
	CHECK_EQ(grid.configure(descriptor, Vector3()), OK);
	CHECK_EQ(grid.get_diagnostics().cleared_cell_count, 250);
	for (const RegirGridCellState &cell : grid.get_cells()) {
		CHECK_FALSE(cell.valid);
	}
}

TEST_CASE("[PathTracing][ReGIR] Proposals require current generations and caller-owned receiver visibility") {
	RegirGrid grid;
	const RegirGridDescriptor descriptor = make_descriptor();
	const RegirGridRevisions revisions = make_revisions();
	CHECK_EQ(grid.configure(descriptor, Vector3()), OK);
	CHECK_EQ(grid.update(Vector3(), make_proposal(), revisions), OK);

	RegirGridQuery query = grid.query(Vector3(), revisions);
	CHECK(query.hit);
	CHECK(query.requires_current_visibility_evaluation);
	CHECK_EQ(query.proposal.selected_sample_id, 101);
	CHECK_FALSE(caller_may_contribute(query, false));
	CHECK(caller_may_contribute(query, true));

	RegirGridRevisions changed = revisions;
	changed.visibility_residency_generation++;
	CHECK_FALSE(grid.query(Vector3(), changed).hit);
	CHECK_EQ(grid.get_diagnostics().revision_rejection_count, 1);
	changed = revisions;
	changed.light_distribution_generation++;
	CHECK_FALSE(grid.query(Vector3(), changed).hit);
	changed = revisions;
	changed.geometry_generation++;
	CHECK_FALSE(grid.query(Vector3(), changed).hit);
	changed = revisions;
	changed.environment_generation++;
	CHECK_FALSE(grid.query(Vector3(), changed).hit);
}

TEST_CASE("[PathTracing][ReGIR] Invalid inputs and descriptor extents are rejected without unbounded allocation") {
	RegirGridDescriptor descriptor = make_descriptor();
	descriptor.cells_x = 0;
	CHECK_EQ(RegirGrid::validate_descriptor(descriptor), ERR_INVALID_PARAMETER);
	descriptor = make_descriptor();
	descriptor.cell_size = 0.0f;
	CHECK_EQ(RegirGrid::validate_descriptor(descriptor), ERR_INVALID_PARAMETER);
	descriptor = make_descriptor();
	descriptor.world_origin.x = std::numeric_limits<float>::infinity();
	CHECK_EQ(RegirGrid::validate_descriptor(descriptor), ERR_INVALID_PARAMETER);
	descriptor = make_descriptor(1024, 1024, 1024);
	CHECK_EQ(RegirGrid::validate_descriptor(descriptor), ERR_OUT_OF_MEMORY);

	descriptor = make_descriptor();
	RegirGridCellKey key;
	CHECK_EQ(RegirGrid::world_position_to_cell(descriptor, Vector3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f), key), ERR_INVALID_PARAMETER);
	CHECK_EQ(RegirGrid::world_position_to_cell(descriptor, Vector3(std::numeric_limits<float>::max(), 0.0f, 0.0f), key), ERR_INVALID_PARAMETER);

	RegirGrid grid;
	CHECK_EQ(grid.configure(descriptor, Vector3()), OK);
	RegirGridProposal invalid_proposal = make_proposal();
	invalid_proposal.selected_weight = 0.0f;
	CHECK_EQ(grid.update(Vector3(), invalid_proposal, make_revisions()), ERR_INVALID_PARAMETER);
	RegirGridRevisions invalid_revisions = make_revisions();
	invalid_revisions.environment_generation = 0;
	CHECK_EQ(grid.update(Vector3(), make_proposal(), invalid_revisions), ERR_INVALID_PARAMETER);
}

TEST_CASE("[PathTracing][ReGIR] Checksums are deterministic and storage remains descriptor bounded") {
	const RegirGridDescriptor descriptor = make_descriptor(7, 3, 2);
	const RegirGridRevisions revisions = make_revisions();
	RegirGrid first;
	RegirGrid second;
	CHECK_EQ(first.configure(descriptor, Vector3(-1.0f, 2.0f, 3.0f)), OK);
	CHECK_EQ(second.configure(descriptor, Vector3(-1.0f, 2.0f, 3.0f)), OK);
	CHECK_EQ(first.update(Vector3(-1.0f, 2.0f, 3.0f), make_proposal(501), revisions), OK);
	CHECK_EQ(second.update(Vector3(-1.0f, 2.0f, 3.0f), make_proposal(501), revisions), OK);
	CHECK_EQ(first.recenter(Vector3(1.0f, 2.0f, 3.0f)), OK);
	CHECK_EQ(second.recenter(Vector3(1.0f, 2.0f, 3.0f)), OK);
	CHECK_EQ(first.get_cells().size(), 42);
	CHECK_EQ(second.get_cells().size(), 42);
	CHECK_EQ(first.get_diagnostics().checksum, second.get_diagnostics().checksum);
}

} // namespace TestPathTracingRegirGrid
