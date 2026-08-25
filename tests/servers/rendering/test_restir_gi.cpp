/**************************************************************************/
/*  test_restir_gi.cpp                                                   */
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
/* permit persons to whom the Software is furnished to do so, subject to   */
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
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_restir_gi)

#include "servers/rendering/path_tracing/restir_gi.h"

#include <limits>

namespace TestRestirGi {

using namespace RendererPathTracing;

static ReusablePathSampleAuthoring make_authoring() {
	ReusablePathSampleAuthoring authoring;
	authoring.source_primary.geometry_instance_id = 11;
	authoring.source_primary.material_id = 12;
	authoring.source_primary.surface_id = 1;
	authoring.source_primary.primitive_id = 2;
	authoring.source_primary.visibility_mask = 0x3;
	authoring.source_primary.revisions.geometry = 101;
	authoring.source_primary.revisions.material = 102;
	authoring.source_primary.revisions.residency = 103;
	authoring.source_primary.world_position = Vector3(0.0f, 0.0f, 0.0f);
	authoring.source_primary.geometric_normal = Vector3(0.0f, 1.0f, 0.0f);
	authoring.source_primary.shading_normal = Vector3(0.0f, 1.0f, 0.0f);
	authoring.secondary.geometry_instance_id = 21;
	authoring.secondary.material_id = 22;
	authoring.secondary.surface_id = 3;
	authoring.secondary.primitive_id = 4;
	authoring.secondary.visibility_mask = 0x5;
	authoring.secondary.revisions.geometry = 201;
	authoring.secondary.revisions.material = 202;
	authoring.secondary.revisions.residency = 203;
	authoring.secondary.world_position = Vector3(0.0f, 2.0f, 0.0f);
	authoring.secondary.geometric_normal = Vector3(0.0f, -1.0f, 0.0f);
	authoring.secondary.shading_normal = Vector3(0.0f, -1.0f, 0.0f);
	authoring.capture_frame = 100;
	authoring.age = 0;
	authoring.lighting_revision = 301;
	authoring.environment_revision = 302;
	authoring.throughput = Color(1.0f, 1.0f, 1.0f, 1.0f);
	authoring.incident_radiance = Color(1.0f, 1.0f, 1.0f, 1.0f);
	authoring.outgoing_radiance = Color(999.0f, 999.0f, 999.0f, 1.0f); // Must not affect the reference estimator.
	authoring.source_primary_proposal_solid_angle_pdf = 1.0f;
	authoring.target = 123.0f; // Cached target is not consulted by ReSTIR GI.
	authoring.normalization = 456.0f;
	return authoring;
}

static ReusablePathSampleCurrentState make_current() {
	const ReusablePathSampleAuthoring authoring = make_authoring();
	ReusablePathSampleCurrentState current;
	current.source_primary = authoring.source_primary;
	current.secondary = authoring.secondary;
	current.current_frame = 100;
	current.lighting_revision = authoring.lighting_revision;
	current.environment_revision = authoring.environment_revision;
	return current;
}

static ReusablePathSampleValidationThresholds make_thresholds() {
	ReusablePathSampleValidationThresholds thresholds;
	thresholds.maximum_age = 2;
	thresholds.maximum_source_primary_world_position_distance = 1.0f;
	thresholds.maximum_secondary_world_position_distance = 0.1f;
	thresholds.minimum_geometric_normal_dot = 0.99f;
	thresholds.minimum_shading_normal_dot = 0.99f;
	return thresholds;
}

static RestirGiReplay make_replay() {
	RestirGiReplay replay;
	replay.sequence_key = 0x12345678ULL;
	replay.selection_seed = 0x87654321ULL;
	return replay;
}

static RestirGiCurrentEvaluation visible_target(float p_target) {
	RestirGiCurrentEvaluation evaluation;
	evaluation.target_evaluated = true;
	evaluation.visibility_evaluated = true;
	evaluation.visible = true;
	evaluation.target = p_target;
	return evaluation;
}

static RestirGiBuildParameters make_parameters() {
	RestirGiBuildParameters parameters;
	parameters.maximum_reused_candidates = 4;
	parameters.maximum_represented_m = 16;
	return parameters;
}

static RestirGiCandidate make_candidate(float p_target, RestirGiCandidateKind p_kind = RESTIR_GI_CANDIDATE_FRESH, uint32_t p_m = 1) {
	return restir_gi_evaluate_candidate(reusable_path_sample_gpu_from_authoring(make_authoring()), make_current(), make_thresholds(), visible_target(p_target), p_kind, p_m, make_replay());
}

TEST_CASE("[PathTracing][ReSTIRGI] one-bounce diffuse candidate uses current receiver solid-angle measure") {
	const RestirGiCandidate candidate = make_candidate(2.0f);
	CHECK(candidate.is_eligible());
	// source q_omega=1, r=2, both cosines are one: source area q=1/4,
	// then current q_omega=(1/4)*4=1.
	CHECK(Math::is_equal_approx(candidate.current_primary_solid_angle_pdf, 1.0f));
	CHECK(Math::is_equal_approx(candidate.ris_weight, 2.0f));
	CHECK_EQ(candidate.represented_m, 1U);
	CHECK_NE(candidate.source_identity_checksum, 0ULL);
}

TEST_CASE("[PathTracing][ReSTIRGI] fresh and reused candidates propagate hand-calculated RIS weights and M") {
	const RestirGiCandidate fresh = make_candidate(2.0f);
	const RestirGiCandidate reused = make_candidate(3.0f, RESTIR_GI_CANDIDATE_REUSED, 2);
	Vector<RestirGiCandidate> reused_candidates;
	reused_candidates.push_back(reused);
	const RestirGiReservoir reservoir = restir_gi_build_reservoir(&fresh, reused_candidates, make_parameters(), make_replay());
	CHECK(reservoir.valid);
	CHECK_EQ(reservoir.represented_m, 3U);
	CHECK_EQ(reservoir.accepted_candidate_count, 2U);
	CHECK(Math::is_equal_approx(reservoir.weight_sum, 8.0f));
	CHECK(Math::is_equal_approx(reservoir.normalization, reservoir.weight_sum / (3.0f * reservoir.selected.current_target)));
	float estimate = 0.0f;
	CHECK(restir_gi_reservoir_reference_estimate(reservoir, estimate));
	CHECK(Math::is_equal_approx(estimate, 8.0f / 3.0f));
}

TEST_CASE("[PathTracing][ReSTIRGI] deterministic selection retains the only eligible proposal endpoint") {
	const RestirGiCandidate fresh = make_candidate(4.0f);
	const RestirGiCandidate invalid_reused = make_candidate(0.0f, RESTIR_GI_CANDIDATE_REUSED, 3);
	Vector<RestirGiCandidate> reused;
	reused.push_back(invalid_reused);
	const RestirGiReservoir first = restir_gi_build_reservoir(&fresh, reused, make_parameters(), make_replay());
	const RestirGiReservoir second = restir_gi_build_reservoir(&fresh, reused, make_parameters(), make_replay());
	CHECK(first.valid);
	CHECK_EQ(first.selected.replay_checksum, fresh.replay_checksum);
	CHECK_EQ(first.replay_checksum, second.replay_checksum);
	CHECK_EQ(first.selected.source_identity_checksum, reusable_path_sample_identity_checksum(fresh.path_sample));
}

TEST_CASE("[PathTracing][ReSTIRGI] current target and visibility are mandatory and cached path payload cannot authorize contribution") {
	const ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	RestirGiCurrentEvaluation evaluation = visible_target(2.0f);
	evaluation.target_evaluated = false;
	RestirGiCandidate candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), evaluation, RESTIR_GI_CANDIDATE_FRESH, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_TARGET_NOT_EVALUATED) != 0);
	evaluation = visible_target(2.0f);
	evaluation.visibility_evaluated = false;
	candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), evaluation, RESTIR_GI_CANDIDATE_FRESH, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_VISIBILITY_NOT_EVALUATED) != 0);
	evaluation = visible_target(2.0f);
	evaluation.visible = false;
	candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), evaluation, RESTIR_GI_CANDIDATE_FRESH, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_NOT_VISIBLE) != 0);
}

