/**************************************************************************/
/*  light_sampling.cpp                                                    */
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

#include "light_sampling.h"

#include "core/math/math_funcs.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace RendererPathTracing {

namespace {

static Error _fail(Error p_error, const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static bool _is_type_domain_valid(uint32_t p_type, uint32_t p_domain) {
	return ((p_type == LIGHT_SAMPLING_TYPE_ANALYTIC || p_type == LIGHT_SAMPLING_TYPE_EMISSIVE_TRIANGLE) && p_domain == LIGHT_SAMPLING_DOMAIN_LOCAL) ||
			(p_type == LIGHT_SAMPLING_TYPE_ENVIRONMENT && p_domain == LIGHT_SAMPLING_DOMAIN_ENVIRONMENT);
}

struct RecordOrder {
	bool operator()(const LightSamplingGpuRecord &p_a, const LightSamplingGpuRecord &p_b) const {
		if (p_a.domain != p_b.domain) {
			return p_a.domain < p_b.domain;
		}
		if (p_a.type != p_b.type) {
			return p_a.type < p_b.type;
		}
		if (p_a.source_id != p_b.source_id) {
			return p_a.source_id < p_b.source_id;
		}
		return p_a.sample_id < p_b.sample_id;
	}
};

static void _hash_bytes(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; i++) {
		r_hash ^= bytes[i];
		r_hash *= 1099511628211ULL;
	}
}

static void _build_distribution(LightSamplingBuildResult &r_result, LightSamplingDistribution &r_distribution, uint32_t p_domain) {
	r_distribution = LightSamplingDistribution();
	r_distribution.domain = p_domain;
	double total_weight = 0.0;
	for (int i = 0; i < r_result.records.size(); i++) {
		LightSamplingGpuRecord &record = r_result.records.write[i];
		if (record.domain != p_domain) {
			continue;
		}
		if (r_distribution.record_count == 0) {
			r_distribution.first_record = i;
		}
		r_distribution.record_count++;
		total_weight += record.weight;
	}
	if (r_distribution.record_count == 0 || total_weight <= 0.0 || !std::isfinite(total_weight)) {
		return;
	}

	r_distribution.selectable = true;
	r_distribution.total_weight = total_weight > std::numeric_limits<float>::max() ? std::numeric_limits<float>::max() : (float)total_weight;
	r_distribution.cdf.resize(r_distribution.record_count);
	double cumulative = 0.0;
	for (uint32_t i = 0; i < r_distribution.record_count; i++) {
		LightSamplingGpuRecord &record = r_result.records.write[r_distribution.first_record + i];
		record.pdf = (float)(record.weight / total_weight);
		cumulative += record.pdf;
		record.cdf = i + 1 == r_distribution.record_count ? 1.0f : (float)cumulative;
		r_distribution.cdf.write[i] = record.cdf;
	}
	r_distribution.normalization_error = Math::abs(r_distribution.cdf[r_distribution.record_count - 1] - 1.0f);
}

} // namespace

void LightSamplingIdentityTracker::clear() {
	previous_indices.clear();
}

