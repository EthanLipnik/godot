/**************************************************************************/
/*  reusable_path_sample.cpp                                              */
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

#include "reusable_path_sample.h"

#include "core/math/math_funcs.h"

#include <cmath>
#include <cstring>

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

static uint32_t _canonical_float_bits(float p_value) {
	if (p_value == 0.0f) {
		return 0;
	}
	if (std::isnan(p_value)) {
		return 0x7fc00000U;
	}
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(p_value));
	std::memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

static uint64_t _fnv_append_float(uint64_t p_hash, float p_value) {
	return _fnv_append_u32(p_hash, _canonical_float_bits(p_value));
}

static bool _finite_color(float p_r, float p_g, float p_b, float p_a) {
	return Math::is_finite(p_r) && Math::is_finite(p_g) && Math::is_finite(p_b) && Math::is_finite(p_a);
}

static bool _finite_record(const ReusablePathSampleGpuRecord &p_sample) {
	return Math::is_finite(p_sample.secondary_world_position_x) && Math::is_finite(p_sample.secondary_world_position_y) && Math::is_finite(p_sample.secondary_world_position_z) &&
			Math::is_finite(p_sample.secondary_geometric_normal_x) && Math::is_finite(p_sample.secondary_geometric_normal_y) && Math::is_finite(p_sample.secondary_geometric_normal_z) &&
			Math::is_finite(p_sample.secondary_shading_normal_x) && Math::is_finite(p_sample.secondary_shading_normal_y) && Math::is_finite(p_sample.secondary_shading_normal_z) &&
			Math::is_finite(p_sample.source_primary_world_position_x) && Math::is_finite(p_sample.source_primary_world_position_y) && Math::is_finite(p_sample.source_primary_world_position_z) &&
			Math::is_finite(p_sample.source_primary_geometric_normal_x) && Math::is_finite(p_sample.source_primary_geometric_normal_y) && Math::is_finite(p_sample.source_primary_geometric_normal_z) &&
			Math::is_finite(p_sample.source_primary_shading_normal_x) && Math::is_finite(p_sample.source_primary_shading_normal_y) && Math::is_finite(p_sample.source_primary_shading_normal_z) &&
			_finite_color(p_sample.throughput_r, p_sample.throughput_g, p_sample.throughput_b, p_sample.throughput_a) &&
			_finite_color(p_sample.incident_radiance_r, p_sample.incident_radiance_g, p_sample.incident_radiance_b, p_sample.incident_radiance_a) &&
			_finite_color(p_sample.outgoing_radiance_r, p_sample.outgoing_radiance_g, p_sample.outgoing_radiance_b, p_sample.outgoing_radiance_a) &&
			Math::is_finite(p_sample.source_primary_proposal_solid_angle_pdf) && Math::is_finite(p_sample.target) && Math::is_finite(p_sample.normalization) && Math::is_finite(p_sample.secondary_barycentric_u) && Math::is_finite(p_sample.secondary_barycentric_v);
}

static Vector3 _secondary_position(const ReusablePathSampleGpuRecord &p_sample) {
	return Vector3(p_sample.secondary_world_position_x, p_sample.secondary_world_position_y, p_sample.secondary_world_position_z);
}

static Vector3 _secondary_geometric_normal(const ReusablePathSampleGpuRecord &p_sample) {
	return Vector3(p_sample.secondary_geometric_normal_x, p_sample.secondary_geometric_normal_y, p_sample.secondary_geometric_normal_z);
}

static Vector3 _secondary_shading_normal(const ReusablePathSampleGpuRecord &p_sample) {
	return Vector3(p_sample.secondary_shading_normal_x, p_sample.secondary_shading_normal_y, p_sample.secondary_shading_normal_z);
}

static Vector3 _source_primary_position(const ReusablePathSampleGpuRecord &p_sample) {
	return Vector3(p_sample.source_primary_world_position_x, p_sample.source_primary_world_position_y, p_sample.source_primary_world_position_z);
}

