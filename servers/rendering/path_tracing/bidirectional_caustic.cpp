/**************************************************************************/
/*  bidirectional_caustic.cpp                                             */
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

#include "bidirectional_caustic.h"

#include "core/math/math_funcs.h"

#include <cmath>
#include <cstring>

namespace RendererPathTracing {

namespace {

static bool _finite_color(const Color &p_color) {
	return Math::is_finite(p_color.r) && Math::is_finite(p_color.g) && Math::is_finite(p_color.b) && Math::is_finite(p_color.a);
}

static bool _finite_normal(const Vector3 &p_normal) {
	return p_normal.is_finite() && p_normal.length_squared() > CMP_EPSILON2;
}

static bool _finite_positive(float p_value) {
	return Math::is_finite(p_value) && p_value > 0.0f;
}

static bool _record_is_finite(const BidirectionalCausticPathRecord &p_record) {
	if (!p_record.source.world_position.is_finite() || !p_record.source.outgoing_direction.is_finite() || !_finite_color(p_record.source.radiance) || !_finite_positive(p_record.source.selection_probability_mass) || !_finite_color(p_record.throughput)) {
		return false;
	}
	for (uint32_t i = 0; i < p_record.delta_vertex_count && i < BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES; i++) {
		const BidirectionalCausticDeltaVertex &vertex = p_record.delta_vertices[i];
		if (!vertex.world_position.is_finite() || !_finite_normal(vertex.geometric_normal) || !_finite_positive(vertex.forward_probability_mass) || !_finite_positive(vertex.reverse_probability_mass) || !_finite_positive(vertex.incident_ior) || !_finite_positive(vertex.transmitted_ior)) {
			return false;
		}
	}
	return true;
}

static bool _receiver_is_finite(const BidirectionalCausticCurrentReceiver &p_receiver) {
	return p_receiver.world_position.is_finite() && _finite_normal(p_receiver.geometric_normal) && _finite_color(p_receiver.current_bsdf_value);
}

static bool _strict_delta_vertex(const BidirectionalCausticDeltaVertex &p_vertex) {
	const uint32_t event = p_vertex.flags & (BIDIRECTIONAL_CAUSTIC_DELTA_REFLECTION | BIDIRECTIONAL_CAUSTIC_DELTA_REFRACTION);
	if ((event != BIDIRECTIONAL_CAUSTIC_DELTA_REFLECTION && event != BIDIRECTIONAL_CAUSTIC_DELTA_REFRACTION) || p_vertex.flags != event) {
		return false;
	}
	if (p_vertex.geometry_instance_id == 0 || p_vertex.material_id == 0 || p_vertex.visibility_mask == 0 || !_finite_positive(p_vertex.forward_probability_mass) || !_finite_positive(p_vertex.reverse_probability_mass) || !_finite_positive(p_vertex.incident_ior) || !_finite_positive(p_vertex.transmitted_ior)) {
		return false;
	}
	// Refraction needs a real IOR transition. Reflection still records both
	// media so future adapters do not have to infer a missing measure field.
	if (event == BIDIRECTIONAL_CAUSTIC_DELTA_REFRACTION && Math::is_equal_approx(p_vertex.incident_ior, p_vertex.transmitted_ior)) {
		return false;
	}
	return true;
}

static uint64_t _fnv_u64(uint64_t p_hash, uint64_t p_value) {
	for (uint32_t byte = 0; byte < 8; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= 1099511628211ULL;
	}
	return p_hash;
}

static uint64_t _fnv_u32(uint64_t p_hash, uint32_t p_value) {
	for (uint32_t byte = 0; byte < 4; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= 1099511628211ULL;
	}
	return p_hash;
}

static uint32_t _float_bits(float p_value) {
	if (p_value == 0.0f) {
		return 0;
	}
	if (std::isnan(p_value)) {
		return 0x7fc00000U;
	}
	uint32_t bits = 0;
	std::memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

static uint64_t _fnv_float(uint64_t p_hash, float p_value) {
	return _fnv_u32(p_hash, _float_bits(p_value));
}

static uint64_t _fnv_vector3(uint64_t p_hash, const Vector3 &p_value) {
	p_hash = _fnv_float(p_hash, p_value.x);
	p_hash = _fnv_float(p_hash, p_value.y);
	return _fnv_float(p_hash, p_value.z);
}

static uint64_t _fnv_color(uint64_t p_hash, const Color &p_value) {
	p_hash = _fnv_float(p_hash, p_value.r);
	p_hash = _fnv_float(p_hash, p_value.g);
	p_hash = _fnv_float(p_hash, p_value.b);
	return _fnv_float(p_hash, p_value.a);
}

static uint64_t _append_vertex_identity(uint64_t p_hash, const BidirectionalCausticDeltaVertex &p_vertex) {
	p_hash = _fnv_u64(p_hash, p_vertex.geometry_instance_id);
	p_hash = _fnv_u64(p_hash, p_vertex.material_id);
	p_hash = _fnv_u32(p_hash, p_vertex.surface_id);
	p_hash = _fnv_u32(p_hash, p_vertex.primitive_id);
	p_hash = _fnv_u64(p_hash, p_vertex.visibility_mask);
	p_hash = _fnv_u64(p_hash, p_vertex.revisions.geometry);
	p_hash = _fnv_u64(p_hash, p_vertex.revisions.material);
	return _fnv_u64(p_hash, p_vertex.revisions.residency);
}

} // namespace

bool bidirectional_caustic_is_active(const BidirectionalCausticPathRecord &p_record) {
	return p_record.abi_version == BIDIRECTIONAL_CAUSTIC_ABI_VERSION && (p_record.source.flags & (BIDIRECTIONAL_CAUSTIC_SOURCE_VALID | BIDIRECTIONAL_CAUSTIC_SOURCE_COMPACT_HIGH_ENERGY)) == (BIDIRECTIONAL_CAUSTIC_SOURCE_VALID | BIDIRECTIONAL_CAUSTIC_SOURCE_COMPACT_HIGH_ENERGY) && p_record.delta_vertex_count >= 1 && p_record.delta_vertex_count <= BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES;
}

BidirectionalCausticValidationResult validate_bidirectional_caustic_path(const BidirectionalCausticPathRecord &p_record, const BidirectionalCausticCurrentReceiver &p_receiver, const BidirectionalCausticCurrentState &p_current, const BidirectionalCausticValidationThresholds &p_thresholds) {
	BidirectionalCausticValidationResult result;
	if (p_record.abi_version != BIDIRECTIONAL_CAUSTIC_ABI_VERSION) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_ABI;
	}
	if (!bidirectional_caustic_is_active(p_record) || p_record.source.source_id == 0 || p_record.source.sample_id == 0 || p_record.source.visibility_mask == 0) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_INACTIVE_SOURCE;
	}
	if (!_record_is_finite(p_record) || !_receiver_is_finite(p_receiver) || !Math::is_finite(p_thresholds.maximum_receiver_distance) || p_thresholds.maximum_receiver_distance < 0.0f) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_NONFINITE;
	}
	if ((p_receiver.flags & (BIDIRECTIONAL_CAUSTIC_RECEIVER_VALID | BIDIRECTIONAL_CAUSTIC_RECEIVER_DIFFUSE_OR_ROUGH)) != (BIDIRECTIONAL_CAUSTIC_RECEIVER_VALID | BIDIRECTIONAL_CAUSTIC_RECEIVER_DIFFUSE_OR_ROUGH)) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_RECEIVER;
	}
	if (p_receiver.geometry_instance_id == 0 || p_receiver.material_id == 0 || p_receiver.visibility_mask == 0) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_RECEIVER;
	}
	if ((p_receiver.flags & (BIDIRECTIONAL_CAUSTIC_RECEIVER_DYNAMIC_GEOMETRY | BIDIRECTIONAL_CAUSTIC_RECEIVER_DISOCCLUDED | BIDIRECTIONAL_CAUSTIC_RECEIVER_ALPHA_TESTED)) != 0) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_DYNAMIC_OR_DISOCCLUDED;
	}
	if (p_record.capture_frame > p_current.current_frame || p_record.age > p_thresholds.maximum_age || p_current.current_frame - p_record.capture_frame > p_thresholds.maximum_age) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_AGE;
	}
	if (p_record.lighting_revision != p_current.lighting_revision || p_record.environment_revision != p_current.environment_revision || p_record.residency_revision != p_current.residency_revision ||
			p_record.source.revisions.geometry != p_current.source_revisions.geometry || p_record.source.revisions.material != p_current.source_revisions.material || p_record.source.revisions.residency != p_current.source_revisions.residency) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_REVISION;
	}
	if (!p_current.current_connection_available) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_CONNECTION;
	}
	if (!p_current.current_visibility_evaluated || !p_current.current_visibility_unoccluded) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_VISIBILITY;
	}
	for (uint32_t i = 0; i < p_record.delta_vertex_count && i < BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES; i++) {
		const BidirectionalCausticDeltaVertex &vertex = p_record.delta_vertices[i];
		if (!_strict_delta_vertex(vertex)) {
			result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_DELTA_CHAIN;
		}
		if ((p_record.source.visibility_mask & vertex.visibility_mask & p_receiver.visibility_mask) == 0) {
			result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_MASK;
		}
		const BidirectionalCausticRevisions &current_revisions = p_current.delta_revisions[i];
		if (vertex.revisions.geometry != current_revisions.geometry || vertex.revisions.material != current_revisions.material || vertex.revisions.residency != current_revisions.residency) {
			result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_REVISION;
		}
	}
	if ((p_record.source.visibility_mask & p_receiver.visibility_mask) == 0) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_MASK;
	}
	const BidirectionalCausticDeltaVertex *last = p_record.delta_vertex_count > 0 && p_record.delta_vertex_count <= BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES ? &p_record.delta_vertices[p_record.delta_vertex_count - 1] : nullptr;
	if (last != nullptr && p_thresholds.maximum_receiver_distance > 0.0f && (last->world_position - p_receiver.world_position).length() > p_thresholds.maximum_receiver_distance) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_DISTANCE;
	}
	float probability_mass = 0.0f;
	if (!bidirectional_caustic_source_path_probability_mass(p_record, probability_mass)) {
		result.rejection_reasons |= BIDIRECTIONAL_CAUSTIC_REJECT_DENSITY;
	}
	result.work_required = bidirectional_caustic_is_active(p_record) && result.rejection_reasons == BIDIRECTIONAL_CAUSTIC_REJECT_NONE;
	result.contribution_permitted = result.work_required;
	return result;
}

