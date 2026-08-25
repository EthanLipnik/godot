/**************************************************************************/
/*  reusable_path_sample.h                                                */
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

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/math/vector2.h"
#include "core/templates/vector.h"

#include <cstddef>
#include <cstdint>

namespace RendererPathTracing {

// This record owns no view, pixel, eye, motion-vector, visibility, or
// reservoir state. Those remain with a screen-space client. It is a stable
// world-sample ABI that a backend may upload without C++ bool/padding rules.
//
// Version 2 records both endpoints of the sampled segment. A BSDF-generated
// secondary hit cannot be shifted to another primary point without its source
// endpoint: that endpoint is needed to convert its solid-angle proposal into
// a secondary-surface-area proposal before deriving the current proposal.
static constexpr uint32_t REUSABLE_PATH_SAMPLE_ABI_VERSION = 3;

enum ReusablePathSampleRecordFlags : uint32_t {
	REUSABLE_PATH_SAMPLE_RECORD_VALID = 1U << 0,
};

struct ReusablePathSampleEndpointRevisions {
	uint64_t geometry = 0;
	uint64_t material = 0;
	uint64_t residency = 0;
};

struct ReusablePathSampleEndpoint {
	uint64_t geometry_instance_id = 0;
	uint64_t material_id = 0;
	uint32_t surface_id = 0;
	uint32_t primitive_id = 0;
	uint64_t visibility_mask = 0;
	ReusablePathSampleEndpointRevisions revisions;
	Vector3 world_position;
	Vector3 geometric_normal;
	Vector3 shading_normal;
	bool dynamic_geometry = false;
	bool disoccluded = false;
};

// CPU authoring form. `outgoing_radiance` is an opaque replay payload; this
// ABI deliberately does not assign it ReSTIR GI estimator semantics. The
// source proposal is explicitly a density with respect to solid angle at
// `source_primary`, not a surface-area density at `secondary`.
struct ReusablePathSampleAuthoring {
	ReusablePathSampleEndpoint source_primary;
	ReusablePathSampleEndpoint secondary;
	uint64_t capture_frame = 0;
	uint32_t age = 0;
	uint64_t lighting_revision = 0;
	uint64_t environment_revision = 0;
	Color throughput;
	Color incident_radiance;
	Color outgoing_radiance;
	float source_primary_proposal_solid_angle_pdf = 0.0f;
	Vector2 secondary_triangle_barycentric;
	float target = 0.0f;
	float normalization = 0.0f;
};

// std430/Metal scalar compatible. The version and valid flag must both be
// set, therefore a zero-initialized record is always invalid. Do not use C++
// bool or native math types here: the offsets below are the shader contract.
struct alignas(16) ReusablePathSampleGpuRecord {
	uint64_t secondary_geometry_instance_id;
	uint64_t secondary_material_id;
	uint32_t secondary_surface_id;
	uint32_t secondary_primitive_id;
	uint64_t source_primary_geometry_instance_id;
	uint64_t source_primary_material_id;
	uint32_t source_primary_surface_id;
	uint32_t source_primary_primitive_id;
	uint32_t abi_version;
	uint32_t flags;
	uint64_t secondary_geometry_revision;
	uint64_t secondary_material_revision;
	uint64_t secondary_residency_revision;
	uint64_t source_primary_geometry_revision;
	uint64_t source_primary_material_revision;
	uint64_t source_primary_residency_revision;
	uint64_t lighting_revision;
	uint64_t environment_revision;
	uint64_t source_primary_mask;
	uint64_t secondary_mask;
	uint64_t capture_frame;
	float secondary_world_position_x;
	float secondary_world_position_y;
	float secondary_world_position_z;
	float reserved_secondary_position;
	float secondary_geometric_normal_x;
	float secondary_geometric_normal_y;
	float secondary_geometric_normal_z;
	float reserved_secondary_geometric_normal;
	float secondary_shading_normal_x;
	float secondary_shading_normal_y;
	float secondary_shading_normal_z;
	float reserved_secondary_shading_normal;
	float source_primary_world_position_x;
	float source_primary_world_position_y;
	float source_primary_world_position_z;
	float reserved_source_primary_position;
	float source_primary_geometric_normal_x;
	float source_primary_geometric_normal_y;
	float source_primary_geometric_normal_z;
	float reserved_source_primary_geometric_normal;
	float source_primary_shading_normal_x;
	float source_primary_shading_normal_y;
	float source_primary_shading_normal_z;
	float reserved_source_primary_shading_normal;
	float throughput_r;
	float throughput_g;
	float throughput_b;
	float throughput_a;
	float incident_radiance_r;
	float incident_radiance_g;
	float incident_radiance_b;
	float incident_radiance_a;
	float outgoing_radiance_r;
	float outgoing_radiance_g;
	float outgoing_radiance_b;
	float outgoing_radiance_a;
	float source_primary_proposal_solid_angle_pdf;
	float target;
	float normalization;
	uint32_t age;
	float secondary_barycentric_u;
	float secondary_barycentric_v;
	uint32_t reserved2;
};

static_assert(sizeof(ReusablePathSampleGpuRecord) == 320);
static_assert(alignof(ReusablePathSampleGpuRecord) == 16);
static_assert(offsetof(ReusablePathSampleGpuRecord, secondary_geometry_instance_id) == 0);
static_assert(offsetof(ReusablePathSampleGpuRecord, secondary_surface_id) == 16);
static_assert(offsetof(ReusablePathSampleGpuRecord, source_primary_geometry_instance_id) == 24);
static_assert(offsetof(ReusablePathSampleGpuRecord, abi_version) == 48);
static_assert(offsetof(ReusablePathSampleGpuRecord, secondary_geometry_revision) == 56);
static_assert(offsetof(ReusablePathSampleGpuRecord, source_primary_geometry_revision) == 80);
static_assert(offsetof(ReusablePathSampleGpuRecord, lighting_revision) == 104);
static_assert(offsetof(ReusablePathSampleGpuRecord, source_primary_mask) == 120);
static_assert(offsetof(ReusablePathSampleGpuRecord, capture_frame) == 136);
static_assert(offsetof(ReusablePathSampleGpuRecord, secondary_world_position_x) == 144);
static_assert(offsetof(ReusablePathSampleGpuRecord, source_primary_world_position_x) == 192);
static_assert(offsetof(ReusablePathSampleGpuRecord, throughput_r) == 240);
static_assert(offsetof(ReusablePathSampleGpuRecord, incident_radiance_r) == 256);
static_assert(offsetof(ReusablePathSampleGpuRecord, outgoing_radiance_r) == 272);
static_assert(offsetof(ReusablePathSampleGpuRecord, source_primary_proposal_solid_angle_pdf) == 288);
static_assert(offsetof(ReusablePathSampleGpuRecord, age) == 300);
static_assert(offsetof(ReusablePathSampleGpuRecord, secondary_barycentric_u) == 304);
static_assert(offsetof(ReusablePathSampleGpuRecord, secondary_barycentric_v) == 308);

enum ReusablePathSampleValidationReason : uint32_t {
	REUSABLE_PATH_SAMPLE_REJECT_NONE = 0,
	REUSABLE_PATH_SAMPLE_REJECT_ABI = 1U << 0,
	REUSABLE_PATH_SAMPLE_REJECT_INVALID_RECORD = 1U << 1,
	REUSABLE_PATH_SAMPLE_REJECT_NONFINITE = 1U << 2,
	REUSABLE_PATH_SAMPLE_REJECT_NONPOSITIVE_PDF = 1U << 3,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_IDENTITY_MISMATCH = 1U << 4,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_IDENTITY_MISMATCH = 1U << 5,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_GEOMETRY_REVISION_MISMATCH = 1U << 6,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_GEOMETRY_REVISION_MISMATCH = 1U << 7,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_MATERIAL_REVISION_MISMATCH = 1U << 8,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_MATERIAL_REVISION_MISMATCH = 1U << 9,
	REUSABLE_PATH_SAMPLE_REVALIDATE_LIGHTING_REVISION_MISMATCH = 1U << 10,
	REUSABLE_PATH_SAMPLE_REVALIDATE_ENVIRONMENT_REVISION_MISMATCH = 1U << 11,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_RESIDENCY_REVISION_MISMATCH = 1U << 12,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_RESIDENCY_REVISION_MISMATCH = 1U << 13,
	REUSABLE_PATH_SAMPLE_REJECT_EXCESSIVE_AGE = 1U << 14,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_WORLD_POSITION = 1U << 15,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_WORLD_POSITION = 1U << 16,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_NORMAL_DISAGREEMENT = 1U << 17,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_NORMAL_DISAGREEMENT = 1U << 18,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_MASK = 1U << 19,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_MASK = 1U << 20,
	REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_DYNAMIC_GEOMETRY = 1U << 21,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_DYNAMIC_GEOMETRY = 1U << 22,
	REUSABLE_PATH_SAMPLE_REJECT_DISOCCLUSION = 1U << 23,
	REUSABLE_PATH_SAMPLE_REJECT_PROPOSAL_MEASURE = 1U << 24,
	REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_BARYCENTRIC = 1U << 25,
};

struct ReusablePathSampleCurrentState {
	ReusablePathSampleEndpoint source_primary;
	ReusablePathSampleEndpoint secondary;
	uint64_t current_frame = 0;
	uint64_t lighting_revision = 0;
	uint64_t environment_revision = 0;
};

struct ReusablePathSampleValidationThresholds {
	uint32_t maximum_age = 0;
	float maximum_source_primary_world_position_distance = 0.0f;
	float maximum_secondary_world_position_distance = 0.0f;
	float minimum_geometric_normal_dot = 1.0f;
	float minimum_shading_normal_dot = 1.0f;
};

struct ReusablePathSampleValidationResult {
	uint32_t rejection_reasons = REUSABLE_PATH_SAMPLE_REJECT_NONE;
	uint32_t revalidation_reasons = REUSABLE_PATH_SAMPLE_REJECT_NONE;
	bool may_reuse_after_revalidation = false;
	// These are deliberately true for every structurally usable record. A
	// caller must re-evaluate its current target and reconnect visibility
	// before it permits a contribution; cached visibility is never current.
	bool requires_current_target_re_evaluation = true;
	bool requires_current_visibility_reconnection = true;
	bool contribution_permitted = false;
};

ReusablePathSampleGpuRecord reusable_path_sample_gpu_from_authoring(const ReusablePathSampleAuthoring &p_authoring);
bool reusable_path_sample_gpu_is_structurally_valid(const ReusablePathSampleGpuRecord &p_sample);
ReusablePathSampleValidationResult validate_reusable_path_sample(const ReusablePathSampleGpuRecord &p_sample, const ReusablePathSampleCurrentState &p_current, const ReusablePathSampleValidationThresholds &p_thresholds);

// Converts the stored source-primary solid-angle density q_omega(y|x_source)
// into q_area(y|x_source) at the secondary surface. A false return means the
// path has a degenerate/nonfinite measure and must be rejected.
bool reusable_path_sample_source_solid_angle_to_secondary_area_pdf(const ReusablePathSampleGpuRecord &p_sample, float &r_secondary_area_pdf);

// Converts q_area(y|x_source) into q_omega(y|x_current) at the supplied
// current primary point. This includes the full geometry/Jacobian factor
// caused by shifting the primary endpoint. It never validates visibility.
bool reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(const ReusablePathSampleGpuRecord &p_sample, const ReusablePathSampleEndpoint &p_current_primary, float &r_current_primary_solid_angle_pdf);

// Small deterministic reference for backend cache ownership. It deliberately
// stores only world cells and ABI-v2 records: camera/pixel/visibility state is
// excluded so a camera move can retain overlapping cells. GPU implementations
// use the same immutable-previous + staging + next-reduction contract.
struct ReusablePathSampleWorldCell {
	Vector3i key;
	ReusablePathSampleGpuRecord record;
};

Vector3i reusable_path_sample_world_cell_key(const Vector3 &p_source_primary_world_position, float p_cell_size);
bool reusable_path_sample_world_cache_query_adjacent(const Vector<ReusablePathSampleWorldCell> &p_cells, const Vector3i &p_key, ReusablePathSampleGpuRecord &r_record);
void reusable_path_sample_world_cache_reduce(const Vector<ReusablePathSampleWorldCell> &p_previous, const Vector<ReusablePathSampleWorldCell> &p_staging, Vector<ReusablePathSampleWorldCell> &r_next);

// These serialize selected fields in a fixed byte order. Identity contains
// no floating point. Replay canonicalizes negative zero and all NaNs.
uint64_t reusable_path_sample_identity_checksum(const ReusablePathSampleGpuRecord &p_sample);
uint64_t reusable_path_sample_replay_checksum(const ReusablePathSampleGpuRecord &p_sample);

} // namespace RendererPathTracing
