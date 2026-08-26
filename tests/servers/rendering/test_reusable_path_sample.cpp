/**************************************************************************/
/*  test_reusable_path_sample.cpp                                         */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_reusable_path_sample)

#include "servers/rendering/path_tracing/reusable_path_sample.h"

#include <limits>

namespace TestReusablePathSample {

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
	authoring.age = 1;
	authoring.lighting_revision = 301;
	authoring.environment_revision = 302;
	authoring.throughput = Color(0.5f, 0.4f, 0.3f, 1.0f);
	authoring.incident_radiance = Color(2.0f, 1.0f, 0.5f, 1.0f);
	authoring.outgoing_radiance = Color(0.8f, 0.4f, 0.2f, 1.0f);
	authoring.source_primary_proposal_solid_angle_pdf = 0.25f;
	authoring.secondary_triangle_barycentric = Vector2(0.2f, 0.3f);
	authoring.target = 1.5f;
	authoring.normalization = 4.0f;
	return authoring;
}

static ReusablePathSampleCurrentState make_current() {
	const ReusablePathSampleAuthoring authoring = make_authoring();
	ReusablePathSampleCurrentState current;
	current.source_primary = authoring.source_primary;
	current.secondary = authoring.secondary;
	current.current_frame = 101;
	current.lighting_revision = authoring.lighting_revision;
	current.environment_revision = authoring.environment_revision;
	return current;
}

static ReusablePathSampleValidationThresholds make_thresholds() {
	ReusablePathSampleValidationThresholds thresholds;
	thresholds.maximum_age = 4;
	thresholds.maximum_source_primary_world_position_distance = 0.25f;
	thresholds.maximum_secondary_world_position_distance = 0.25f;
	thresholds.minimum_geometric_normal_dot = 0.99f;
	thresholds.minimum_shading_normal_dot = 0.99f;
	return thresholds;
}

static void check_rejection(const ReusablePathSampleGpuRecord &p_sample, const ReusablePathSampleCurrentState &p_current, const ReusablePathSampleValidationThresholds &p_thresholds, uint32_t p_reason) {
	const ReusablePathSampleValidationResult result = validate_reusable_path_sample(p_sample, p_current, p_thresholds);
	CHECK_FALSE(result.may_reuse_after_revalidation);
	CHECK((result.rejection_reasons & p_reason) != 0);
	CHECK(result.requires_current_target_re_evaluation);
	CHECK(result.requires_current_visibility_reconnection);
	CHECK_FALSE(result.contribution_permitted);
}

TEST_CASE("[PathTracing][ReusablePathSample] fixed GPU ABI has explicit validity and authoring conversion") {
	const ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	CHECK_EQ(sizeof(ReusablePathSampleGpuRecord), size_t(320));
	CHECK_EQ(alignof(ReusablePathSampleGpuRecord), size_t(16));
	CHECK_EQ(sample.abi_version, REUSABLE_PATH_SAMPLE_ABI_VERSION);
	CHECK_EQ(sample.represented_m, 1U);
	CHECK((sample.flags & REUSABLE_PATH_SAMPLE_RECORD_VALID) != 0);
	CHECK_EQ(sample.source_primary_geometry_instance_id, 11ULL);
	CHECK_EQ(sample.secondary_surface_id, 3U);
	CHECK_EQ(sample.secondary_world_position_y, 2.0f);
	CHECK_EQ(sample.source_primary_proposal_solid_angle_pdf, 0.25f);
	CHECK_EQ(sample.secondary_barycentric_u, 0.2f);
	CHECK_EQ(sample.secondary_barycentric_v, 0.3f);
	CHECK(reusable_path_sample_gpu_is_structurally_valid(sample));

	const ReusablePathSampleGpuRecord zeroed = {};
	CHECK_FALSE(reusable_path_sample_gpu_is_structurally_valid(zeroed));
}

TEST_CASE("[PathTracing][ReusablePathSample] represented M is explicit and rejects zero") {
	ReusablePathSampleAuthoring authoring = make_authoring();
	authoring.represented_m = 7;
	ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(authoring);
	CHECK_EQ(sample.represented_m, 7U);
	CHECK(reusable_path_sample_gpu_is_structurally_valid(sample));
	sample.represented_m = 0;
	CHECK_FALSE(reusable_path_sample_gpu_is_structurally_valid(sample));
}

TEST_CASE("[PathTracing][ReusablePathSample] valid world sample survives neighbor and camera movement") {
	const ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	ReusablePathSampleCurrentState current = make_current();
	// Camera coordinates are intentionally absent: a different camera/pixel may
	// validate the same world sample without invalidating it.
	current.current_frame = 102;
	current.source_primary.world_position.x += 0.1f;
	const ReusablePathSampleValidationResult result = validate_reusable_path_sample(sample, current, make_thresholds());
	CHECK(result.may_reuse_after_revalidation);
	CHECK_EQ(result.rejection_reasons, REUSABLE_PATH_SAMPLE_REJECT_NONE);
	CHECK_EQ(result.revalidation_reasons, REUSABLE_PATH_SAMPLE_REJECT_NONE);
	CHECK(result.requires_current_target_re_evaluation);
	CHECK(result.requires_current_visibility_reconnection);
	CHECK_FALSE(result.contribution_permitted);
}