bool bidirectional_caustic_solid_angle_to_area_pdf(float p_solid_angle_pdf, const Vector3 &p_from, const Vector3 &p_to, const Vector3 &p_to_normal, float &r_area_pdf) {
	r_area_pdf = 0.0f;
	if (!_finite_positive(p_solid_angle_pdf) || !p_from.is_finite() || !p_to.is_finite() || !_finite_normal(p_to_normal)) {
		return false;
	}
	const Vector3 displacement = p_to - p_from;
	const float distance_squared = displacement.length_squared();
	if (!Math::is_finite(distance_squared) || distance_squared <= CMP_EPSILON2) {
		return false;
	}
	const float cosine = Math::abs(p_to_normal.normalized().dot(-displacement / Math::sqrt(distance_squared)));
	if (!_finite_positive(cosine)) {
		return false;
	}
	r_area_pdf = p_solid_angle_pdf * cosine / distance_squared;
	return _finite_positive(r_area_pdf);
}

bool bidirectional_caustic_area_to_solid_angle_pdf(float p_area_pdf, const Vector3 &p_from, const Vector3 &p_to, const Vector3 &p_to_normal, float &r_solid_angle_pdf) {
	r_solid_angle_pdf = 0.0f;
	if (!_finite_positive(p_area_pdf) || !p_from.is_finite() || !p_to.is_finite() || !_finite_normal(p_to_normal)) {
		return false;
	}
	const Vector3 displacement = p_to - p_from;
	const float distance_squared = displacement.length_squared();
	if (!Math::is_finite(distance_squared) || distance_squared <= CMP_EPSILON2) {
		return false;
	}
	const float cosine = Math::abs(p_to_normal.normalized().dot(-displacement / Math::sqrt(distance_squared)));
	if (!_finite_positive(cosine)) {
		return false;
	}
	r_solid_angle_pdf = p_area_pdf * distance_squared / cosine;
	return _finite_positive(r_solid_angle_pdf);
}