static Vector3 _source_primary_geometric_normal(const ReusablePathSampleGpuRecord &p_sample) {
	return Vector3(p_sample.source_primary_geometric_normal_x, p_sample.source_primary_geometric_normal_y, p_sample.source_primary_geometric_normal_z);
}

static Vector3 _source_primary_shading_normal(const ReusablePathSampleGpuRecord &p_sample) {
	return Vector3(p_sample.source_primary_shading_normal_x, p_sample.source_primary_shading_normal_y, p_sample.source_primary_shading_normal_z);
}

static bool _normals_agree(const Vector3 &p_a, const Vector3 &p_b, float p_minimum_dot) {
	if (!p_a.is_finite() || !p_b.is_finite() || !Math::is_finite(p_minimum_dot) || p_minimum_dot < -1.0f || p_minimum_dot > 1.0f) {
		return false;
	}
	const float a_length_squared = p_a.length_squared();
	const float b_length_squared = p_b.length_squared();
	if (a_length_squared <= CMP_EPSILON2 || b_length_squared <= CMP_EPSILON2) {
		return false;
	}
	return p_a.dot(p_b) / Math::sqrt(a_length_squared * b_length_squared) >= p_minimum_dot;
}

static bool _normal_is_valid(const Vector3 &p_normal) {
	return p_normal.is_finite() && p_normal.length_squared() > CMP_EPSILON2;
}

static bool _thresholds_are_valid(const ReusablePathSampleValidationThresholds &p_thresholds) {
	return Math::is_finite(p_thresholds.maximum_source_primary_world_position_distance) && p_thresholds.maximum_source_primary_world_position_distance >= 0.0f &&
			Math::is_finite(p_thresholds.maximum_secondary_world_position_distance) && p_thresholds.maximum_secondary_world_position_distance >= 0.0f &&
			Math::is_finite(p_thresholds.minimum_geometric_normal_dot) && p_thresholds.minimum_geometric_normal_dot >= -1.0f && p_thresholds.minimum_geometric_normal_dot <= 1.0f &&
			Math::is_finite(p_thresholds.minimum_shading_normal_dot) && p_thresholds.minimum_shading_normal_dot >= -1.0f && p_thresholds.minimum_shading_normal_dot <= 1.0f;
}

static bool _endpoint_is_finite(const ReusablePathSampleEndpoint &p_endpoint) {
	return p_endpoint.world_position.is_finite() && p_endpoint.geometric_normal.is_finite() && p_endpoint.shading_normal.is_finite();
}

static bool _endpoint_identity_matches(uint64_t p_geometry_instance_id, uint64_t p_material_id, uint32_t p_surface_id, uint32_t p_primitive_id, const ReusablePathSampleEndpoint &p_endpoint) {
	return p_geometry_instance_id == p_endpoint.geometry_instance_id && p_material_id == p_endpoint.material_id && p_surface_id == p_endpoint.surface_id && p_primitive_id == p_endpoint.primitive_id;
}

} // namespace

