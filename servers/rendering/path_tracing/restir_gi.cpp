/**************************************************************************/
/*  restir_gi.cpp                                                        */
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
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "restir_gi.h"

#include "core/math/math_funcs.h"

#include <cmath>

namespace RendererPathTracing {

namespace {

static uint64_t _fnv_append_u32(uint64_t p_hash, uint32_t p_value) {
	for (uint32_t byte = 0; byte < 4; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= 1099511628211ULL;
	}
	return p_hash;
}

static uint64_t _fnv_append_u64(uint64_t p_hash, uint64_t p_value) {
	for (uint32_t byte = 0; byte < 8; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= 1099511628211ULL;
	}
	return p_hash;
}

static uint64_t _mix64(uint64_t p_value) {
	p_value += 0x9e3779b97f4a7c15ULL;
	p_value = (p_value ^ (p_value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	p_value = (p_value ^ (p_value >> 27)) * 0x94d049bb133111ebULL;
	return p_value ^ (p_value >> 31);
}

static float _selection_uniform(const RestirGiReplay &p_replay, const RestirGiCandidate &p_candidate, uint32_t p_ordinal) {
	const uint64_t mixed = _mix64(p_replay.selection_seed ^ p_replay.sequence_key ^ p_candidate.replay_checksum ^ uint64_t(p_ordinal));
	// Map the top 24 bits to [0, 1). The exact range is stable across backends.
	return float(mixed >> 40) * (1.0f / 16777216.0f);
}

static bool _valid_replay(const RestirGiReplay &p_replay) {
	return p_replay.abi_version == RESTIR_GI_REFERENCE_ABI_VERSION;
}

static bool _valid_parameters(const RestirGiBuildParameters &p_parameters) {
	return p_parameters.abi_version == RESTIR_GI_REFERENCE_ABI_VERSION && p_parameters.maximum_represented_m > 0;
}

static uint64_t _reservoir_checksum(const RestirGiReservoir &p_reservoir, const RestirGiReplay &p_replay) {
	uint64_t hash = 1469598103934665603ULL;
	hash = _fnv_append_u64(hash, restir_gi_replay_checksum(p_replay));
	hash = _fnv_append_u64(hash, p_reservoir.selected.source_identity_checksum);
	hash = _fnv_append_u64(hash, p_reservoir.selected.replay_checksum);
	hash = _fnv_append_u32(hash, p_reservoir.represented_m);
	hash = _fnv_append_u32(hash, p_reservoir.accepted_candidate_count);
	return hash;
}

static void _consider_candidate(const RestirGiCandidate &p_candidate, uint32_t p_ordinal, const RestirGiReplay &p_replay, RestirGiReservoir &r_reservoir) {
	if (!p_candidate.is_eligible()) {
		return;
	}
	const float new_weight_sum = r_reservoir.weight_sum + p_candidate.ris_weight;
	if (!Math::is_finite(new_weight_sum) || new_weight_sum <= 0.0f) {
		return;
	}
	const float selection = _selection_uniform(p_replay, p_candidate, p_ordinal);
	if (!r_reservoir.valid || selection * new_weight_sum < p_candidate.ris_weight) {
		r_reservoir.selected = p_candidate;
		r_reservoir.valid = true;
	}
	r_reservoir.weight_sum = new_weight_sum;
	r_reservoir.represented_m += p_candidate.represented_m;
	r_reservoir.accepted_candidate_count++;
}

} // namespace

uint64_t restir_gi_replay_checksum(const RestirGiReplay &p_replay) {
	uint64_t hash = 1469598103934665603ULL;
	hash = _fnv_append_u32(hash, p_replay.abi_version);
	hash = _fnv_append_u64(hash, p_replay.sequence_key);
	hash = _fnv_append_u64(hash, p_replay.selection_seed);
	return hash;
}

RestirGiCandidate restir_gi_evaluate_candidate(const ReusablePathSampleGpuRecord &p_path_sample, const ReusablePathSampleCurrentState &p_current, const ReusablePathSampleValidationThresholds &p_thresholds, const RestirGiCurrentEvaluation &p_evaluation, RestirGiCandidateKind p_kind, uint32_t p_represented_m, const RestirGiReplay &p_replay) {
	RestirGiCandidate candidate;
	candidate.path_sample = p_path_sample;
	candidate.kind = p_kind;
	candidate.represented_m = p_represented_m;
	candidate.replay = p_replay;
	candidate.source_identity_checksum = reusable_path_sample_identity_checksum(p_path_sample);
	candidate.replay_checksum = reusable_path_sample_replay_checksum(p_path_sample) ^ restir_gi_replay_checksum(p_replay);

	if (p_kind != RESTIR_GI_CANDIDATE_FRESH && p_kind != RESTIR_GI_CANDIDATE_REUSED) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_INVALID_REPLAY;
	}
	if (!_valid_replay(p_replay)) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_INVALID_REPLAY;
	}
	if (p_represented_m == 0 || (p_kind == RESTIR_GI_CANDIDATE_FRESH && p_represented_m != 1)) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_INVALID_REPRESENTED_M;
	}
	const ReusablePathSampleValidationResult path_validation = validate_reusable_path_sample(p_path_sample, p_current, p_thresholds);
	if (!path_validation.may_reuse_after_revalidation) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_PATH_SAMPLE;
	}
	if (!p_evaluation.target_evaluated) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_TARGET_NOT_EVALUATED;
	}
	if (!p_evaluation.visibility_evaluated) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_VISIBILITY_NOT_EVALUATED;
	}
	if (p_evaluation.visibility_evaluated && !p_evaluation.visible) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_NOT_VISIBLE;
	}
	if (!Math::is_finite(p_evaluation.target) || p_evaluation.target <= 0.0f) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_NONPOSITIVE_OR_NONFINITE_TARGET;
	}
	if (!reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(p_path_sample, p_current.source_primary, candidate.current_primary_solid_angle_pdf) || !Math::is_finite(candidate.current_primary_solid_angle_pdf) || candidate.current_primary_solid_angle_pdf <= 0.0f) {
		candidate.rejection_reasons |= RESTIR_GI_REJECT_NONPOSITIVE_PROPOSAL;
	}
	candidate.current_target = p_evaluation.target;
	if (candidate.rejection_reasons == RESTIR_GI_REJECT_NONE) {
		candidate.ris_weight = float(p_represented_m) * candidate.current_target / candidate.current_primary_solid_angle_pdf;
		if (!Math::is_finite(candidate.ris_weight) || candidate.ris_weight < 0.0f) {
			candidate.rejection_reasons |= RESTIR_GI_REJECT_NONFINITE_WEIGHT;
		}
	}
	return candidate;
}