Error LightSamplingIdentityTracker::build(const Vector<LightSamplingInputRecord> &p_input, LightSamplingBuildResult &r_result, String *r_error) {
	r_result = LightSamplingBuildResult();
	HashMap<uint64_t, bool> current_ids;
	current_ids.reserve(p_input.size());
	r_result.records.reserve(p_input.size());

	for (const LightSamplingInputRecord &input : p_input) {
		if (input.source_id == 0 || input.sample_id == 0) {
			return _fail(ERR_INVALID_PARAMETER, "Light-sampling source and sample IDs must be non-zero.", r_error);
		}
		if (!_is_type_domain_valid(input.type, input.domain)) {
			return _fail(ERR_INVALID_PARAMETER, "Light-sampling type and domain are incompatible.", r_error);
		}
		if (current_ids.has(input.sample_id)) {
			return _fail(ERR_INVALID_PARAMETER, "Light-sampling sample IDs must be unique per build.", r_error);
		}
		current_ids.insert(input.sample_id, true);

		LightSamplingGpuRecord record = {};
		record.source_id = input.source_id;
		record.sample_id = input.sample_id;
		record.current_index = LIGHT_SAMPLING_INVALID_INDEX;
		record.previous_index = LIGHT_SAMPLING_INVALID_INDEX;
		record.type = input.type;
		record.domain = input.domain;
		record.flags = LIGHT_SAMPLING_RECORD_VALID | input.flags;
		record.selection = LIGHT_SAMPLING_SELECTION_DISCRETE;
		record.lineage_instance_id = input.lineage_instance_id;
		record.lineage_primitive_id = input.lineage_primitive_id;
		if (!Math::is_finite(input.weight) || input.weight < 0.0f) {
			record.flags |= LIGHT_SAMPLING_RECORD_WEIGHT_SANITIZED;
			r_result.diagnostics.invalid_weight_count++;
		} else {
			record.weight = input.weight;
			r_result.diagnostics.valid_weight_count++;
			if (record.weight == 0.0f) {
				r_result.diagnostics.zero_weight_count++;
			}
		}
		r_result.records.push_back(record);
	}

	r_result.records.sort_custom<RecordOrder>();
	HashMap<uint64_t, uint32_t> next_indices;
	next_indices.reserve(r_result.records.size());
	for (int i = 0; i < r_result.records.size(); i++) {
		LightSamplingGpuRecord &record = r_result.records.write[i];
		record.current_index = i;
		const uint32_t *previous_index = previous_indices.getptr(record.sample_id);
		if (previous_index) {
			record.previous_index = *previous_index;
			r_result.diagnostics.preserved_count++;
		} else {
			r_result.diagnostics.added_count++;
		}
		next_indices.insert(record.sample_id, i);
	}
	for (const KeyValue<uint64_t, uint32_t> &previous : previous_indices) {
		if (!next_indices.has(previous.key)) {
			r_result.diagnostics.removed_count++;
		}
	}
	previous_indices = std::move(next_indices);

	_build_distribution(r_result, r_result.local_distribution, LIGHT_SAMPLING_DOMAIN_LOCAL);
	_build_distribution(r_result, r_result.environment_distribution, LIGHT_SAMPLING_DOMAIN_ENVIRONMENT);
	r_result.diagnostics.local_record_count = r_result.local_distribution.record_count;
	r_result.diagnostics.environment_record_count = r_result.environment_distribution.record_count;
	r_result.diagnostics.local_distribution_selectable = r_result.local_distribution.selectable;
	r_result.diagnostics.environment_distribution_selectable = r_result.environment_distribution.selectable;
	r_result.diagnostics.local_normalization_error = r_result.local_distribution.normalization_error;
	r_result.diagnostics.environment_normalization_error = r_result.environment_distribution.normalization_error;

	uint64_t checksum = 14695981039346656037ULL;
	_hash_bytes(checksum, &r_result.abi_version, sizeof(r_result.abi_version));
	for (const LightSamplingGpuRecord &record : r_result.records) {
		_hash_bytes(checksum, &record, sizeof(record));
	}
	_hash_bytes(checksum, &r_result.local_distribution.selectable, sizeof(r_result.local_distribution.selectable));
	_hash_bytes(checksum, &r_result.environment_distribution.selectable, sizeof(r_result.environment_distribution.selectable));
	r_result.diagnostics.checksum = checksum;
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

float light_sampling_triangle_power_weight(float p_radiance_luminance, float p_area) {
	if (!Math::is_finite(p_radiance_luminance) || !Math::is_finite(p_area) || p_radiance_luminance <= 0.0f || p_area <= 0.0f) {
		return 0.0f;
	}
	const double value = double(p_radiance_luminance) * double(p_area);
	return value >= std::numeric_limits<float>::max() ? std::numeric_limits<float>::max() : (float)value;
}

LightSamplingReservoir make_invalid_light_sampling_reservoir() {
	LightSamplingReservoir reservoir = {};
	reservoir.selected_index = LIGHT_SAMPLING_INVALID_INDEX;
	return reservoir;
}

Error initialize_light_sampling_view_reservoirs(uint32_t p_view_count, uint32_t p_reservoir_count, Vector<LightSamplingViewReservoirs> &r_views, String *r_error) {
	if (p_view_count == 0 || p_view_count > 2) {
		return _fail(ERR_INVALID_PARAMETER, "Light sampling supports one or two independent view reservoir arrays.", r_error);
	}
	r_views.clear();
	r_views.resize(p_view_count);
	for (uint32_t view_index = 0; view_index < p_view_count; view_index++) {
		LightSamplingViewReservoirs &view = r_views.write[view_index];
		view.view_index = view_index;
		view.reservoirs.resize(p_reservoir_count);
		for (uint32_t i = 0; i < p_reservoir_count; i++) {
			view.reservoirs.write[i] = make_invalid_light_sampling_reservoir();
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

bool update_light_sampling_reservoir(const LightSamplingReservoirCandidate &p_candidate, float p_random, LightSamplingReservoir &r_reservoir) {
	if (p_candidate.record_index == LIGHT_SAMPLING_INVALID_INDEX || p_candidate.source_id == 0 || p_candidate.sample_id == 0 ||
			!Math::is_finite(p_candidate.target) || !Math::is_finite(p_candidate.proposal_pdf) || p_candidate.target <= 0.0f || p_candidate.proposal_pdf <= 0.0f) {
		return false;
	}
	const float candidate_weight = p_candidate.target / p_candidate.proposal_pdf;
	if (!Math::is_finite(candidate_weight) || candidate_weight <= 0.0f) {
		return false;
	}
	const float previous_weight = (r_reservoir.flags & LIGHT_SAMPLING_RESERVOIR_VALID) != 0 && Math::is_finite(r_reservoir.weight_sum) && r_reservoir.weight_sum > 0.0f ? r_reservoir.weight_sum : 0.0f;
	const float total_weight = previous_weight + candidate_weight;
	if (!Math::is_finite(total_weight) || total_weight <= 0.0f) {
		return false;
	}
	p_random = CLAMP(p_random, 0.0f, 0.99999994f);
	if (previous_weight == 0.0f || p_random * total_weight < candidate_weight) {
		r_reservoir.selected_source_id = p_candidate.source_id;
		r_reservoir.selected_sample_id = p_candidate.sample_id;
		r_reservoir.selected_index = p_candidate.record_index;
		r_reservoir.selected_pdf = p_candidate.proposal_pdf;
	}
	r_reservoir.weight_sum = total_weight;
	r_reservoir.candidate_count = r_reservoir.candidate_count < UINT32_MAX ? r_reservoir.candidate_count + 1 : UINT32_MAX;
	r_reservoir.normalization = total_weight / float(r_reservoir.candidate_count);
	r_reservoir.age = 0;
	r_reservoir.flags |= LIGHT_SAMPLING_RESERVOIR_VALID;
	return true;
}

} // namespace RendererPathTracing