TEST_CASE("[PathTracing][ReSTIRGI] source secondary revisions masks and dynamic or disoccluded state reject reusable paths") {
	const ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	ReusablePathSampleCurrentState current = make_current();
	current.secondary.revisions.geometry++;
	RestirGiCandidate candidate = restir_gi_evaluate_candidate(sample, current, make_thresholds(), visible_target(1.0f), RESTIR_GI_CANDIDATE_REUSED, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_PATH_SAMPLE) != 0);
	current = make_current();
	current.secondary.visibility_mask = 0;
	candidate = restir_gi_evaluate_candidate(sample, current, make_thresholds(), visible_target(1.0f), RESTIR_GI_CANDIDATE_REUSED, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	current = make_current();
	current.source_primary.dynamic_geometry = true;
	candidate = restir_gi_evaluate_candidate(sample, current, make_thresholds(), visible_target(1.0f), RESTIR_GI_CANDIDATE_REUSED, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	current = make_current();
	current.secondary.disoccluded = true;
	candidate = restir_gi_evaluate_candidate(sample, current, make_thresholds(), visible_target(1.0f), RESTIR_GI_CANDIDATE_REUSED, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
}

TEST_CASE("[PathTracing][ReSTIRGI] lighting and environment revisions require current re-evaluation but do not invalidate a valid proposal") {
	ReusablePathSampleCurrentState current = make_current();
	current.lighting_revision++;
	current.environment_revision++;
	const RestirGiCandidate candidate = restir_gi_evaluate_candidate(reusable_path_sample_gpu_from_authoring(make_authoring()), current, make_thresholds(), visible_target(7.0f), RESTIR_GI_CANDIDATE_REUSED, 2, make_replay());
	CHECK(candidate.is_eligible());
	CHECK(Math::is_equal_approx(candidate.current_target, 7.0f));
}

TEST_CASE("[PathTracing][ReSTIRGI] zero and nonfinite proposal or target inputs fail closed") {
	ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	sample.source_primary_proposal_solid_angle_pdf = 0.0f;
	RestirGiCandidate candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), visible_target(1.0f), RESTIR_GI_CANDIDATE_FRESH, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_NONPOSITIVE_PROPOSAL) != 0);
	sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), visible_target(std::numeric_limits<float>::infinity()), RESTIR_GI_CANDIDATE_FRESH, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_NONPOSITIVE_OR_NONFINITE_TARGET) != 0);
	candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), visible_target(0.0f), RESTIR_GI_CANDIDATE_FRESH, 1, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_NONPOSITIVE_OR_NONFINITE_TARGET) != 0);
	candidate = restir_gi_evaluate_candidate(sample, make_current(), make_thresholds(), visible_target(std::numeric_limits<float>::max()), RESTIR_GI_CANDIDATE_REUSED, 16, make_replay());
	CHECK_FALSE(candidate.is_eligible());
	CHECK((candidate.rejection_reasons & RESTIR_GI_REJECT_NONFINITE_WEIGHT) != 0);
}

TEST_CASE("[PathTracing][ReSTIRGI] constant integrand reservoir matches reference Monte Carlo estimate") {
	Vector<RestirGiCandidate> reused;
	for (uint32_t i = 0; i < 63; i++) {
		reused.push_back(make_candidate(2.5f, RESTIR_GI_CANDIDATE_REUSED, 1));
	}
	RestirGiBuildParameters parameters = make_parameters();
	parameters.maximum_reused_candidates = 63;
	parameters.maximum_represented_m = 1;
	const RestirGiCandidate fresh = make_candidate(2.5f);
	const RestirGiReservoir reservoir = restir_gi_build_reservoir(&fresh, reused, parameters, make_replay());
	float estimate = 0.0f;
	CHECK(restir_gi_reservoir_reference_estimate(reservoir, estimate));
	CHECK_EQ(reservoir.represented_m, 64U);
	CHECK(Math::is_equal_approx(estimate, 2.5f));
}

} // namespace TestRestirGi