ReusablePathSampleGpuRecord reusable_path_sample_gpu_from_authoring(const ReusablePathSampleAuthoring &p_authoring) {
	ReusablePathSampleGpuRecord result = {};
	result.secondary_geometry_instance_id = p_authoring.secondary.geometry_instance_id;
	result.secondary_material_id = p_authoring.secondary.material_id;
	result.secondary_surface_id = p_authoring.secondary.surface_id;
	result.secondary_primitive_id = p_authoring.secondary.primitive_id;
	result.source_primary_geometry_instance_id = p_authoring.source_primary.geometry_instance_id;
	result.source_primary_material_id = p_authoring.source_primary.material_id;
	result.source_primary_surface_id = p_authoring.source_primary.surface_id;
	result.source_primary_primitive_id = p_authoring.source_primary.primitive_id;
	result.abi_version = REUSABLE_PATH_SAMPLE_ABI_VERSION;
	result.flags = REUSABLE_PATH_SAMPLE_RECORD_VALID;
	result.secondary_geometry_revision = p_authoring.secondary.revisions.geometry;
	result.secondary_material_revision = p_authoring.secondary.revisions.material;
	result.secondary_residency_revision = p_authoring.secondary.revisions.residency;
	result.source_primary_geometry_revision = p_authoring.source_primary.revisions.geometry;
	result.source_primary_material_revision = p_authoring.source_primary.revisions.material;
	result.source_primary_residency_revision = p_authoring.source_primary.revisions.residency;
	result.lighting_revision = p_authoring.lighting_revision;
	result.environment_revision = p_authoring.environment_revision;
	result.source_primary_mask = p_authoring.source_primary.visibility_mask;
	result.secondary_mask = p_authoring.secondary.visibility_mask;
	result.capture_frame = p_authoring.capture_frame;
	result.secondary_world_position_x = p_authoring.secondary.world_position.x;
	result.secondary_world_position_y = p_authoring.secondary.world_position.y;
	result.secondary_world_position_z = p_authoring.secondary.world_position.z;
	result.secondary_geometric_normal_x = p_authoring.secondary.geometric_normal.x;
	result.secondary_geometric_normal_y = p_authoring.secondary.geometric_normal.y;
	result.secondary_geometric_normal_z = p_authoring.secondary.geometric_normal.z;
	result.secondary_shading_normal_x = p_authoring.secondary.shading_normal.x;
	result.secondary_shading_normal_y = p_authoring.secondary.shading_normal.y;
	result.secondary_shading_normal_z = p_authoring.secondary.shading_normal.z;
	result.source_primary_world_position_x = p_authoring.source_primary.world_position.x;
	result.source_primary_world_position_y = p_authoring.source_primary.world_position.y;
	result.source_primary_world_position_z = p_authoring.source_primary.world_position.z;
	result.source_primary_geometric_normal_x = p_authoring.source_primary.geometric_normal.x;
	result.source_primary_geometric_normal_y = p_authoring.source_primary.geometric_normal.y;
	result.source_primary_geometric_normal_z = p_authoring.source_primary.geometric_normal.z;
	result.source_primary_shading_normal_x = p_authoring.source_primary.shading_normal.x;
	result.source_primary_shading_normal_y = p_authoring.source_primary.shading_normal.y;
	result.source_primary_shading_normal_z = p_authoring.source_primary.shading_normal.z;
	result.throughput_r = p_authoring.throughput.r;
	result.throughput_g = p_authoring.throughput.g;
	result.throughput_b = p_authoring.throughput.b;
	result.throughput_a = p_authoring.throughput.a;
	result.incident_radiance_r = p_authoring.incident_radiance.r;
	result.incident_radiance_g = p_authoring.incident_radiance.g;
	result.incident_radiance_b = p_authoring.incident_radiance.b;
	result.incident_radiance_a = p_authoring.incident_radiance.a;
	result.outgoing_radiance_r = p_authoring.outgoing_radiance.r;
	result.outgoing_radiance_g = p_authoring.outgoing_radiance.g;
	result.outgoing_radiance_b = p_authoring.outgoing_radiance.b;
	result.outgoing_radiance_a = p_authoring.outgoing_radiance.a;
	result.source_primary_proposal_solid_angle_pdf = p_authoring.source_primary_proposal_solid_angle_pdf;
	result.secondary_barycentric_u = p_authoring.secondary_triangle_barycentric.x;
	result.secondary_barycentric_v = p_authoring.secondary_triangle_barycentric.y;
	result.target = p_authoring.target;
	result.normalization = p_authoring.normalization;
	result.represented_m = p_authoring.represented_m == 0 ? 1u : p_authoring.represented_m;
	result.age = p_authoring.age;
	return result;
}

