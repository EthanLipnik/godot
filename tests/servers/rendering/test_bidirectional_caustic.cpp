/**************************************************************************/
/*  test_bidirectional_caustic.cpp                                        */
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

TEST_FORCE_LINK(test_bidirectional_caustic)

#include "servers/rendering/path_tracing/bidirectional_caustic.h"

#include <limits>

namespace TestBidirectionalCaustic {

using namespace RendererPathTracing;

static BidirectionalCausticPathRecord make_path() {
	BidirectionalCausticPathRecord record;
	record.abi_version = BIDIRECTIONAL_CAUSTIC_ABI_VERSION;
	record.delta_vertex_count = 1;
	record.capture_frame = 100;
	record.age = 1;
	record.lighting_revision = 101;
	record.environment_revision = 102;
	record.residency_revision = 103;
	record.source.source_id = 10;
	record.source.sample_id = 11;
	record.source.visibility_mask = 0x7;
	record.source.revisions = { 201, 202, 203 };
	record.source.world_position = Vector3(0.0f, 4.0f, 0.0f);
	record.source.outgoing_direction = Vector3(0.0f, -1.0f, 0.0f);
	record.source.radiance = Color(8.0f, 4.0f, 2.0f, 1.0f);
	record.source.selection_probability_mass = 0.5f;
	record.source.flags = BIDIRECTIONAL_CAUSTIC_SOURCE_VALID | BIDIRECTIONAL_CAUSTIC_SOURCE_COMPACT_HIGH_ENERGY;
	BidirectionalCausticDeltaVertex &vertex = record.delta_vertices[0];
	vertex.geometry_instance_id = 20;
	vertex.material_id = 21;
	vertex.surface_id = 2;
	vertex.primitive_id = 3;
	vertex.visibility_mask = 0x3;
	vertex.revisions = { 301, 302, 303 };
	vertex.world_position = Vector3(0.0f, 1.0f, 0.0f);
	vertex.geometric_normal = Vector3(0.0f, 1.0f, 0.0f);
	vertex.forward_probability_mass = 0.25f;
	vertex.reverse_probability_mass = 0.25f;
	vertex.incident_ior = 1.0f;
	vertex.transmitted_ior = 1.0f;
	vertex.flags = BIDIRECTIONAL_CAUSTIC_DELTA_REFLECTION;
	record.throughput = Color(0.5f, 0.5f, 0.5f, 1.0f);
	return record;
}

static BidirectionalCausticCurrentReceiver make_receiver() {
	BidirectionalCausticCurrentReceiver receiver;
	receiver.geometry_instance_id = 30;
	receiver.material_id = 31;
	receiver.surface_id = 4;
	receiver.primitive_id = 5;
	receiver.visibility_mask = 0x3;
	receiver.revisions = { 401, 402, 403 };
	receiver.world_position = Vector3(0.0f, 3.0f, 0.0f);
	receiver.geometric_normal = Vector3(0.0f, -1.0f, 0.0f);
	receiver.current_bsdf_value = Color(0.25f, 0.5f, 1.0f, 1.0f);
	receiver.flags = BIDIRECTIONAL_CAUSTIC_RECEIVER_VALID | BIDIRECTIONAL_CAUSTIC_RECEIVER_DIFFUSE_OR_ROUGH;
	return receiver;
}

static BidirectionalCausticCurrentState make_current() {
	const BidirectionalCausticPathRecord record = make_path();
	BidirectionalCausticCurrentState current;
	current.current_frame = 101;
	current.lighting_revision = record.lighting_revision;
	current.environment_revision = record.environment_revision;
	current.residency_revision = record.residency_revision;
	current.source_revisions = record.source.revisions;
	current.delta_revisions[0] = record.delta_vertices[0].revisions;
	current.current_connection_available = true;
	current.current_visibility_evaluated = true;
	current.current_visibility_unoccluded = true;
	return current;
}

static BidirectionalCausticValidationThresholds make_thresholds() {
	BidirectionalCausticValidationThresholds thresholds;
	thresholds.maximum_age = 4;
	thresholds.maximum_receiver_distance = 3.0f;
	return thresholds;
}

static void check_reject(const BidirectionalCausticPathRecord &p_record, const BidirectionalCausticCurrentReceiver &p_receiver, const BidirectionalCausticCurrentState &p_current, uint32_t p_reason) {
	const BidirectionalCausticValidationResult result = validate_bidirectional_caustic_path(p_record, p_receiver, p_current, make_thresholds());
	CHECK_FALSE(result.work_required);
	CHECK_FALSE(result.contribution_permitted);
	CHECK((result.rejection_reasons & p_reason) != 0);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] inactive ordinary scenes require no work") {
	BidirectionalCausticPathRecord record;
	const BidirectionalCausticValidationResult result = validate_bidirectional_caustic_path(record, make_receiver(), make_current(), make_thresholds());
	CHECK_FALSE(bidirectional_caustic_is_active(record));
	CHECK_FALSE(result.work_required);
	CHECK_FALSE(result.contribution_permitted);
	CHECK((result.rejection_reasons & BIDIRECTIONAL_CAUSTIC_REJECT_INACTIVE_SOURCE) != 0);
	BidirectionalCausticPathRecord source_off = make_path();
	source_off.source.flags &= ~BIDIRECTIONAL_CAUSTIC_SOURCE_COMPACT_HIGH_ENERGY;
	check_reject(source_off, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_INACTIVE_SOURCE);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] a mirror chain has a current connection estimate") {
	const BidirectionalCausticPathRecord record = make_path();
	const BidirectionalCausticValidationResult validation = validate_bidirectional_caustic_path(record, make_receiver(), make_current(), make_thresholds());
	CHECK_EQ(validation.rejection_reasons, BIDIRECTIONAL_CAUSTIC_REJECT_NONE);
	CHECK(validation.work_required);
	CHECK(validation.contribution_permitted);
	const BidirectionalCausticContribution contribution = evaluate_bidirectional_caustic_connection(record, make_receiver(), make_current(), make_thresholds());
	CHECK(contribution.valid);
	CHECK_EQ(contribution.geometry_term, 0.25f);
	CHECK_EQ(contribution.source_path_probability_mass, 0.125f);
	CHECK_EQ(contribution.competing_continuous_density, 0.0f);
	CHECK_EQ(contribution.mis_weight, 1.0f);
	CHECK_EQ(contribution.value.r, 2.0f);
	CHECK_EQ(contribution.value.g, 2.0f);
	CHECK_EQ(contribution.value.b, 2.0f);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] refractive chains retain their IOR transition") {
	BidirectionalCausticPathRecord record = make_path();
	record.delta_vertices[0].flags = BIDIRECTIONAL_CAUSTIC_DELTA_REFRACTION;
	record.delta_vertices[0].transmitted_ior = 1.5f;
	CHECK(validate_bidirectional_caustic_path(record, make_receiver(), make_current(), make_thresholds()).contribution_permitted);
	record.delta_vertices[0].transmitted_ior = 1.0f;
	check_reject(record, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_DELTA_CHAIN);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] current visibility and receiver target are mandatory") {
	const BidirectionalCausticPathRecord record = make_path();
	BidirectionalCausticCurrentState current = make_current();
	current.current_visibility_evaluated = false;
	check_reject(record, make_receiver(), current, BIDIRECTIONAL_CAUSTIC_REJECT_VISIBILITY);
	current = make_current();
	current.current_visibility_unoccluded = false;
	check_reject(record, make_receiver(), current, BIDIRECTIONAL_CAUSTIC_REJECT_VISIBILITY);
	current = make_current();
	current.current_connection_available = false;
	check_reject(record, make_receiver(), current, BIDIRECTIONAL_CAUSTIC_REJECT_CONNECTION);
	BidirectionalCausticCurrentReceiver receiver = make_receiver();
	receiver.flags = BIDIRECTIONAL_CAUSTIC_RECEIVER_VALID;
	check_reject(record, receiver, make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_RECEIVER);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] unsupported rough alpha dynamic and disoccluded paths fail closed") {
	const BidirectionalCausticPathRecord baseline = make_path();
	for (const uint32_t flag : { BIDIRECTIONAL_CAUSTIC_DELTA_UNSUPPORTED_MATERIAL, BIDIRECTIONAL_CAUSTIC_DELTA_ROUGH_OR_GLOSSY, BIDIRECTIONAL_CAUSTIC_DELTA_ALPHA_TESTED, BIDIRECTIONAL_CAUSTIC_DELTA_DYNAMIC_GEOMETRY, BIDIRECTIONAL_CAUSTIC_DELTA_DISOCCLUDED }) {
		BidirectionalCausticPathRecord record = baseline;
		record.delta_vertices[0].flags |= flag;
		check_reject(record, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_DELTA_CHAIN);
	}
	BidirectionalCausticCurrentReceiver receiver = make_receiver();
	receiver.flags |= BIDIRECTIONAL_CAUSTIC_RECEIVER_ALPHA_TESTED;
	check_reject(baseline, receiver, make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_DYNAMIC_OR_DISOCCLUDED);
	receiver = make_receiver();
	receiver.flags |= BIDIRECTIONAL_CAUSTIC_RECEIVER_DYNAMIC_GEOMETRY | BIDIRECTIONAL_CAUSTIC_RECEIVER_DISOCCLUDED;
	check_reject(baseline, receiver, make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_DYNAMIC_OR_DISOCCLUDED);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] revisions masks dynamic age and bounds invalidate") {
	const BidirectionalCausticPathRecord baseline = make_path();
	BidirectionalCausticCurrentState current = make_current();
	current.delta_revisions[0].material++;
	check_reject(baseline, make_receiver(), current, BIDIRECTIONAL_CAUSTIC_REJECT_REVISION);
	current = make_current();
	current.source_revisions.residency++;
	check_reject(baseline, make_receiver(), current, BIDIRECTIONAL_CAUSTIC_REJECT_REVISION);
	BidirectionalCausticPathRecord record = baseline;
	record.delta_vertices[0].visibility_mask = 0x8;
	check_reject(record, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_MASK);
	record = baseline;
	record.capture_frame = 90;
	check_reject(record, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_AGE);
	BidirectionalCausticCurrentReceiver receiver = make_receiver();
	receiver.world_position.y = 5.0f;
	check_reject(baseline, receiver, make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_DISTANCE);
}