TEST_CASE("[PathTracing][ReusablePathSample] fatal source and secondary identity revision and transport evidence reject closed") {
	const ReusablePathSampleGpuRecord baseline = reusable_path_sample_gpu_from_authoring(make_authoring());
	const ReusablePathSampleCurrentState current = make_current();
	const ReusablePathSampleValidationThresholds thresholds = make_thresholds();

	ReusablePathSampleGpuRecord sample = baseline;
	sample.abi_version++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_ABI);
	sample = baseline;
	sample.flags = 0;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_INVALID_RECORD);
	sample = baseline;
	sample.source_primary_geometry_instance_id++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_IDENTITY_MISMATCH);
	sample = baseline;
	sample.secondary_geometry_instance_id++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_IDENTITY_MISMATCH);
	sample = baseline;
	sample.source_primary_geometry_revision++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_GEOMETRY_REVISION_MISMATCH);
	sample = baseline;
	sample.secondary_geometry_revision++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_GEOMETRY_REVISION_MISMATCH);
	sample = baseline;
	sample.source_primary_material_revision++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_MATERIAL_REVISION_MISMATCH);
	sample = baseline;
	sample.secondary_material_revision++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_MATERIAL_REVISION_MISMATCH);
	sample = baseline;
	sample.source_primary_residency_revision++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_RESIDENCY_REVISION_MISMATCH);
	sample = baseline;
	sample.secondary_residency_revision++;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_RESIDENCY_REVISION_MISMATCH);

	ReusablePathSampleCurrentState dynamic_current = current;
	dynamic_current.source_primary.dynamic_geometry = true;
	check_rejection(baseline, dynamic_current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_DYNAMIC_GEOMETRY);
	dynamic_current = current;
	dynamic_current.secondary.dynamic_geometry = true;
	check_rejection(baseline, dynamic_current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_DYNAMIC_GEOMETRY);
	dynamic_current = current;
	dynamic_current.source_primary.disoccluded = true;
	check_rejection(baseline, dynamic_current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_DISOCCLUSION);
}

TEST_CASE("[PathTracing][ReusablePathSample] lighting and environment changes preserve a re-evaluable candidate") {
	ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	const ReusablePathSampleCurrentState current = make_current();
	sample.lighting_revision++;
	sample.environment_revision++;
	const ReusablePathSampleValidationResult result = validate_reusable_path_sample(sample, current, make_thresholds());
	CHECK(result.may_reuse_after_revalidation);
	CHECK_EQ(result.rejection_reasons, REUSABLE_PATH_SAMPLE_REJECT_NONE);
	CHECK((result.revalidation_reasons & REUSABLE_PATH_SAMPLE_REVALIDATE_LIGHTING_REVISION_MISMATCH) != 0);
	CHECK((result.revalidation_reasons & REUSABLE_PATH_SAMPLE_REVALIDATE_ENVIRONMENT_REVISION_MISMATCH) != 0);
	CHECK(result.requires_current_target_re_evaluation);
	CHECK(result.requires_current_visibility_reconnection);
	CHECK_FALSE(result.contribution_permitted);
}

TEST_CASE("[PathTracing][ReusablePathSample] finite PDF age position normal and masks fail closed") {
	const ReusablePathSampleGpuRecord baseline = reusable_path_sample_gpu_from_authoring(make_authoring());
	const ReusablePathSampleCurrentState current = make_current();
	const ReusablePathSampleValidationThresholds thresholds = make_thresholds();

	ReusablePathSampleGpuRecord sample = baseline;
	sample.source_primary_proposal_solid_angle_pdf = 0.0f;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_NONPOSITIVE_PDF);
	sample = baseline;
	sample.source_primary_proposal_solid_angle_pdf = std::numeric_limits<float>::quiet_NaN();
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_NONFINITE);
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_NONPOSITIVE_PDF);
	sample = baseline;
	sample.outgoing_radiance_r = std::numeric_limits<float>::infinity();
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_NONFINITE);
	sample = baseline;
	sample.capture_frame = 90;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_EXCESSIVE_AGE);
	sample = baseline;
	sample.age = 5;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_EXCESSIVE_AGE);
	sample = baseline;
	sample.source_primary_world_position_x += 0.3f;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_WORLD_POSITION);
	// A nearby opposite-facing hit is a thin-wall mismatch, not a safe reuse.
	sample = baseline;
	sample.secondary_geometric_normal_y = 1.0f;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_NORMAL_DISAGREEMENT);
	sample = baseline;
	sample.source_primary_mask = 0x8;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_MASK);
	sample = baseline;
	sample.secondary_mask = 0x8;
	check_rejection(sample, current, thresholds, REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_MASK);
}

