/**************************************************************************/
/*  transport_culling.h                                                   */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/templates/vector.h"

namespace RendererPathTracing {

// Conservative CPU-side selection for the ray-transport scene. Raster culling
// remains authoritative for primary visibility; this only bounds secondary rays.
enum TransportCullingState {
	TRANSPORT_CULLING_DISABLED,
	TRANSPORT_CULLING_BOUNDED,
	TRANSPORT_CULLING_FAIL_OPEN,
};

enum TransportCullingReason {
	TRANSPORT_CULLING_REASON_DISABLED,
	TRANSPORT_CULLING_REASON_ACTIVE,
	TRANSPORT_CULLING_REASON_INVALID_DISTANCE,
	TRANSPORT_CULLING_REASON_EMPTY_PRIMARY,
	TRANSPORT_CULLING_REASON_INVALID_BOUNDS,
};

struct TransportGeometryCandidate {
	uint64_t stable_id = 0;
	AABB world_aabb;
	bool primary = false;
};

struct TransportLightCandidate {
	uint64_t stable_id = 0;
	AABB influence_aabb;
	bool directional = false;
};

struct TransportCullingInput {
	bool enabled = true;
	float max_distance = 64.0f;
	Vector<TransportGeometryCandidate> geometry;
	Vector<TransportLightCandidate> lights;
};

struct TransportCullingResult {
	TransportCullingState state = TRANSPORT_CULLING_DISABLED;
	TransportCullingReason reason = TRANSPORT_CULLING_REASON_DISABLED;
	Vector<uint64_t> geometry_ids;
	Vector<uint64_t> light_ids;
	uint32_t primary_geometry_count = 0;
	uint32_t eligible_geometry_count = 0;
	uint32_t eligible_light_count = 0;
	float max_distance = 0.0f;
};

TransportCullingResult transport_cull(const TransportCullingInput &p_input);
const char *transport_culling_state_name(TransportCullingState p_state);
const char *transport_culling_reason_name(TransportCullingReason p_reason);

} // namespace RendererPathTracing
