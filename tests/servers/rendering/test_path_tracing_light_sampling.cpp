/**************************************************************************/
/*  test_path_tracing_light_sampling.cpp                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_path_tracing_light_sampling)

#include "servers/rendering/path_tracing/light_sampling.h"

#include <limits>

namespace TestPathTracingLightSampling {

using namespace RendererPathTracing;

static LightSamplingInputRecord make_record(uint64_t p_source_id, uint64_t p_sample_id, uint32_t p_type, uint32_t p_domain, float p_weight) {
	LightSamplingInputRecord record;
	record.source_id = p_source_id;
	record.sample_id = p_sample_id;
	record.type = p_type;
	record.domain = p_domain;
	record.weight = p_weight;
	return record;
}

TEST_CASE("[PathTracing][LightSampling] Unified records retain analytic, emissive, and environment lineage") {
	Vector<LightSamplingInputRecord> input;
	input.push_back(make_record(300, 301, LIGHT_SAMPLING_TYPE_ENVIRONMENT, LIGHT_SAMPLING_DOMAIN_ENVIRONMENT, 2.0f));
	LightSamplingInputRecord emissive = make_record(200, 201, LIGHT_SAMPLING_TYPE_EMISSIVE_TRIANGLE, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f);
	emissive.lineage_instance_id = 7;
	emissive.lineage_primitive_id = 3;
	input.push_back(emissive);
	input.push_back(make_record(100, 100, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 4.0f));

	LightSamplingIdentityTracker tracker;
	LightSamplingBuildResult result;
	CHECK_EQ(tracker.build(input, result), OK);
	CHECK_EQ(result.records.size(), 3);
	CHECK_EQ(result.records[0].sample_id, 100);
	CHECK_EQ(result.records[1].sample_id, 201);
	CHECK_EQ(result.records[1].lineage_instance_id, 7);
	CHECK_EQ(result.records[1].lineage_primitive_id, 3);
	CHECK_EQ(result.records[2].sample_id, 301);
	CHECK_EQ(result.records[0].previous_index, LIGHT_SAMPLING_INVALID_INDEX);
	CHECK_EQ(result.local_distribution.record_count, 2);
	CHECK_EQ(result.environment_distribution.record_count, 1);
	CHECK_EQ(result.diagnostics.valid_weight_count, 3);
	CHECK_EQ(result.diagnostics.invalid_weight_count, 0);
	CHECK(result.local_distribution.selectable);
	CHECK(result.environment_distribution.selectable);
	CHECK_EQ(result.records[0].pdf, doctest::Approx(0.8f));
	CHECK_EQ(result.records[1].pdf, doctest::Approx(0.2f));
	CHECK_EQ(result.records[1].cdf, 1.0f);
	CHECK_EQ(result.records[2].pdf, 1.0f);
}

TEST_CASE("[PathTracing][LightSampling] Stable sample identities map across reordering and membership changes") {
	LightSamplingIdentityTracker tracker;
	LightSamplingBuildResult first;
	Vector<LightSamplingInputRecord> initial;
	initial.push_back(make_record(10, 11, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	initial.push_back(make_record(20, 21, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	initial.push_back(make_record(30, 31, LIGHT_SAMPLING_TYPE_ENVIRONMENT, LIGHT_SAMPLING_DOMAIN_ENVIRONMENT, 1.0f));
	CHECK_EQ(tracker.build(initial, first), OK);

	LightSamplingBuildResult second;
	Vector<LightSamplingInputRecord> next;
	next.push_back(make_record(30, 31, LIGHT_SAMPLING_TYPE_ENVIRONMENT, LIGHT_SAMPLING_DOMAIN_ENVIRONMENT, 1.0f));
	next.push_back(make_record(40, 41, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	next.push_back(make_record(10, 11, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	CHECK_EQ(tracker.build(next, second), OK);
	CHECK_EQ(second.diagnostics.preserved_count, 2);
	CHECK_EQ(second.diagnostics.added_count, 1);
	CHECK_EQ(second.diagnostics.removed_count, 1);
	CHECK_EQ(second.records[0].sample_id, 11);
	CHECK_EQ(second.records[0].previous_index, 0);
	CHECK_EQ(second.records[2].sample_id, 31);
	CHECK_EQ(second.records[2].previous_index, 2);
}

TEST_CASE("[PathTracing][LightSampling] Invalid identities reject and invalid weights sanitize to zero") {
	LightSamplingIdentityTracker tracker;
	LightSamplingBuildResult result;
	Vector<LightSamplingInputRecord> invalid;
	invalid.push_back(make_record(0, 1, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	CHECK_EQ(tracker.build(invalid, result), ERR_INVALID_PARAMETER);

	invalid.clear();
	invalid.push_back(make_record(1, 2, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	invalid.push_back(make_record(2, 2, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	CHECK_EQ(tracker.build(invalid, result), ERR_INVALID_PARAMETER);

	Vector<LightSamplingInputRecord> sanitized;
	sanitized.push_back(make_record(1, 2, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, -1.0f));
	sanitized.push_back(make_record(2, 3, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, std::numeric_limits<float>::quiet_NaN()));
	CHECK_EQ(tracker.build(sanitized, result), OK);
	CHECK_EQ(result.diagnostics.invalid_weight_count, 2);
	CHECK_EQ(result.diagnostics.zero_weight_count, 0);
	CHECK_FALSE(result.local_distribution.selectable);
	CHECK_EQ(result.records[0].weight, 0.0f);
	CHECK((result.records[0].flags & LIGHT_SAMPLING_RECORD_WEIGHT_SANITIZED) != 0);
}

TEST_CASE("[PathTracing][LightSampling] All-zero distributions and replay checksums are deterministic") {
	Vector<LightSamplingInputRecord> zero_input;
	zero_input.push_back(make_record(1, 2, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 0.0f));
	zero_input.push_back(make_record(3, 4, LIGHT_SAMPLING_TYPE_ENVIRONMENT, LIGHT_SAMPLING_DOMAIN_ENVIRONMENT, 0.0f));
	LightSamplingIdentityTracker zero_tracker;
	LightSamplingBuildResult zero_result;
	CHECK_EQ(zero_tracker.build(zero_input, zero_result), OK);
	CHECK_FALSE(zero_result.local_distribution.selectable);
	CHECK_FALSE(zero_result.environment_distribution.selectable);
	CHECK(zero_result.local_distribution.cdf.is_empty());
	CHECK_EQ(zero_result.records[0].pdf, 0.0f);
	CHECK_EQ(zero_result.diagnostics.valid_weight_count, 2);
	CHECK_EQ(zero_result.diagnostics.zero_weight_count, 2);

	Vector<LightSamplingInputRecord> deterministic_input;
	deterministic_input.push_back(make_record(30, 31, LIGHT_SAMPLING_TYPE_ENVIRONMENT, LIGHT_SAMPLING_DOMAIN_ENVIRONMENT, 2.0f));
	deterministic_input.push_back(make_record(10, 11, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, 3.0f));
	deterministic_input.push_back(make_record(20, 21, LIGHT_SAMPLING_TYPE_EMISSIVE_TRIANGLE, LIGHT_SAMPLING_DOMAIN_LOCAL, 1.0f));
	LightSamplingIdentityTracker first_tracker;
	LightSamplingIdentityTracker second_tracker;
	LightSamplingBuildResult first;
	LightSamplingBuildResult second;
	CHECK_EQ(first_tracker.build(deterministic_input, first), OK);
	CHECK_EQ(second_tracker.build(deterministic_input, second), OK);
	CHECK_EQ(first.diagnostics.checksum, second.diagnostics.checksum);
	CHECK_EQ(first.records.size(), second.records.size());
	for (int i = 0; i < first.records.size(); i++) {
		CHECK_EQ(first.records[i].sample_id, second.records[i].sample_id);
		CHECK_EQ(first.records[i].cdf, second.records[i].cdf);
	}
}

TEST_CASE("[PathTracing][LightSampling] Deterministic many-light fixture has no bounded packet cap") {
	Vector<LightSamplingInputRecord> input;
	constexpr uint32_t light_count = 4096;
	input.reserve(light_count);
	for (uint32_t i = 0; i < light_count; i++) {
		input.push_back(make_record(10000 + i, 20000 + i, LIGHT_SAMPLING_TYPE_ANALYTIC, LIGHT_SAMPLING_DOMAIN_LOCAL, (float)((i % 11) + 1)));
	}
	LightSamplingIdentityTracker tracker;
	LightSamplingBuildResult result;
	CHECK_EQ(tracker.build(input, result), OK);
	CHECK_EQ(result.records.size(), light_count);
	CHECK_EQ(result.local_distribution.record_count, light_count);
	CHECK_EQ(result.local_distribution.cdf.size(), light_count);
	CHECK_EQ(result.local_distribution.cdf[light_count - 1], 1.0f);
	CHECK(result.diagnostics.local_normalization_error <= 0.000001f);
}

TEST_CASE("[PathTracing][LightSampling] Shared distributions use distinct per-view reservoir state") {
	Vector<LightSamplingViewReservoirs> views;
	CHECK_EQ(initialize_light_sampling_view_reservoirs(2, 8, views), OK);
	CHECK_EQ(views.size(), 2);
	CHECK_EQ(views[0].view_index, 0);
	CHECK_EQ(views[1].view_index, 1);
	CHECK_EQ(views[0].reservoirs.size(), 8);
	CHECK_EQ(views[0].reservoirs[0].flags, 0);
	CHECK_EQ(views[0].reservoirs[0].selected_index, LIGHT_SAMPLING_INVALID_INDEX);
	views.write[0].reservoirs.write[0].flags = LIGHT_SAMPLING_RESERVOIR_VALID;
	CHECK_EQ(views[1].reservoirs[0].flags, 0);
	CHECK_EQ(initialize_light_sampling_view_reservoirs(3, 8, views), ERR_INVALID_PARAMETER);
}

} // namespace TestPathTracingLightSampling