RestirGiReservoir restir_gi_build_reservoir(const RestirGiCandidate *p_fresh_candidate, const Vector<RestirGiCandidate> &p_reused_candidates, const RestirGiBuildParameters &p_parameters, const RestirGiReplay &p_replay) {
	RestirGiReservoir reservoir;
	if (!_valid_parameters(p_parameters) || !_valid_replay(p_replay)) {
		return reservoir;
	}
	uint32_t ordinal = 0;
	if (p_fresh_candidate && p_fresh_candidate->kind == RESTIR_GI_CANDIDATE_FRESH && p_fresh_candidate->represented_m <= p_parameters.maximum_represented_m) {
		_consider_candidate(*p_fresh_candidate, ordinal++, p_replay, reservoir);
	}
	for (uint32_t i = 0; i < p_reused_candidates.size() && reservoir.attempted_reused_candidate_count < p_parameters.maximum_reused_candidates; i++) {
		const RestirGiCandidate &candidate = p_reused_candidates[i];
		reservoir.attempted_reused_candidate_count++;
		if (candidate.kind != RESTIR_GI_CANDIDATE_REUSED || candidate.represented_m > p_parameters.maximum_represented_m) {
			continue;
		}
		_consider_candidate(candidate, ordinal++, p_replay, reservoir);
	}
	if (!reservoir.valid || reservoir.represented_m == 0 || !Math::is_finite(reservoir.weight_sum) || reservoir.weight_sum <= 0.0f || !Math::is_finite(reservoir.selected.current_target) || reservoir.selected.current_target <= 0.0f) {
		reservoir = RestirGiReservoir();
		return reservoir;
	}
	reservoir.normalization = reservoir.weight_sum / (float(reservoir.represented_m) * reservoir.selected.current_target);
	if (!Math::is_finite(reservoir.normalization) || reservoir.normalization <= 0.0f) {
		return RestirGiReservoir();
	}
	reservoir.replay_checksum = _reservoir_checksum(reservoir, p_replay);
	return reservoir;
}

bool restir_gi_reservoir_reference_estimate(const RestirGiReservoir &p_reservoir, float &r_estimate) {
	r_estimate = 0.0f;
	if (p_reservoir.abi_version != RESTIR_GI_REFERENCE_ABI_VERSION || !p_reservoir.valid || !p_reservoir.selected.is_eligible() || p_reservoir.represented_m == 0 || !Math::is_finite(p_reservoir.weight_sum) || p_reservoir.weight_sum <= 0.0f) {
		return false;
	}
	r_estimate = p_reservoir.weight_sum / float(p_reservoir.represented_m);
	return Math::is_finite(r_estimate) && r_estimate >= 0.0f;
}

} // namespace RendererPathTracing
