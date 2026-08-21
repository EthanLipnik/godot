/**************************************************************************/
/*  light_sampling.h                                                      */
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

#pragma once

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

#include <cstddef>
#include <cstdint>

namespace RendererPathTracing {

// This ABI is intentionally independent from ScenePacket schema 1 and from
// backend or vendor SDK types. Zero-initialized GPU records are invalid.
static constexpr uint32_t LIGHT_SAMPLING_ABI_VERSION = 1;
static constexpr uint32_t LIGHT_SAMPLING_INVALID_INDEX = UINT32_MAX;

enum LightSamplingType : uint32_t {
	LIGHT_SAMPLING_TYPE_ANALYTIC = 1,
	LIGHT_SAMPLING_TYPE_EMISSIVE_TRIANGLE = 2,
	LIGHT_SAMPLING_TYPE_ENVIRONMENT = 3,
};

enum LightSamplingDomain : uint32_t {
	LIGHT_SAMPLING_DOMAIN_LOCAL = 1,
	LIGHT_SAMPLING_DOMAIN_ENVIRONMENT = 2,
};

enum LightSamplingRecordFlags : uint32_t {
	LIGHT_SAMPLING_RECORD_VALID = 1 << 0,
	LIGHT_SAMPLING_RECORD_WEIGHT_SANITIZED = 1 << 1,
};

enum LightSamplingSelection : uint32_t {
	LIGHT_SAMPLING_SELECTION_DISCRETE = 1,
};

enum LightSamplingReservoirFlags : uint32_t {
	LIGHT_SAMPLING_RESERVOIR_VALID = 1 << 0,
};

// `source_id` identifies an analytic light, emissive mesh instance, or
// environment source. `sample_id` identifies one selectable sample and must
// be non-zero and unique per build; emissive triangles therefore share a
// source_id but have distinct sample_ids.
struct LightSamplingInputRecord {
	uint64_t source_id = 0;
	uint64_t sample_id = 0;
	uint32_t type = 0;
	uint32_t domain = 0;
	uint32_t flags = 0;
	uint32_t lineage_instance_id = 0;
	uint32_t lineage_primitive_id = 0;
	float weight = 0.0f;
};

// GPU-facing std430/Metal-compatible scalar layout. `current_index` and
// `previous_index` use LIGHT_SAMPLING_INVALID_INDEX when unavailable. The
// valid flag, not a zero identity/index, determines whether a zeroed record
// may be consumed.
struct alignas(16) LightSamplingGpuRecord {
	uint64_t source_id;
	uint64_t sample_id;
	uint32_t current_index;
	uint32_t previous_index;
	uint32_t type;
	uint32_t domain;
	uint32_t flags;
	uint32_t selection;
	float weight;
	float pdf;
	uint32_t lineage_instance_id;
	uint32_t lineage_primitive_id;
	float cdf;
	uint32_t reserved;
};

// GPU-facing direct-light reservoir. A reservoir belongs to exactly one view
// and one screen-space element; scene distributions are shared separately.
struct alignas(16) LightSamplingReservoir {
	uint64_t selected_source_id;
	uint64_t selected_sample_id;
	float weight_sum;
	float selected_pdf;
	uint32_t candidate_count;
	uint32_t age;
	uint32_t flags;
	uint32_t selected_index;
	float normalization;
	uint32_t reserved;
};

static_assert(sizeof(LightSamplingGpuRecord) == 64);
static_assert(alignof(LightSamplingGpuRecord) == 16);
static_assert(offsetof(LightSamplingGpuRecord, source_id) == 0);
static_assert(offsetof(LightSamplingGpuRecord, current_index) == 16);
static_assert(offsetof(LightSamplingGpuRecord, type) == 24);
static_assert(offsetof(LightSamplingGpuRecord, weight) == 40);
static_assert(offsetof(LightSamplingGpuRecord, lineage_instance_id) == 48);
static_assert(offsetof(LightSamplingGpuRecord, cdf) == 56);
static_assert(sizeof(LightSamplingReservoir) == 48);
static_assert(alignof(LightSamplingReservoir) == 16);
static_assert(offsetof(LightSamplingReservoir, selected_source_id) == 0);
static_assert(offsetof(LightSamplingReservoir, weight_sum) == 16);
static_assert(offsetof(LightSamplingReservoir, candidate_count) == 24);
static_assert(offsetof(LightSamplingReservoir, flags) == 32);

struct LightSamplingDistribution {
	uint32_t domain = 0;
	uint32_t first_record = 0;
	uint32_t record_count = 0;
	bool selectable = false;
	float total_weight = 0.0f;
	float normalization_error = 0.0f;
	Vector<float> cdf;
};

struct LightSamplingDiagnostics {
	uint32_t local_record_count = 0;
	uint32_t environment_record_count = 0;
	uint32_t valid_weight_count = 0;
	uint32_t zero_weight_count = 0;
	uint32_t invalid_weight_count = 0;
	uint32_t added_count = 0;
	uint32_t removed_count = 0;
	uint32_t preserved_count = 0;
	bool local_distribution_selectable = false;
	bool environment_distribution_selectable = false;
	float local_normalization_error = 0.0f;
	float environment_normalization_error = 0.0f;
	uint64_t checksum = 0;
};

struct LightSamplingBuildResult {
	uint32_t abi_version = LIGHT_SAMPLING_ABI_VERSION;
	Vector<LightSamplingGpuRecord> records;
	LightSamplingDistribution local_distribution;
	LightSamplingDistribution environment_distribution;
	LightSamplingDiagnostics diagnostics;
};

class LightSamplingIdentityTracker {
	HashMap<uint64_t, uint32_t> previous_indices;

public:
	void clear();
	Error build(const Vector<LightSamplingInputRecord> &p_input, LightSamplingBuildResult &r_result, String *r_error = nullptr);
};

struct LightSamplingViewReservoirs {
	uint32_t view_index = 0;
	Vector<LightSamplingReservoir> reservoirs;
};

// Scene distributions may be shared between views. This helper only creates
// independent per-view screen-space reservoir storage; it does not imply any
// cross-eye sample reuse.
Error initialize_light_sampling_view_reservoirs(uint32_t p_view_count, uint32_t p_reservoir_count, Vector<LightSamplingViewReservoirs> &r_views, String *r_error = nullptr);
LightSamplingReservoir make_invalid_light_sampling_reservoir();

} // namespace RendererPathTracing
