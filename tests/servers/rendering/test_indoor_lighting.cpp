/**************************************************************************/
/*  test_indoor_lighting.cpp                                              */
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

TEST_FORCE_LINK(test_indoor_lighting)

#include "servers/rendering/path_tracing/diffuse_radiance_cache.h"
#include "servers/rendering/path_tracing/indoor_lighting.h"

namespace TestIndoorLighting {

using namespace RendererPathTracing;

static LightSamplingInputRecord light_record(uint64_t p_source, uint64_t p_sample, float p_weight) {
	LightSamplingInputRecord result;
	result.source_id = p_source;
	result.sample_id = p_sample;
	result.type = LIGHT_SAMPLING_TYPE_EMISSIVE_TRIANGLE;
	result.domain = LIGHT_SAMPLING_DOMAIN_LOCAL;
	result.weight = p_weight;
	return result;
}

TEST_CASE("[PathTracing][IndoorLighting] Ray cones select non-zero mip LOD for valid textured triangles") {
	RayConeTriangleLodInput input;
	input.position[0] = Vector3(0, 0, 0);
	input.position[1] = Vector3(1, 0, 0);
	input.position[2] = Vector3(0, 1, 0);
	input.uv[0] = Vector2(0, 0);
	input.uv[1] = Vector2(1, 0);
	input.uv[2] = Vector2(0, 1);
	input.texture_width = 1024;
	input.texture_height = 1024;
	input.cone_width = 0.125f;
	input.max_lod = 10.0f;
	const RayConeTriangleLodResult result = ray_cone_triangle_mip_lod(input);
	CHECK(result.valid);
	CHECK(result.world_texel_size > 0.0f);
	CHECK(result.lod > 0.0f);
	CHECK(result.lod <= input.max_lod);
	input.uv[2] = input.uv[1];
	CHECK_FALSE(ray_cone_triangle_mip_lod(input).valid);
	input.uv[2] = Vector2(0, 1);
	input.texture_is_constant = true;
	CHECK_FALSE(ray_cone_triangle_mip_lod(input).valid);
}

TEST_CASE("[PathTracing][IndoorLighting] Power-area distributions preserve identities and feed bounded reservoir cells") {
	CHECK_EQ(light_sampling_triangle_power_weight(4.0f, 3.0f), 12.0f);
	CHECK_EQ(light_sampling_triangle_power_weight(-1.0f, 3.0f), 0.0f);
	Vector<LightSamplingInputRecord> input;
	input.push_back(light_record(10, 11, light_sampling_triangle_power_weight(1.0f, 1.0f)));
	input.push_back(light_record(20, 21, light_sampling_triangle_power_weight(2.0f, 3.0f)));
	LightSamplingIdentityTracker tracker;
	LightSamplingBuildResult distribution;
	CHECK_EQ(tracker.build(input, distribution), OK);
	CHECK_EQ(distribution.records[0].pdf, doctest::Approx(1.0f / 7.0f));
	CHECK_EQ(distribution.records[1].pdf, doctest::Approx(6.0f / 7.0f));
	LightSamplingBuildResult next;
	CHECK_EQ(tracker.build(input, next), OK);
	CHECK_EQ(next.records[0].previous_index, 0);
	CHECK_EQ(next.records[1].previous_index, 1);

	LocalLightProposalCellInput cell_input;
	cell_input.cell_id = 100;
	cell_input.maximum_candidates = 2;
	cell_input.record_indices.push_back(0);
	cell_input.record_indices.push_back(1);
	LocalLightProposalCell cell;
	CHECK_EQ(build_local_light_proposal_cell(distribution, cell_input, cell), OK);
	CHECK(cell.selectable);
	CHECK_EQ(cell.pdf[0] + cell.pdf[1], doctest::Approx(1.0f));
	CHECK_EQ(sample_local_light_proposal_cell(cell, 0.9f).record_index, 1);

	LightSamplingReservoir reservoir = make_invalid_light_sampling_reservoir();
	LightSamplingReservoirCandidate candidate;
	candidate.record_index = 1;
	candidate.source_id = distribution.records[1].source_id;
	candidate.sample_id = distribution.records[1].sample_id;
	candidate.target = 2.0f;
	candidate.proposal_pdf = cell.pdf[1];
	CHECK(update_light_sampling_reservoir(candidate, 0.0f, reservoir));
	CHECK((reservoir.flags & LIGHT_SAMPLING_RESERVOIR_VALID) != 0);
	CHECK_EQ(reservoir.selected_index, 1);
	CHECK_FALSE(update_light_sampling_reservoir(LightSamplingReservoirCandidate(), 0.0f, reservoir));
	Vector<LightSamplingViewReservoirs> per_view;
	CHECK_EQ(initialize_light_sampling_view_reservoirs(2, 3, per_view), OK);
	CHECK_EQ(per_view.size(), 2);
	CHECK_EQ(per_view[0].view_index, 0);
	CHECK_EQ(per_view[1].view_index, 1);
	CHECK_EQ(per_view[0].reservoirs.size(), 3);
	CHECK_EQ(per_view[1].reservoirs.size(), 3);
	CHECK(update_light_sampling_reservoir(candidate, 0.0f, per_view.write[0].reservoirs.write[0]));
	CHECK_EQ(per_view[1].reservoirs[0].selected_index, LIGHT_SAMPLING_INVALID_INDEX);
}

TEST_CASE("[PathTracing][IndoorLighting] Diffuse cache rejects a snapped-grid origin change") {
	DiffuseRadianceCache cache;
	DiffuseRadianceCacheRevisions revisions;
	revisions.geometry = 1;
	revisions.material = 2;
	revisions.light = 3;
	revisions.environment = 4;
	DiffuseRadianceCacheKey key = make_diffuse_radiance_cache_key(Vector3(2, 3, 4), Vector3(0, 1, 0), 1.0f);
	CHECK(cache.update(key, revisions, Vector3(1, 2, 3), 1));
	CHECK(cache.query(key, revisions).hit);
	revisions.grid_origin_x++;
	CHECK_FALSE(cache.query(key, revisions).hit);
}

TEST_CASE("[PathTracing][IndoorLighting] Portal proposals have normalized technique weights and finite mixture PDFs") {
	EnvironmentPortal portal;
	portal.portal_id = 7;
	portal.center = Vector3(0, 0, 2);
	portal.axis_u = Vector3(1, 0, 0);
	portal.axis_v = Vector3(0, 1, 0);
	portal.weight = 1.0f;
	Vector<EnvironmentPortal> portals;
	portals.push_back(portal);
	EnvironmentPortalMixture mixture;
	CHECK_EQ(build_environment_portal_mixture(1.0f, portals, mixture), OK);
	CHECK(mixture.selectable);
	CHECK_EQ(mixture.environment_selection_pdf + mixture.portal_selection_pdf[0], doctest::Approx(1.0f));
	const EnvironmentPortalSample sample = sample_environment_portal(mixture, Vector3(), 0.75f, Vector2(0.5f, 0.5f), 0.2f);
	CHECK(sample.valid);
	CHECK_EQ(sample.portal_index, 0);
	CHECK(sample.portal_pdf_solid_angle > 0.0f);
	CHECK_EQ(sample.mixture_pdf_solid_angle, doctest::Approx(0.5f * 0.2f + 0.5f * sample.portal_pdf_solid_angle));
	CHECK_EQ(environment_portal_mis_balance_weight(sample.mixture_pdf_solid_angle, sample.portal_pdf_solid_angle) + environment_portal_mis_balance_weight(sample.portal_pdf_solid_angle, sample.mixture_pdf_solid_angle), doctest::Approx(1.0f));
}

TEST_CASE("[PathTracing][IndoorLighting] Diffuse cache is scene-revisioned and adaptive budgets stay bounded") {
	const DiffuseRadianceCacheKey key = make_diffuse_radiance_cache_key(Vector3(0.25f, 0.25f, 0.25f), Vector3(0, 1, 0), 1.0f);
	const DiffuseRadianceCacheKey same_key = make_diffuse_radiance_cache_key(Vector3(0.75f, 0.75f, 0.75f), Vector3(0, 1, 0), 1.0f);
	CHECK(key == same_key);
	DiffuseRadianceCacheRevisions revisions;
	revisions.geometry = 1;
	revisions.material = 2;
	revisions.light = 3;
	revisions.environment = 4;
	DiffuseRadianceCache cache;
	CHECK(cache.update(key, revisions, Vector3(1, 2, 3), 9));
	CHECK(cache.query(key, revisions).hit);
	DiffuseRadianceCacheRevisions changed = revisions;
	changed.light++;
	CHECK_FALSE(cache.query(key, changed).hit);
	CHECK_EQ(cache.get_metrics().revision_rejection_count, 1);

	TransportAdaptiveBudgetInput budget_input;
	budget_input.minimum_samples = 2;
	budget_input.maximum_samples = 6;
	budget_input.variance = 100.0f;
	budget_input.variance_reference = 1.0f;
	const TransportAdaptiveBudget high = compute_transport_adaptive_budget(budget_input);
	CHECK_EQ(high.direct_samples, 6);
	CHECK(high.diffuse_samples >= 2);
	CHECK(high.diffuse_samples <= 6);
	budget_input.variance = -1.0f;
	budget_input.reservoir_valid = true;
	budget_input.cache_valid = true;
	const TransportAdaptiveBudget low = compute_transport_adaptive_budget(budget_input);
	CHECK_EQ(low.direct_samples, 2);
	CHECK_EQ(low.diffuse_samples, 2);
}

} // namespace TestIndoorLighting
