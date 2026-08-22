/**************************************************************************/
/*  transport_culling.cpp                                                 */
/**************************************************************************/

#include "transport_culling.h"

#include "core/math/math_funcs.h"

namespace RendererPathTracing {

static bool _finite_aabb(const AABB &p_aabb) {
	return Math::is_finite(p_aabb.position.x) && Math::is_finite(p_aabb.position.y) && Math::is_finite(p_aabb.position.z) &&
			Math::is_finite(p_aabb.size.x) && Math::is_finite(p_aabb.size.y) && Math::is_finite(p_aabb.size.z) &&
			p_aabb.size.x >= 0.0f && p_aabb.size.y >= 0.0f && p_aabb.size.z >= 0.0f;
}

static AABB _grown(const AABB &p_aabb, float p_distance) {
	const Vector3 grow = Vector3(p_distance, p_distance, p_distance) * 1.1f;
	return AABB(p_aabb.position - grow, p_aabb.size + grow * 2.0f);
}

struct _PrimaryBucket {
	int64_t x = 0;
	int64_t y = 0;
	int64_t z = 0;
	uint64_t stable_id = 0;
	AABB aabb;
};

struct _BucketLess {
	bool operator()(const _PrimaryBucket &a, const _PrimaryBucket &b) const {
		if (a.x != b.x) return a.x < b.x;
		if (a.y != b.y) return a.y < b.y;
		if (a.z != b.z) return a.z < b.z;
		return a.stable_id < b.stable_id;
	}
};

struct _GeometryLess {
	bool operator()(const TransportGeometryCandidate &a, const TransportGeometryCandidate &b) const { return a.stable_id < b.stable_id; }
};

struct _LightLess {
	bool operator()(const TransportLightCandidate &a, const TransportLightCandidate &b) const { return a.stable_id < b.stable_id; }
};

TransportCullingResult transport_cull(const TransportCullingInput &p_input) {
	TransportCullingResult result;
	result.eligible_geometry_count = p_input.geometry.size();
	result.eligible_light_count = p_input.lights.size();
	result.max_distance = p_input.max_distance;

	Vector<TransportGeometryCandidate> geometry = p_input.geometry;
	Vector<TransportLightCandidate> lights = p_input.lights;
	geometry.sort_custom<_GeometryLess>();
	lights.sort_custom<_LightLess>();
	for (const TransportGeometryCandidate &candidate : geometry) {
		result.primary_geometry_count += candidate.primary;
	}
	for (const TransportGeometryCandidate &candidate : geometry) {
		if (!_finite_aabb(candidate.world_aabb)) {
			result.state = TRANSPORT_CULLING_FAIL_OPEN;
			result.reason = TRANSPORT_CULLING_REASON_INVALID_BOUNDS;
			goto fail_open;
		}
	}
	for (const TransportLightCandidate &candidate : lights) {
		if (!candidate.directional && !_finite_aabb(candidate.influence_aabb)) {
			result.state = TRANSPORT_CULLING_FAIL_OPEN;
			result.reason = TRANSPORT_CULLING_REASON_INVALID_BOUNDS;
			goto fail_open;
		}
	}
	if (!p_input.enabled) {
		result.state = TRANSPORT_CULLING_DISABLED;
		result.reason = TRANSPORT_CULLING_REASON_DISABLED;
		goto fail_open;
	}
	if (!Math::is_finite(p_input.max_distance) || p_input.max_distance <= 0.0f) {
		result.state = TRANSPORT_CULLING_FAIL_OPEN;
		result.reason = TRANSPORT_CULLING_REASON_INVALID_DISTANCE;
		goto fail_open;
	}
	if (result.primary_geometry_count == 0) {
		result.state = TRANSPORT_CULLING_FAIL_OPEN;
		result.reason = TRANSPORT_CULLING_REASON_EMPTY_PRIMARY;
		goto fail_open;
	}
	{
		Vector<_PrimaryBucket> buckets;
		for (const TransportGeometryCandidate &candidate : geometry) {
			if (!candidate.primary) continue;
			const Vector3 center = candidate.world_aabb.get_center();
			_PrimaryBucket bucket;
			bucket.x = int64_t(Math::floor(center.x / p_input.max_distance));
			bucket.y = int64_t(Math::floor(center.y / p_input.max_distance));
			bucket.z = int64_t(Math::floor(center.z / p_input.max_distance));
			bucket.stable_id = candidate.stable_id;
			bucket.aabb = candidate.world_aabb;
			buckets.push_back(bucket);
		}
		buckets.sort_custom<_BucketLess>();
		Vector<AABB> regions;
		Vector<_PrimaryBucket> region_keys;
		for (const _PrimaryBucket &bucket : buckets) {
			if (regions.is_empty() || bucket.x != region_keys[region_keys.size() - 1].x || bucket.y != region_keys[region_keys.size() - 1].y || bucket.z != region_keys[region_keys.size() - 1].z) {
				regions.push_back(bucket.aabb);
				region_keys.push_back(bucket);
			} else {
				regions.write[regions.size() - 1] = regions[regions.size() - 1].merge(bucket.aabb);
			}
		}
		for (const TransportGeometryCandidate &candidate : geometry) {
			bool selected = candidate.primary;
			for (const AABB &region : regions) {
				if (_grown(region, p_input.max_distance * 2.0f).intersects(candidate.world_aabb)) {
					selected = true;
					break;
				}
			}
			if (selected && (result.geometry_ids.is_empty() || result.geometry_ids[result.geometry_ids.size() - 1] != candidate.stable_id)) result.geometry_ids.push_back(candidate.stable_id);
		}
		for (const TransportLightCandidate &candidate : lights) {
			bool selected = candidate.directional;
			for (const AABB &region : regions) {
				if (_grown(region, p_input.max_distance).intersects(candidate.influence_aabb)) {
					selected = true;
					break;
				}
			}
			if (selected && (result.light_ids.is_empty() || result.light_ids[result.light_ids.size() - 1] != candidate.stable_id)) result.light_ids.push_back(candidate.stable_id);
		}
	}
	result.state = TRANSPORT_CULLING_BOUNDED;
	result.reason = TRANSPORT_CULLING_REASON_ACTIVE;
	return result;

fail_open:
	for (const TransportGeometryCandidate &candidate : geometry) {
		if (result.geometry_ids.is_empty() || result.geometry_ids[result.geometry_ids.size() - 1] != candidate.stable_id) result.geometry_ids.push_back(candidate.stable_id);
	}
	for (const TransportLightCandidate &candidate : lights) {
		if (result.light_ids.is_empty() || result.light_ids[result.light_ids.size() - 1] != candidate.stable_id) result.light_ids.push_back(candidate.stable_id);
	}
	return result;
}

const char *transport_culling_state_name(TransportCullingState p_state) {
	return p_state == TRANSPORT_CULLING_BOUNDED ? "bounded" : (p_state == TRANSPORT_CULLING_FAIL_OPEN ? "fail-open" : "disabled");
}

const char *transport_culling_reason_name(TransportCullingReason p_reason) {
	switch (p_reason) {
		case TRANSPORT_CULLING_REASON_ACTIVE: return "active";
		case TRANSPORT_CULLING_REASON_INVALID_DISTANCE: return "invalid-distance";
		case TRANSPORT_CULLING_REASON_EMPTY_PRIMARY: return "empty-primary";
		case TRANSPORT_CULLING_REASON_INVALID_BOUNDS: return "invalid-bounds";
		default: return "disabled";
	}
}

} // namespace RendererPathTracing