TEST_CASE("[PathTracing][BidirectionalCaustic] PDF Jacobians and delta MIS measure are explicit") {
	float area_pdf = 0.0f;
	CHECK(bidirectional_caustic_solid_angle_to_area_pdf(0.5f, Vector3(), Vector3(0.0f, 2.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), area_pdf));
	CHECK_EQ(area_pdf, 0.125f);
	float solid_angle_pdf = 0.0f;
	CHECK(bidirectional_caustic_area_to_solid_angle_pdf(area_pdf, Vector3(), Vector3(0.0f, 2.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), solid_angle_pdf));
	CHECK_EQ(solid_angle_pdf, 0.5f);
	float density = 123.0f;
	CHECK(bidirectional_caustic_delta_competing_continuous_density(make_path().delta_vertices[0], density));
	CHECK_EQ(density, 0.0f);
	CHECK_FALSE(bidirectional_caustic_solid_angle_to_area_pdf(0.0f, Vector3(), Vector3(), Vector3(0.0f, 1.0f, 0.0f), area_pdf));
}

TEST_CASE("[PathTracing][BidirectionalCaustic] planar virtual source has strict finite-mirror measure") {
	BidirectionalCausticPlanarMirrorTriangle mirror;
	mirror.p0 = Vector3(0.0f, -2.0f, -2.0f);
	mirror.p1 = Vector3(0.0f, 2.0f, -2.0f);
	mirror.p2 = Vector3(0.0f, 0.0f, 2.0f);
	mirror.geometric_normal = Vector3(1.0f, 0.0f, 0.0f);
	mirror.selection_probability_mass = 0.5f;
	BidirectionalCausticPlanarVirtualConnection connection;
	CHECK(bidirectional_caustic_build_planar_virtual_connection(mirror, Vector3(1.0f, 0.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), 0.25f, connection));
	CHECK(connection.valid);
	CHECK(connection.mirror_position.is_equal_approx(Vector3(0.0f, 0.5f, 0.0f)));
	CHECK(connection.virtual_source_position.is_equal_approx(Vector3(-1.0f, 1.0f, 0.0f)));
	CHECK_EQ(connection.proposal_area_pdf, 0.125f);
	CHECK(connection.barycentric.x > 0.0f);
	CHECK(connection.barycentric.y > 0.0f);
	CHECK(connection.barycentric.z > 0.0f);
	// Off-triangle, grazing, and invalid measure fail before a backend could
	// submit visibility work.
	mirror.p0 = Vector3(0.0f, 10.0f, -2.0f);
	mirror.p1 = Vector3(0.0f, 14.0f, -2.0f);
	mirror.p2 = Vector3(0.0f, 12.0f, 2.0f);
	CHECK_FALSE(bidirectional_caustic_build_planar_virtual_connection(mirror, Vector3(1.0f, 0.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), 0.25f, connection));
	mirror.p0 = Vector3(0.0f, -2.0f, -2.0f);
	mirror.p1 = Vector3(0.0f, 2.0f, -2.0f);
	mirror.p2 = Vector3(0.0f, 0.0f, 2.0f);
	CHECK_FALSE(bidirectional_caustic_build_planar_virtual_connection(mirror, Vector3(1.0f, 0.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), Vector3(-1.0f, 1.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), 0.25f, connection));
	CHECK_FALSE(bidirectional_caustic_build_planar_virtual_connection(mirror, Vector3(1.0f, 0.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), 0.0f, connection));
}

TEST_CASE("[PathTracing][BidirectionalCaustic] replay is deterministic and nonfinite payloads fail") {
	const BidirectionalCausticPathRecord baseline = make_path();
	CHECK_EQ(bidirectional_caustic_identity_checksum(baseline), bidirectional_caustic_identity_checksum(baseline));
	CHECK_EQ(bidirectional_caustic_replay_checksum(baseline), bidirectional_caustic_replay_checksum(baseline));
	BidirectionalCausticPathRecord changed = baseline;
	changed.throughput.r += 1.0f;
	CHECK_EQ(bidirectional_caustic_identity_checksum(baseline), bidirectional_caustic_identity_checksum(changed));
	CHECK_NE(bidirectional_caustic_replay_checksum(baseline), bidirectional_caustic_replay_checksum(changed));
	changed = baseline;
	changed.delta_vertices[0].forward_probability_mass = std::numeric_limits<float>::quiet_NaN();
	check_reject(changed, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_NONFINITE);
	check_reject(changed, make_receiver(), make_current(), BIDIRECTIONAL_CAUSTIC_REJECT_DENSITY);
}

} // namespace TestBidirectionalCaustic