bool bidirectional_caustic_delta_competing_continuous_density(const BidirectionalCausticDeltaVertex &p_vertex, float &r_density) {
	r_density = 0.0f;
	return _strict_delta_vertex(p_vertex);
}

bool bidirectional_caustic_source_path_probability_mass(const BidirectionalCausticPathRecord &p_record, float &r_probability_mass) {
	r_probability_mass = 0.0f;
	if (!bidirectional_caustic_is_active(p_record) || !_finite_positive(p_record.source.selection_probability_mass)) {
		return false;
	}
	float mass = p_record.source.selection_probability_mass;
	for (uint32_t i = 0; i < p_record.delta_vertex_count; i++) {
		if (!_strict_delta_vertex(p_record.delta_vertices[i])) {
			return false;
		}
		mass *= p_record.delta_vertices[i].forward_probability_mass;
		if (!_finite_positive(mass)) {
			return false;
		}
	}
	r_probability_mass = mass;
	return true;
}

BidirectionalCausticContribution evaluate_bidirectional_caustic_connection(const BidirectionalCausticPathRecord &p_record, const BidirectionalCausticCurrentReceiver &p_receiver, const BidirectionalCausticCurrentState &p_current, const BidirectionalCausticValidationThresholds &p_thresholds) {
	BidirectionalCausticContribution result;
	const BidirectionalCausticValidationResult validation = validate_bidirectional_caustic_path(p_record, p_receiver, p_current, p_thresholds);
	if (!validation.contribution_permitted) {
		return result;
	}
	const BidirectionalCausticDeltaVertex &last = p_record.delta_vertices[p_record.delta_vertex_count - 1];
	const Vector3 displacement = p_receiver.world_position - last.world_position;
	const float distance_squared = displacement.length_squared();
	if (!Math::is_finite(distance_squared) || distance_squared <= CMP_EPSILON2) {
		return result;
	}
	const Vector3 direction = displacement / Math::sqrt(distance_squared);
	const float last_cosine = Math::abs(last.geometric_normal.normalized().dot(direction));
	const float receiver_cosine = Math::abs(p_receiver.geometric_normal.normalized().dot(-direction));
	const float geometry = last_cosine * receiver_cosine / distance_squared;
	float path_mass = 0.0f;
	float competing_density = 0.0f;
	if (!_finite_positive(geometry) || !bidirectional_caustic_source_path_probability_mass(p_record, path_mass) || !bidirectional_caustic_delta_competing_continuous_density(last, competing_density)) {
		return result;
	}
	result.geometry_term = geometry;
	result.source_path_probability_mass = path_mass;
	result.competing_continuous_density = competing_density;
	result.mis_weight = 1.0f;
	result.value = Color(
			p_record.source.radiance.r * p_record.throughput.r * p_receiver.current_bsdf_value.r * geometry / path_mass,
			p_record.source.radiance.g * p_record.throughput.g * p_receiver.current_bsdf_value.g * geometry / path_mass,
			p_record.source.radiance.b * p_record.throughput.b * p_receiver.current_bsdf_value.b * geometry / path_mass,
			1.0f);
	result.valid = _finite_color(result.value);
	return result;
}

