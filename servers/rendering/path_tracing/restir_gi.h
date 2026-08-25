/**************************************************************************/
/*  restir_gi.h                                                          */
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

#pragma once

#include "core/error/error_list.h"
#include "core/templates/vector.h"
#include "servers/rendering/path_tracing/reusable_path_sample.h"

#include <cstdint>

namespace RendererPathTracing {

// Backend-neutral reference contract for diffuse, one-bounce ReSTIR GI.
// All proposal PDFs are densities over the solid angle at the *current*
// primary receiver. The selected transport sample is re-evaluated there.
// This neither represents glossy transport nor paths beyond one diffuse bounce.
static constexpr uint32_t RESTIR_GI_REFERENCE_ABI_VERSION = 1;

enum RestirGiCandidateKind : uint32_t {
	RESTIR_GI_CANDIDATE_FRESH = 1,
	RESTIR_GI_CANDIDATE_REUSED = 2,
};

enum RestirGiRejectionReason : uint32_t {
	RESTIR_GI_REJECT_NONE = 0,
	RESTIR_GI_REJECT_PATH_SAMPLE = 1U << 0,
	RESTIR_GI_REJECT_TARGET_NOT_EVALUATED = 1U << 1,
	RESTIR_GI_REJECT_VISIBILITY_NOT_EVALUATED = 1U << 2,
	RESTIR_GI_REJECT_NOT_VISIBLE = 1U << 3,
	RESTIR_GI_REJECT_NONPOSITIVE_OR_NONFINITE_TARGET = 1U << 4,
	RESTIR_GI_REJECT_NONPOSITIVE_PROPOSAL = 1U << 5,
	RESTIR_GI_REJECT_NONFINITE_WEIGHT = 1U << 6,
	RESTIR_GI_REJECT_INVALID_REPRESENTED_M = 1U << 7,
	RESTIR_GI_REJECT_INVALID_REPLAY = 1U << 8,
};

// These values must be produced by the current receiver. They intentionally
// contain no cached radiance or cached visibility field. A client cannot turn
// a reusable sample into a contribution without supplying both evaluations.
struct RestirGiCurrentEvaluation {
	bool target_evaluated = false;
	bool visibility_evaluated = false;
	bool visible = false;
	float target = 0.0f;
};

struct RestirGiReplay {
	uint32_t abi_version = RESTIR_GI_REFERENCE_ABI_VERSION;
	uint64_t sequence_key = 0;
	uint64_t selection_seed = 0;
};

struct RestirGiCandidate {
	ReusablePathSampleGpuRecord path_sample;
	RestirGiCandidateKind kind = RESTIR_GI_CANDIDATE_FRESH;
	// Number of source proposals represented by this selected proposal. A
	// fresh candidate represents one. A reused reservoir carries its M.
	uint32_t represented_m = 1;
	float current_primary_solid_angle_pdf = 0.0f;
	float current_target = 0.0f;
	float ris_weight = 0.0f;
	uint64_t source_identity_checksum = 0;
	uint64_t replay_checksum = 0;
	RestirGiReplay replay;
	uint32_t rejection_reasons = RESTIR_GI_REJECT_NONE;

	bool is_eligible() const { return rejection_reasons == RESTIR_GI_REJECT_NONE; }
};

struct RestirGiBuildParameters {
	uint32_t abi_version = RESTIR_GI_REFERENCE_ABI_VERSION;
	uint32_t maximum_reused_candidates = 0;
	uint32_t maximum_represented_m = 0;
};

struct RestirGiReservoir {
	uint32_t abi_version = RESTIR_GI_REFERENCE_ABI_VERSION;
	bool valid = false;
	RestirGiCandidate selected;
	uint32_t represented_m = 0;
	uint32_t accepted_candidate_count = 0;
	uint32_t attempted_reused_candidate_count = 0;
	float weight_sum = 0.0f;
	// W / (M * p_hat(y_selected)). With a visible selected sample, its scalar
	// reference estimate is p_hat(y_selected) * normalization = W / M.
	float normalization = 0.0f;
	uint64_t replay_checksum = 0;
};

// Validates the reusable world sample against the supplied current state,
// converts its source solid-angle proposal through secondary area measure to
// current-primary solid angle, and consumes a current target/visibility result.
// It does not reuse cached radiance or visibility from the path payload.
RestirGiCandidate restir_gi_evaluate_candidate(const ReusablePathSampleGpuRecord &p_path_sample, const ReusablePathSampleCurrentState &p_current, const ReusablePathSampleValidationThresholds &p_thresholds, const RestirGiCurrentEvaluation &p_evaluation, RestirGiCandidateKind p_kind, uint32_t p_represented_m, const RestirGiReplay &p_replay);

// Reservoir update uses RIS weights w_i = M_i * p_hat_i / q_i, with q_i in
// current receiver solid-angle measure. It accepts at most one fresh candidate
// and the configured number of reused candidates. Selection is deterministic
// from p_replay and candidate replay data; it is not backend RNG state.
RestirGiReservoir restir_gi_build_reservoir(const RestirGiCandidate *p_fresh_candidate, const Vector<RestirGiCandidate> &p_reused_candidates, const RestirGiBuildParameters &p_parameters, const RestirGiReplay &p_replay);

// Returns false unless the selected candidate was made from a current target
// and current visibility evaluation. `r_estimate` is W/M for the scalar
// reference integrand. It intentionally has no radiance payload.
bool restir_gi_reservoir_reference_estimate(const RestirGiReservoir &p_reservoir, float &r_estimate);

uint64_t restir_gi_replay_checksum(const RestirGiReplay &p_replay);

} // namespace RendererPathTracing
