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

// Product-neutral eligibility gate for replacing a raster source only in the
// secondary-ray scene. The producer's opaque-equivalence bit is deliberately
// explicit: a proxy is never inferred from ordinary raster LOD state.
struct RayProxyRelationInput {
	bool relation_present = false;
	bool explicit_opaque_equivalence = false;
	bool source_static = false;
	bool proxy_static = false;
	bool proxy_hidden = false;
	bool distinct_geometry = false;
	bool same_transform = false;
	bool same_effective_materials = false;
	bool same_surface_topology = false;
	bool containment_certified = false;
	bool source_bounds_contained = false;
	bool surface_mapping_valid = false;
	bool opaque = false;
	bool outside_near_field = false;
	bool acyclic = false;
};

enum RayProxyRelationReason {
	RAY_PROXY_RELATION_NONE,
	RAY_PROXY_RELATION_VALID,
	RAY_PROXY_RELATION_UNPROVEN,
	RAY_PROXY_RELATION_DYNAMIC,
	RAY_PROXY_RELATION_VISIBLE_PROXY,
	RAY_PROXY_RELATION_SAME_GEOMETRY,
	RAY_PROXY_RELATION_TRANSFORM_MISMATCH,
	RAY_PROXY_RELATION_MATERIAL_MISMATCH,
	RAY_PROXY_RELATION_TOPOLOGY_MISMATCH,
	RAY_PROXY_RELATION_INVALID_CONTAINMENT,
	RAY_PROXY_RELATION_INVALID_SURFACE_MAPPING,
	RAY_PROXY_RELATION_NON_OPAQUE,
	RAY_PROXY_RELATION_NEAR_FIELD,
	RAY_PROXY_RELATION_CYCLIC,
	RAY_PROXY_RELATION_MISSING_RENDERER_GEOMETRY,
	RAY_PROXY_RELATION_MAX,
};

struct RayProxyRelationResult {
	bool substitute = false;
	RayProxyRelationReason reason = RAY_PROXY_RELATION_NONE;
};

RayProxyRelationResult validate_ray_proxy_relation(const RayProxyRelationInput &p_input);
const char *ray_proxy_relation_reason_name(RayProxyRelationReason p_reason);

struct TransportCullingResult {
	TransportCullingState state = TRANSPORT_CULLING_DISABLED;
	TransportCullingReason reason = TRANSPORT_CULLING_REASON_DISABLED;
	Vector<uint64_t> geometry_ids;
	Vector<uint64_t> light_ids;
	uint32_t primary_geometry_count = 0;
	uint32_t eligible_geometry_count = 0;
	uint32_t eligible_light_count = 0;
	// Secondary-ray proxy accounting. These are source-instance counts and do
	// not alter the transport-culling selection contract.
	uint32_t ray_proxy_source_count = 0;
	uint32_t ray_proxy_substituted_count = 0;
	uint32_t ray_proxy_fail_open_count = 0;
	uint32_t ray_proxy_duplicate_count = 0;
	uint32_t ray_proxy_rejection_counts[RAY_PROXY_RELATION_MAX] = {};
	float max_distance = 0.0f;
};

TransportCullingResult transport_cull(const TransportCullingInput &p_input);
const char *transport_culling_state_name(TransportCullingState p_state);
const char *transport_culling_reason_name(TransportCullingReason p_reason);

} // namespace RendererPathTracing