bool bidirectional_caustic_build_planar_virtual_connection(const BidirectionalCausticPlanarMirrorTriangle &p_mirror, const Vector3 &p_receiver_position, const Vector3 &p_receiver_normal, const Vector3 &p_source_position, const Vector3 &p_source_normal, float p_source_area_pdf, BidirectionalCausticPlanarVirtualConnection &r_connection) {
	r_connection = BidirectionalCausticPlanarVirtualConnection();
	if (!p_mirror.p0.is_finite() || !p_mirror.p1.is_finite() || !p_mirror.p2.is_finite() || !_finite_normal(p_mirror.geometric_normal) || !p_receiver_position.is_finite() || !_finite_normal(p_receiver_normal) || !p_source_position.is_finite() || !_finite_normal(p_source_normal) || !_finite_positive(p_source_area_pdf) || !_finite_positive(p_mirror.selection_probability_mass)) {
		return false;
	}
	const Vector3 normal = p_mirror.geometric_normal.normalized();
	const float plane_area_twice = (p_mirror.p1 - p_mirror.p0).cross(p_mirror.p2 - p_mirror.p0).length();
	if (!Math::is_finite(plane_area_twice) || plane_area_twice <= CMP_EPSILON) {
		return false;
	}
	const float source_plane_distance = (p_source_position - p_mirror.p0).dot(normal);
	const Vector3 virtual_source = p_source_position - normal * (2.0f * source_plane_distance);
	const Vector3 to_virtual = virtual_source - p_receiver_position;
	const float distance_squared = to_virtual.length_squared();
	if (!Math::is_finite(distance_squared) || distance_squared <= CMP_EPSILON2) {
		return false;
	}
	const float denominator = to_virtual.dot(normal);
	if (!Math::is_finite(denominator) || Math::abs(denominator) <= CMP_EPSILON) {
		return false;
	}
	const float t = (p_mirror.p0 - p_receiver_position).dot(normal) / denominator;
	if (!Math::is_finite(t) || t <= CMP_EPSILON || t >= 1.0f - CMP_EPSILON) {
		return false;
	}
	const Vector3 mirror_position = p_receiver_position + to_virtual * t;
	const Vector3 edge0 = p_mirror.p1 - p_mirror.p0;
	const Vector3 edge1 = p_mirror.p2 - p_mirror.p0;
	const Vector3 offset = mirror_position - p_mirror.p0;
	const float dot00 = edge0.dot(edge0);
	const float dot01 = edge0.dot(edge1);
	const float dot11 = edge1.dot(edge1);
	const float dot20 = offset.dot(edge0);
	const float dot21 = offset.dot(edge1);
	const float barycentric_denominator = dot00 * dot11 - dot01 * dot01;
	if (!Math::is_finite(barycentric_denominator) || Math::abs(barycentric_denominator) <= CMP_EPSILON) {
		return false;
	}
	const float bary_y = (dot11 * dot20 - dot01 * dot21) / barycentric_denominator;
	const float bary_z = (dot00 * dot21 - dot01 * dot20) / barycentric_denominator;
	const float bary_x = 1.0f - bary_y - bary_z;
	// Strict interior removes edge ambiguity between adjacent mirror triangles.
	if (!Math::is_finite(bary_x) || !Math::is_finite(bary_y) || !Math::is_finite(bary_z) || bary_x <= CMP_EPSILON || bary_y <= CMP_EPSILON || bary_z <= CMP_EPSILON) {
		return false;
	}
	const Vector3 receiver_direction = to_virtual / Math::sqrt(distance_squared);
	const Vector3 virtual_normal = p_source_normal.normalized() - normal * (2.0f * p_source_normal.normalized().dot(normal));
	const float receiver_cosine = p_receiver_normal.normalized().dot(receiver_direction);
	const float source_cosine = virtual_normal.dot(-receiver_direction);
	if (!_finite_positive(receiver_cosine) || !_finite_positive(source_cosine) || !_finite_normal(virtual_normal)) {
		return false;
	}
	// Reflection is an isometry: source and virtual-source area measures are
	// equal. The discrete mirror-selection mass remains part of the proposal.
	const float proposal_area_pdf = p_source_area_pdf * p_mirror.selection_probability_mass;
	if (!_finite_positive(proposal_area_pdf)) {
		return false;
	}
	r_connection.mirror_position = mirror_position;
	r_connection.virtual_source_position = virtual_source;
	r_connection.virtual_source_normal = virtual_normal.normalized();
	r_connection.barycentric = Vector3(bary_x, bary_y, bary_z);
	r_connection.receiver_to_virtual_distance_squared = distance_squared;
	r_connection.source_area_pdf = p_source_area_pdf;
	r_connection.mirror_selection_probability_mass = p_mirror.selection_probability_mass;
	r_connection.proposal_area_pdf = proposal_area_pdf;
	r_connection.valid = true;
	return true;
}

