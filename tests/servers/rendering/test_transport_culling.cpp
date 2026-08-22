/**************************************************************************/
/*  test_transport_culling.cpp                                            */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_transport_culling)

#include "servers/rendering/path_tracing/transport_culling.h"

namespace TestTransportCulling {

using namespace RendererPathTracing;

static TransportGeometryCandidate geometry(uint64_t p_id, const Vector3 &p_position, bool p_primary = false) {
	TransportGeometryCandidate candidate;
	candidate.stable_id = p_id;
	candidate.world_aabb = AABB(p_position, Vector3(1, 1, 1));
	candidate.primary = p_primary;
	return candidate;
}

TEST_CASE("[PathTracing][TransportCulling] disabled and empty-primary fail open") {
	TransportCullingInput input;
	input.enabled = false;
	input.geometry.push_back(geometry(7, Vector3(), true));
	input.geometry.push_back(geometry(3, Vector3(200, 0, 0)));
	TransportCullingResult result = transport_cull(input);
	CHECK_EQ(result.state, TRANSPORT_CULLING_DISABLED);
	CHECK_EQ(result.geometry_ids.size(), 2);
	input.enabled = true;
	input.geometry.clear();
	input.geometry.push_back(geometry(3, Vector3(200, 0, 0)));
	result = transport_cull(input);
	CHECK_EQ(result.state, TRANSPORT_CULLING_FAIL_OPEN);
	CHECK_EQ(result.reason, TRANSPORT_CULLING_REASON_EMPTY_PRIMARY);
}

TEST_CASE("[PathTracing][TransportCulling] preserves near hidden transport and excludes distant candidates") {
	TransportCullingInput input;
	input.max_distance = 10.0f;
	input.geometry.push_back(geometry(1, Vector3(), true));
	input.geometry.push_back(geometry(2, Vector3(22, 0, 0)));
	input.geometry.push_back(geometry(3, Vector3(40, 0, 0)));
	const TransportCullingResult result = transport_cull(input);
	CHECK_EQ(result.state, TRANSPORT_CULLING_BOUNDED);
	CHECK_EQ(result.geometry_ids.size(), 2);
	CHECK_EQ(result.geometry_ids[0], 1);
	CHECK_EQ(result.geometry_ids[1], 2);
}

TEST_CASE("[PathTracing][TransportCulling] exact conservative boundary and input permutation are stable") {
	TransportCullingInput first;
	first.max_distance = 10.0f;
	first.geometry.push_back(geometry(10, Vector3(), true));
	first.geometry.push_back(geometry(20, Vector3(22.999f, 0, 0))); // Just inside the 2.2D envelope.
	first.geometry.push_back(geometry(30, Vector3(23.001f, 0, 0))); // Just outside it.
	TransportCullingInput second = first;
	second.geometry.reverse();
	const TransportCullingResult a = transport_cull(first);
	const TransportCullingResult b = transport_cull(second);
	CHECK_EQ(a.geometry_ids.size(), 2);
	CHECK_EQ(a.geometry_ids, b.geometry_ids);
}

TEST_CASE("[PathTracing][TransportCulling] uses deterministic buckets and conservative light influence") {
	TransportCullingInput input;
	input.max_distance = 10.0f;
	input.geometry.push_back(geometry(8, Vector3(15, 0, 0), true));
	input.geometry.push_back(geometry(2, Vector3(), true));
	input.geometry.push_back(geometry(3, Vector3(38, 0, 0)));
	TransportLightCandidate directional;
	directional.stable_id = 9;
	directional.directional = true;
	input.lights.push_back(directional);
	TransportLightCandidate near;
	near.stable_id = 4;
	near.influence_aabb = AABB(Vector3(19, 0, 0), Vector3(1, 1, 1));
	input.lights.push_back(near);
	TransportLightCandidate far;
	far.stable_id = 5;
	far.influence_aabb = AABB(Vector3(100, 0, 0), Vector3(1, 1, 1));
	input.lights.push_back(far);
	const TransportCullingResult result = transport_cull(input);
	CHECK_EQ(result.geometry_ids.size(), 2); // Separate primary buckets do not bridge the 38 m candidate.
	CHECK_EQ(result.light_ids.size(), 2);
	CHECK_EQ(result.light_ids[0], 4);
	CHECK_EQ(result.light_ids[1], 9);
}

TEST_CASE("[PathTracing][TransportCulling] invalid bounds fail open") {
	TransportCullingInput input;
	input.geometry.push_back(geometry(1, Vector3(), true));
	TransportGeometryCandidate invalid = geometry(2, Vector3());
	invalid.world_aabb.size.x = Math::INF;
	input.geometry.push_back(invalid);
	const TransportCullingResult result = transport_cull(input);
	CHECK_EQ(result.state, TRANSPORT_CULLING_FAIL_OPEN);
	CHECK_EQ(result.reason, TRANSPORT_CULLING_REASON_INVALID_BOUNDS);
	CHECK_EQ(result.geometry_ids.size(), 2);
}

} // namespace TestTransportCulling
