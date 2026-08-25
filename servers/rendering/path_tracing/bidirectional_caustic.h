/**************************************************************************/
/*  bidirectional_caustic.h                                               */
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

#include <cstdint>

namespace RendererPathTracing {

// This is a deliberately narrow, backend-neutral reference ABI for the
// singular path family source -> strictly-delta events -> current diffuse
// receiver. It is not a general BDPT, VCM, photon splatting, or manifold
// implementation. Camera, pixel, cached visibility, and cached contribution
// are intentionally absent: none can authorize reuse after a camera move.
static constexpr uint32_t BIDIRECTIONAL_CAUSTIC_ABI_VERSION = 1;
static constexpr uint32_t BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES = 4;

enum BidirectionalCausticSourceFlags : uint32_t {
	BIDIRECTIONAL_CAUSTIC_SOURCE_VALID = 1U << 0,
	BIDIRECTIONAL_CAUSTIC_SOURCE_COMPACT_HIGH_ENERGY = 1U << 1,
	BIDIRECTIONAL_CAUSTIC_SOURCE_ENVIRONMENT = 1U << 2,
};

enum BidirectionalCausticDeltaFlags : uint32_t {
	BIDIRECTIONAL_CAUSTIC_DELTA_REFLECTION = 1U << 0,
	BIDIRECTIONAL_CAUSTIC_DELTA_REFRACTION = 1U << 1,
	// These flags are explicit rejection evidence. This phase only transports
	// perfect delta events; a backend must not silently approximate these.
	BIDIRECTIONAL_CAUSTIC_DELTA_UNSUPPORTED_MATERIAL = 1U << 2,
	BIDIRECTIONAL_CAUSTIC_DELTA_ROUGH_OR_GLOSSY = 1U << 3,
	BIDIRECTIONAL_CAUSTIC_DELTA_ALPHA_TESTED = 1U << 4,
	BIDIRECTIONAL_CAUSTIC_DELTA_DYNAMIC_GEOMETRY = 1U << 5,
	BIDIRECTIONAL_CAUSTIC_DELTA_DISOCCLUDED = 1U << 6,
};

enum BidirectionalCausticReceiverFlags : uint32_t {
	BIDIRECTIONAL_CAUSTIC_RECEIVER_VALID = 1U << 0,
	BIDIRECTIONAL_CAUSTIC_RECEIVER_DIFFUSE_OR_ROUGH = 1U << 1,
	BIDIRECTIONAL_CAUSTIC_RECEIVER_DYNAMIC_GEOMETRY = 1U << 2,
	BIDIRECTIONAL_CAUSTIC_RECEIVER_DISOCCLUDED = 1U << 3,
	BIDIRECTIONAL_CAUSTIC_RECEIVER_ALPHA_TESTED = 1U << 4,
};

struct BidirectionalCausticRevisions {
	uint64_t geometry = 0;
	uint64_t material = 0;
	uint64_t residency = 0;
};

// A compact sampled emitter identity. `selection_probability_mass` is a
// discrete probability, never a solid-angle or area PDF.
struct BidirectionalCausticSource {
	uint64_t source_id = 0;
	uint64_t sample_id = 0;
	uint64_t visibility_mask = 0;
	BidirectionalCausticRevisions revisions;
	Vector3 world_position;
	Vector3 outgoing_direction;
	Color radiance;
	float selection_probability_mass = 0.0f;
	uint32_t flags = 0;
};

// `forward_probability_mass` and `reverse_probability_mass` are discrete
// delta-event probabilities. They must not be treated as continuous PDFs.
struct BidirectionalCausticDeltaVertex {
	uint64_t geometry_instance_id = 0;
	uint64_t material_id = 0;
	uint32_t surface_id = 0;
	uint32_t primitive_id = 0;
	uint64_t visibility_mask = 0;
	BidirectionalCausticRevisions revisions;
	Vector3 world_position;
	Vector3 geometric_normal;
	float forward_probability_mass = 0.0f;
	float reverse_probability_mass = 0.0f;
	float incident_ior = 0.0f;
	float transmitted_ior = 0.0f;
	uint32_t flags = 0;
};

struct BidirectionalCausticPathRecord {
	uint32_t abi_version = 0;
	uint32_t delta_vertex_count = 0;
	uint64_t capture_frame = 0;
	uint32_t age = 0;
	uint64_t lighting_revision = 0;
	uint64_t environment_revision = 0;
	uint64_t residency_revision = 0;
	BidirectionalCausticSource source;
	BidirectionalCausticDeltaVertex delta_vertices[BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES];
	Color throughput;
};

// Current receiver data is deliberately supplied separately. A cached record
// cannot claim a target, material evaluation, or visibility result on its own.
struct BidirectionalCausticCurrentReceiver {
	uint64_t geometry_instance_id = 0;
	uint64_t material_id = 0;
	uint32_t surface_id = 0;
	uint32_t primitive_id = 0;
	uint64_t visibility_mask = 0;
	BidirectionalCausticRevisions revisions;
	Vector3 world_position;
	Vector3 geometric_normal;
	Color current_bsdf_value;
	uint32_t flags = 0;
};

struct BidirectionalCausticCurrentState {
	uint64_t current_frame = 0;
	uint64_t lighting_revision = 0;
	uint64_t environment_revision = 0;
	uint64_t residency_revision = 0;
	BidirectionalCausticRevisions source_revisions;
	BidirectionalCausticRevisions delta_revisions[BIDIRECTIONAL_CAUSTIC_MAX_DELTA_VERTICES];
	bool current_connection_available = false;
	bool current_visibility_evaluated = false;
	bool current_visibility_unoccluded = false;
};

struct BidirectionalCausticValidationThresholds {
	uint32_t maximum_age = 0;
	float maximum_receiver_distance = 0.0f;
};

enum BidirectionalCausticRejectReason : uint32_t {
	BIDIRECTIONAL_CAUSTIC_REJECT_NONE = 0,
	BIDIRECTIONAL_CAUSTIC_REJECT_ABI = 1U << 0,
	BIDIRECTIONAL_CAUSTIC_REJECT_INACTIVE_SOURCE = 1U << 1,
	BIDIRECTIONAL_CAUSTIC_REJECT_DELTA_CHAIN = 1U << 2,
	BIDIRECTIONAL_CAUSTIC_REJECT_RECEIVER = 1U << 3,
	BIDIRECTIONAL_CAUSTIC_REJECT_CONNECTION = 1U << 4,
	BIDIRECTIONAL_CAUSTIC_REJECT_VISIBILITY = 1U << 5,
	BIDIRECTIONAL_CAUSTIC_REJECT_IDENTITY = 1U << 6,
	BIDIRECTIONAL_CAUSTIC_REJECT_REVISION = 1U << 7,
	BIDIRECTIONAL_CAUSTIC_REJECT_MASK = 1U << 8,
	BIDIRECTIONAL_CAUSTIC_REJECT_DYNAMIC_OR_DISOCCLUDED = 1U << 9,
	BIDIRECTIONAL_CAUSTIC_REJECT_AGE = 1U << 10,
	BIDIRECTIONAL_CAUSTIC_REJECT_DISTANCE = 1U << 11,
	BIDIRECTIONAL_CAUSTIC_REJECT_NONFINITE = 1U << 12,
	BIDIRECTIONAL_CAUSTIC_REJECT_DENSITY = 1U << 13,
};

struct BidirectionalCausticValidationResult {
	uint32_t rejection_reasons = BIDIRECTIONAL_CAUSTIC_REJECT_NONE;
	bool requires_current_receiver_evaluation = true;
	bool requires_current_visibility = true;
	bool work_required = false;
	bool contribution_permitted = false;
};

struct BidirectionalCausticContribution {
	Color value;
	float geometry_term = 0.0f;
	float source_path_probability_mass = 0.0f;
	float competing_continuous_density = 0.0f;
	float mis_weight = 0.0f;
	bool valid = false;
};

// Geometry-only reference for the first active backend slice. A sampled
// finite emitter point is reflected through the plane; the receiver-to-virtual
// source line must land strictly inside this triangle. This avoids treating a
// finite mirror as an infinite reflection plane.
struct BidirectionalCausticPlanarMirrorTriangle {
	Vector3 p0;
	Vector3 p1;
	Vector3 p2;
	Vector3 geometric_normal;
	float selection_probability_mass = 0.0f;
};

struct BidirectionalCausticPlanarVirtualConnection {
	Vector3 mirror_position;
	Vector3 virtual_source_position;
	Vector3 virtual_source_normal;
	Vector3 barycentric;
	float receiver_to_virtual_distance_squared = 0.0f;
	float source_area_pdf = 0.0f;
	float mirror_selection_probability_mass = 0.0f;
	float proposal_area_pdf = 0.0f;
	bool valid = false;
};

bool bidirectional_caustic_is_active(const BidirectionalCausticPathRecord &p_record);
BidirectionalCausticValidationResult validate_bidirectional_caustic_path(const BidirectionalCausticPathRecord &p_record, const BidirectionalCausticCurrentReceiver &p_receiver, const BidirectionalCausticCurrentState &p_current, const BidirectionalCausticValidationThresholds &p_thresholds);

// Measure conversions for non-delta endpoint proposals. A delta vertex is
// singular, so its competing continuous density is always exactly zero.
bool bidirectional_caustic_solid_angle_to_area_pdf(float p_solid_angle_pdf, const Vector3 &p_from, const Vector3 &p_to, const Vector3 &p_to_normal, float &r_area_pdf);
bool bidirectional_caustic_area_to_solid_angle_pdf(float p_area_pdf, const Vector3 &p_from, const Vector3 &p_to, const Vector3 &p_to_normal, float &r_solid_angle_pdf);
bool bidirectional_caustic_delta_competing_continuous_density(const BidirectionalCausticDeltaVertex &p_vertex, float &r_density);
bool bidirectional_caustic_source_path_probability_mass(const BidirectionalCausticPathRecord &p_record, float &r_probability_mass);

// Produces a current, visibility-authorized connection estimate. The receiver
// BSDF and geometry factor are evaluated here; cached values are never used.
// Delta chains are singular relative to ordinary continuous competitors, so
// their competing continuous density and balance-heuristic denominator term
// are zero and the MIS weight is one.
BidirectionalCausticContribution evaluate_bidirectional_caustic_connection(const BidirectionalCausticPathRecord &p_record, const BidirectionalCausticCurrentReceiver &p_receiver, const BidirectionalCausticCurrentState &p_current, const BidirectionalCausticValidationThresholds &p_thresholds);

// Builds the exact one-delta virtual-source connection and its area-measure
// proposal density. It performs no visibility query: callers must trace both
// receiver->mirror and mirror->source segments at the current frame.
bool bidirectional_caustic_build_planar_virtual_connection(const BidirectionalCausticPlanarMirrorTriangle &p_mirror, const Vector3 &p_receiver_position, const Vector3 &p_receiver_normal, const Vector3 &p_source_position, const Vector3 &p_source_normal, float p_source_area_pdf, BidirectionalCausticPlanarVirtualConnection &r_connection);

uint64_t bidirectional_caustic_identity_checksum(const BidirectionalCausticPathRecord &p_record);
uint64_t bidirectional_caustic_replay_checksum(const BidirectionalCausticPathRecord &p_record);

} // namespace RendererPathTracing