bool reusable_path_sample_gpu_is_structurally_valid(const ReusablePathSampleGpuRecord &p_sample) {
	return p_sample.abi_version == REUSABLE_PATH_SAMPLE_ABI_VERSION && (p_sample.flags & REUSABLE_PATH_SAMPLE_RECORD_VALID) != 0 &&
			p_sample.secondary_geometry_instance_id != 0 && p_sample.secondary_material_id != 0 && p_sample.source_primary_geometry_instance_id != 0 && p_sample.source_primary_material_id != 0 &&
			p_sample.source_primary_mask != 0 && p_sample.secondary_mask != 0 && _finite_record(p_sample) &&
			p_sample.source_primary_proposal_solid_angle_pdf > 0.0f && p_sample.target >= 0.0f && p_sample.normalization >= 0.0f && p_sample.represented_m > 0 &&
			p_sample.secondary_barycentric_u >= 0.0f && p_sample.secondary_barycentric_v >= 0.0f && p_sample.secondary_barycentric_u + p_sample.secondary_barycentric_v <= 1.0f;
}

bool reusable_path_sample_source_solid_angle_to_secondary_area_pdf(const ReusablePathSampleGpuRecord &p_sample, float &r_secondary_area_pdf) {
	r_secondary_area_pdf = 0.0f;
	if (!_finite_record(p_sample) || !Math::is_finite(p_sample.source_primary_proposal_solid_angle_pdf) || p_sample.source_primary_proposal_solid_angle_pdf <= 0.0f) {
		return false;
	}
	const Vector3 source = _source_primary_position(p_sample);
	const Vector3 secondary = _secondary_position(p_sample);
	if (!_normal_is_valid(_source_primary_geometric_normal(p_sample)) || !_normal_is_valid(_source_primary_shading_normal(p_sample)) || !_normal_is_valid(_secondary_geometric_normal(p_sample)) || !_normal_is_valid(_secondary_shading_normal(p_sample))) {
		return false;
	}
	const Vector3 displacement = secondary - source;
	const float distance_squared = displacement.length_squared();
	if (!Math::is_finite(distance_squared) || distance_squared <= CMP_EPSILON2) {
		return false;
	}
	const Vector3 direction = displacement / Math::sqrt(distance_squared);
	const Vector3 secondary_normal = _secondary_geometric_normal(p_sample).normalized();
	if (secondary_normal.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	const float secondary_cosine = Math::abs(secondary_normal.dot(-direction));
	if (!Math::is_finite(secondary_cosine) || secondary_cosine <= CMP_EPSILON) {
		return false;
	}
	r_secondary_area_pdf = p_sample.source_primary_proposal_solid_angle_pdf * secondary_cosine / distance_squared;
	return Math::is_finite(r_secondary_area_pdf) && r_secondary_area_pdf > 0.0f;
}

bool reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(const ReusablePathSampleGpuRecord &p_sample, const ReusablePathSampleEndpoint &p_current_primary, float &r_current_primary_solid_angle_pdf) {
	r_current_primary_solid_angle_pdf = 0.0f;
	if (!_endpoint_is_finite(p_current_primary) || !_normal_is_valid(p_current_primary.geometric_normal) || !_normal_is_valid(p_current_primary.shading_normal)) {
		return false;
	}
	float secondary_area_pdf = 0.0f;
	if (!reusable_path_sample_source_solid_angle_to_secondary_area_pdf(p_sample, secondary_area_pdf)) {
		return false;
	}
	const Vector3 secondary = _secondary_position(p_sample);
	const Vector3 displacement = secondary - p_current_primary.world_position;
	const float distance_squared = displacement.length_squared();
	if (!Math::is_finite(distance_squared) || distance_squared <= CMP_EPSILON2) {
		return false;
	}
	const Vector3 direction = displacement / Math::sqrt(distance_squared);
	const Vector3 secondary_normal = _secondary_geometric_normal(p_sample).normalized();
	if (secondary_normal.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	const float secondary_cosine = Math::abs(secondary_normal.dot(-direction));
	if (!Math::is_finite(secondary_cosine) || secondary_cosine <= CMP_EPSILON) {
		return false;
	}
	r_current_primary_solid_angle_pdf = secondary_area_pdf * distance_squared / secondary_cosine;
	return Math::is_finite(r_current_primary_solid_angle_pdf) && r_current_primary_solid_angle_pdf > 0.0f;
}

Vector3i reusable_path_sample_world_cell_key(const Vector3 &p_source_primary_world_position, float p_cell_size) {
	if (!p_source_primary_world_position.is_finite() || !Math::is_finite(p_cell_size) || p_cell_size <= 0.0f) {
		return Vector3i();
	}
	return Vector3i(
			int32_t(Math::floor(p_source_primary_world_position.x / p_cell_size)),
			int32_t(Math::floor(p_source_primary_world_position.y / p_cell_size)),
			int32_t(Math::floor(p_source_primary_world_position.z / p_cell_size)));
}

bool reusable_path_sample_world_cache_query_adjacent(const Vector<ReusablePathSampleWorldCell> &p_cells, const Vector3i &p_key, ReusablePathSampleGpuRecord &r_record) {
	for (const ReusablePathSampleWorldCell &cell : p_cells) {
		const Vector3i delta = cell.key - p_key;
		if (std::abs(delta.x) <= 1 && std::abs(delta.y) <= 1 && std::abs(delta.z) <= 1 && reusable_path_sample_gpu_is_structurally_valid(cell.record)) {
			r_record = cell.record;
			return true;
		}
	}
	return false;
}

void reusable_path_sample_world_cache_reduce(const Vector<ReusablePathSampleWorldCell> &p_previous, const Vector<ReusablePathSampleWorldCell> &p_staging, Vector<ReusablePathSampleWorldCell> &r_next) {
	r_next = p_previous;
	for (const ReusablePathSampleWorldCell &staged : p_staging) {
		if (!reusable_path_sample_gpu_is_structurally_valid(staged.record)) {
			continue;
		}
		bool replaced = false;
		for (ReusablePathSampleWorldCell &next : r_next) {
			if (next.key == staged.key) {
				next.record = staged.record;
				replaced = true;
				break;
			}
		}
		if (!replaced) {
			r_next.push_back(staged);
		}
	}
}

ReusablePathSampleValidationResult validate_reusable_path_sample(const ReusablePathSampleGpuRecord &p_sample, const ReusablePathSampleCurrentState &p_current, const ReusablePathSampleValidationThresholds &p_thresholds) {
	ReusablePathSampleValidationResult result;
	if (p_sample.abi_version != REUSABLE_PATH_SAMPLE_ABI_VERSION) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_ABI;
	}
	if ((p_sample.flags & REUSABLE_PATH_SAMPLE_RECORD_VALID) == 0 || p_sample.secondary_geometry_instance_id == 0 || p_sample.secondary_material_id == 0 || p_sample.source_primary_geometry_instance_id == 0 || p_sample.source_primary_material_id == 0) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_INVALID_RECORD;
	}
	if (!_finite_record(p_sample) || !_endpoint_is_finite(p_current.source_primary) || !_endpoint_is_finite(p_current.secondary) || !_thresholds_are_valid(p_thresholds)) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_NONFINITE;
	}
	if (!Math::is_finite(p_sample.source_primary_proposal_solid_angle_pdf) || p_sample.source_primary_proposal_solid_angle_pdf <= 0.0f) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_NONPOSITIVE_PDF;
	}
	if (!Math::is_finite(p_sample.secondary_barycentric_u) || !Math::is_finite(p_sample.secondary_barycentric_v) || p_sample.secondary_barycentric_u < 0.0f || p_sample.secondary_barycentric_v < 0.0f || p_sample.secondary_barycentric_u + p_sample.secondary_barycentric_v > 1.0f) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_BARYCENTRIC;
	}
	if (!_endpoint_identity_matches(p_sample.source_primary_geometry_instance_id, p_sample.source_primary_material_id, p_sample.source_primary_surface_id, p_sample.source_primary_primitive_id, p_current.source_primary)) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_IDENTITY_MISMATCH;
	}
	if (!_endpoint_identity_matches(p_sample.secondary_geometry_instance_id, p_sample.secondary_material_id, p_sample.secondary_surface_id, p_sample.secondary_primitive_id, p_current.secondary)) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_IDENTITY_MISMATCH;
	}
	if (p_sample.source_primary_geometry_revision != p_current.source_primary.revisions.geometry) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_GEOMETRY_REVISION_MISMATCH;
	}
	if (p_sample.secondary_geometry_revision != p_current.secondary.revisions.geometry) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_GEOMETRY_REVISION_MISMATCH;
	}
	if (p_sample.source_primary_material_revision != p_current.source_primary.revisions.material) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_MATERIAL_REVISION_MISMATCH;
	}
	if (p_sample.secondary_material_revision != p_current.secondary.revisions.material) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_MATERIAL_REVISION_MISMATCH;
	}
	if (p_sample.lighting_revision != p_current.lighting_revision) {
		result.revalidation_reasons |= REUSABLE_PATH_SAMPLE_REVALIDATE_LIGHTING_REVISION_MISMATCH;
	}
	if (p_sample.environment_revision != p_current.environment_revision) {
		result.revalidation_reasons |= REUSABLE_PATH_SAMPLE_REVALIDATE_ENVIRONMENT_REVISION_MISMATCH;
	}
	if (p_sample.source_primary_residency_revision != p_current.source_primary.revisions.residency) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_RESIDENCY_REVISION_MISMATCH;
	}
	if (p_sample.secondary_residency_revision != p_current.secondary.revisions.residency) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_RESIDENCY_REVISION_MISMATCH;
	}
	if (p_current.current_frame < p_sample.capture_frame || p_current.current_frame - p_sample.capture_frame > p_thresholds.maximum_age || p_sample.age > p_thresholds.maximum_age) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_EXCESSIVE_AGE;
	}
	if (_source_primary_position(p_sample).distance_to(p_current.source_primary.world_position) > p_thresholds.maximum_source_primary_world_position_distance) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_WORLD_POSITION;
	}
	if (_secondary_position(p_sample).distance_to(p_current.secondary.world_position) > p_thresholds.maximum_secondary_world_position_distance) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_WORLD_POSITION;
	}
	if (!_normals_agree(_source_primary_geometric_normal(p_sample), p_current.source_primary.geometric_normal, p_thresholds.minimum_geometric_normal_dot) || !_normals_agree(_source_primary_shading_normal(p_sample), p_current.source_primary.shading_normal, p_thresholds.minimum_shading_normal_dot)) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_NORMAL_DISAGREEMENT;
	}
	if (!_normals_agree(_secondary_geometric_normal(p_sample), p_current.secondary.geometric_normal, p_thresholds.minimum_geometric_normal_dot) || !_normals_agree(_secondary_shading_normal(p_sample), p_current.secondary.shading_normal, p_thresholds.minimum_shading_normal_dot)) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_NORMAL_DISAGREEMENT;
	}
	if ((p_sample.source_primary_mask & p_current.source_primary.visibility_mask) == 0) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_MASK;
	}
	if ((p_sample.secondary_mask & p_current.secondary.visibility_mask) == 0) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_MASK;
	}
	if (p_current.source_primary.dynamic_geometry) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SOURCE_PRIMARY_DYNAMIC_GEOMETRY;
	}
	if (p_current.secondary.dynamic_geometry) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_SECONDARY_DYNAMIC_GEOMETRY;
	}
	if (p_current.source_primary.disoccluded || p_current.secondary.disoccluded) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_DISOCCLUSION;
	}
	float current_primary_solid_angle_pdf = 0.0f;
	if (!reusable_path_sample_secondary_area_to_current_primary_solid_angle_pdf(p_sample, p_current.source_primary, current_primary_solid_angle_pdf)) {
		result.rejection_reasons |= REUSABLE_PATH_SAMPLE_REJECT_PROPOSAL_MEASURE;
	}
	result.may_reuse_after_revalidation = result.rejection_reasons == REUSABLE_PATH_SAMPLE_REJECT_NONE;
	return result;
}