TEST_CASE("[PathTracing][ReusablePathSample] identity and replay checksums are stable and float canonical") {
	const ReusablePathSampleGpuRecord baseline = reusable_path_sample_gpu_from_authoring(make_authoring());
	const uint64_t identity = reusable_path_sample_identity_checksum(baseline);
	const uint64_t replay = reusable_path_sample_replay_checksum(baseline);
	CHECK_NE(identity, 0ULL);
	CHECK_NE(replay, 0ULL);
	CHECK_EQ(identity, reusable_path_sample_identity_checksum(baseline));
	CHECK_EQ(replay, reusable_path_sample_replay_checksum(baseline));

	ReusablePathSampleGpuRecord changed_payload = baseline;
	changed_payload.outgoing_radiance_r += 1.0f;
	CHECK_EQ(identity, reusable_path_sample_identity_checksum(changed_payload));
	CHECK_NE(replay, reusable_path_sample_replay_checksum(changed_payload));
	ReusablePathSampleGpuRecord negative_zero = baseline;
	negative_zero.normalization = -0.0f;
	ReusablePathSampleGpuRecord positive_zero = baseline;
	positive_zero.normalization = 0.0f;
	CHECK_EQ(reusable_path_sample_replay_checksum(negative_zero), reusable_path_sample_replay_checksum(positive_zero));
}

TEST_CASE("[PathTracing][ReusablePathSample] source solid angle converts to secondary area and current solid angle") {
	const ReusablePathSampleGpuRecord sample = reusable_path_sample_gpu_from_authoring(make_authoring());
	float secondary_area_pdf = 0.0f;
	CHECK(reusable_path_sample_source_solid_angle_to_secondary_area_pdf(sample, secondary_area_pdf));
	// q_omega = 0.25, |n_y . -omega| = 1, and distance^2 = 4.
	CHECK_EQ(secondary_area_pdf, 0.0625f);

	ReusablePathSampleEndpoint same_primary = make_current().source_primary;
	float current_solid_angle_pdf = 0.0f;
	CHECK(reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(sample, same_primary, current_solid_angle_pdf));
	CHECK_EQ(current_solid_angle_pdf, 0.25f);

	ReusablePathSampleEndpoint shifted_primary = same_primary;
	shifted_primary.world_position = Vector3(0.0f, 1.0f, 0.0f);
	CHECK(reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(sample, shifted_primary, current_solid_angle_pdf));
	// The same area density subtends one quarter the source solid angle.
	CHECK_EQ(current_solid_angle_pdf, 0.0625f);
}

TEST_CASE("[PathTracing][ReusablePathSample] degenerate shifted measures reject closed") {
	const ReusablePathSampleGpuRecord baseline = reusable_path_sample_gpu_from_authoring(make_authoring());
	const ReusablePathSampleCurrentState current = make_current();
	float pdf = 1.0f;

	ReusablePathSampleGpuRecord sample = baseline;
	sample.secondary_geometric_normal_y = 0.0f;
	CHECK_FALSE(reusable_path_sample_source_solid_angle_to_secondary_area_pdf(sample, pdf));
	check_rejection(sample, current, make_thresholds(), REUSABLE_PATH_SAMPLE_REJECT_PROPOSAL_MEASURE);

	sample = baseline;
	sample.secondary_world_position_y = 0.0f;
	CHECK_FALSE(reusable_path_sample_source_solid_angle_to_secondary_area_pdf(sample, pdf));
	check_rejection(sample, current, make_thresholds(), REUSABLE_PATH_SAMPLE_REJECT_PROPOSAL_MEASURE);

	ReusablePathSampleEndpoint degenerate_primary = current.source_primary;
	degenerate_primary.world_position = Vector3(0.0f, 2.0f, 0.0f);
	CHECK_FALSE(reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(baseline, degenerate_primary, pdf));
	sample = baseline;
	sample.secondary_barycentric_u = 0.8f;
	sample.secondary_barycentric_v = 0.3f;
	check_rejection(sample, current, make_thresholds(), REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_BARYCENTRIC);
}

TEST_CASE("[PathTracing][ReusablePathSample] world cache reduction is deterministic and camera independent") {
	const ReusablePathSampleGpuRecord first = reusable_path_sample_gpu_from_authoring(make_authoring());
	ReusablePathSampleGpuRecord replacement = first;
	replacement.capture_frame++;
	const Vector3i key = reusable_path_sample_world_cell_key(Vector3(17.9f, 0.0f, -0.1f), 16.0f);
	CHECK_EQ(key, Vector3i(1, 0, -1));
	Vector<ReusablePathSampleWorldCell> previous;
	previous.push_back({ key, first });
	Vector<ReusablePathSampleWorldCell> staging;
	staging.push_back({ key, replacement });
	Vector<ReusablePathSampleWorldCell> next;
	reusable_path_sample_world_cache_reduce(previous, staging, next);
	CHECK_EQ(next.size(), 1);
	CHECK_EQ(next[0].record.capture_frame, replacement.capture_frame);
	ReusablePathSampleGpuRecord found = {};
	// Camera position is absent: an adjacent world-key query survives motion.
	CHECK(reusable_path_sample_world_cache_query_adjacent(next, key + Vector3i(1, 0, 0), found));
	CHECK_EQ(found.capture_frame, replacement.capture_frame);
}

} // namespace TestReusablePathSample