uint64_t bidirectional_caustic_identity_checksum(const BidirectionalCausticPathRecord &p_record) {
	uint64_t hash = 1469598103934665603ULL;
	hash = _fnv_u32(hash, p_record.abi_version);
	hash = _fnv_u32(hash, p_record.delta_vertex_count);
	hash = _fnv_u64(hash, p_record.source.source_id);
	hash = _fnv_u64(hash, p_record.source.sample_id);
	hash = _fnv_u64(hash, p_record.source.visibility_mask);
	hash = _fnv_u64(hash, p_record.source.revisions.geometry);
	hash = _fnv_u64(hash, p_record.source.revisions.material);
	hash = _fnv_u64(hash, p_record.source.revisions.residency);
	for (uint32_t i = 0; i < p_record.delta_vertex_count && i < BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES; i++) {
		hash = _append_vertex_identity(hash, p_record.delta_vertices[i]);
	}
	return hash;
}

uint64_t bidirectional_caustic_replay_checksum(const BidirectionalCausticPathRecord &p_record) {
	uint64_t hash = bidirectional_caustic_identity_checksum(p_record);
	hash = _fnv_u64(hash, p_record.capture_frame);
	hash = _fnv_u32(hash, p_record.age);
	hash = _fnv_u64(hash, p_record.lighting_revision);
	hash = _fnv_u64(hash, p_record.environment_revision);
	hash = _fnv_u64(hash, p_record.residency_revision);
	hash = _fnv_vector3(hash, p_record.source.world_position);
	hash = _fnv_vector3(hash, p_record.source.outgoing_direction);
	hash = _fnv_color(hash, p_record.source.radiance);
	hash = _fnv_float(hash, p_record.source.selection_probability_mass);
	hash = _fnv_u32(hash, p_record.source.flags);
	for (uint32_t i = 0; i < p_record.delta_vertex_count && i < BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES; i++) {
		const BidirectionalCausticDeltaVertex &vertex = p_record.delta_vertices[i];
		hash = _fnv_vector3(hash, vertex.world_position);
		hash = _fnv_vector3(hash, vertex.geometric_normal);
		hash = _fnv_float(hash, vertex.forward_probability_mass);
		hash = _fnv_float(hash, vertex.reverse_probability_mass);
		hash = _fnv_float(hash, vertex.incident_ior);
		hash = _fnv_float(hash, vertex.transmitted_ior);
		hash = _fnv_u32(hash, vertex.flags);
	}
	return _fnv_color(hash, p_record.throughput);
}

} // namespace RendererPathTracing