uint64_t reusable_path_sample_identity_checksum(const ReusablePathSampleGpuRecord &p_sample) {
	uint64_t hash = 1469598103934665603ULL;
	hash = _fnv_append_u32(hash, p_sample.abi_version);
	const uint64_t values[] = {
		p_sample.secondary_geometry_instance_id, p_sample.secondary_material_id,
		p_sample.source_primary_geometry_instance_id, p_sample.source_primary_material_id,
		p_sample.secondary_geometry_revision, p_sample.secondary_material_revision, p_sample.secondary_residency_revision,
		p_sample.source_primary_geometry_revision, p_sample.source_primary_material_revision, p_sample.source_primary_residency_revision,
		p_sample.source_primary_mask, p_sample.secondary_mask,
	};
	for (uint64_t value : values) {
		hash = _fnv_append_u64(hash, value);
	}
	hash = _fnv_append_u32(hash, p_sample.secondary_surface_id);
	hash = _fnv_append_u32(hash, p_sample.secondary_primitive_id);
	hash = _fnv_append_u32(hash, p_sample.source_primary_surface_id);
	hash = _fnv_append_u32(hash, p_sample.source_primary_primitive_id);
	return hash;
}

uint64_t reusable_path_sample_replay_checksum(const ReusablePathSampleGpuRecord &p_sample) {
	uint64_t hash = reusable_path_sample_identity_checksum(p_sample);
	hash = _fnv_append_u64(hash, p_sample.lighting_revision);
	hash = _fnv_append_u64(hash, p_sample.environment_revision);
	hash = _fnv_append_u64(hash, p_sample.capture_frame);
	const float values[] = {
		p_sample.secondary_world_position_x, p_sample.secondary_world_position_y, p_sample.secondary_world_position_z,
		p_sample.secondary_geometric_normal_x, p_sample.secondary_geometric_normal_y, p_sample.secondary_geometric_normal_z,
		p_sample.secondary_shading_normal_x, p_sample.secondary_shading_normal_y, p_sample.secondary_shading_normal_z,
		p_sample.source_primary_world_position_x, p_sample.source_primary_world_position_y, p_sample.source_primary_world_position_z,
		p_sample.source_primary_geometric_normal_x, p_sample.source_primary_geometric_normal_y, p_sample.source_primary_geometric_normal_z,
		p_sample.source_primary_shading_normal_x, p_sample.source_primary_shading_normal_y, p_sample.source_primary_shading_normal_z,
		p_sample.throughput_r, p_sample.throughput_g, p_sample.throughput_b, p_sample.throughput_a,
		p_sample.incident_radiance_r, p_sample.incident_radiance_g, p_sample.incident_radiance_b, p_sample.incident_radiance_a,
		p_sample.outgoing_radiance_r, p_sample.outgoing_radiance_g, p_sample.outgoing_radiance_b, p_sample.outgoing_radiance_a,
		p_sample.source_primary_proposal_solid_angle_pdf, p_sample.target, p_sample.normalization,
		p_sample.secondary_barycentric_u, p_sample.secondary_barycentric_v,
	};
	for (float value : values) {
		hash = _fnv_append_float(hash, value);
	}
	hash = _fnv_append_u32(hash, p_sample.age);
	hash = _fnv_append_u32(hash, p_sample.represented_m);
	return hash;
}

} // namespace RendererPathTracing
