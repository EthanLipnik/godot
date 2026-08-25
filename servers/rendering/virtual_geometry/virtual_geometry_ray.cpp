/**************************************************************************/
/*  virtual_geometry_ray.cpp                                              */
/**************************************************************************/

#include "virtual_geometry_ray.h"

namespace RendererVirtualGeometry {

Error VirtualGeometryRayHierarchy::set_manifest(const Manifest &p_manifest) {
	manifest = p_manifest;
	groups.clear();
	retirements.clear();
	diagnostics = {};
	for (const RayGroupDescriptor &descriptor : manifest.ray_groups) {
		ERR_FAIL_COND_V(descriptor.stable_id == 0 || groups.has(descriptor.stable_id), ERR_INVALID_DATA);
		RuntimeGroup runtime;
		runtime.descriptor = &descriptor;
		groups.insert(descriptor.stable_id, runtime);
	}
	return OK;
}

RayTransportTier VirtualGeometryRayHierarchy::select_desired_tier(const RayTransportSelectionInput &p_input) {
	if (p_input.role == RayTransportRole::REFERENCE || p_input.role == RayTransportRole::PRIMARY) {
		return RayTransportTier::TIER_NEAR;
	}
	const float distance = MAX(0.0f, p_input.distance);
	const float footprint = MAX(0.0f, p_input.ray_footprint);
	const float roughness = CLAMP(p_input.roughness, 0.0f, 1.0f);
	const float contribution = MAX(0.0f, p_input.expected_contribution);
	if (p_input.role == RayTransportRole::SHARP_REFLECTION) {
		return roughness <= 0.12f && footprint <= 0.02f && p_input.path_depth <= 1 && distance <= p_input.near_distance ? RayTransportTier::TIER_NEAR : (distance <= p_input.middle_distance ? RayTransportTier::TIER_MIDDLE : RayTransportTier::TIER_FAR);
	}
	if (p_input.role == RayTransportRole::DIRECT_VISIBILITY || p_input.role == RayTransportRole::EMISSIVE_VISIBILITY) {
		if (distance <= p_input.near_distance && contribution >= 0.25f && footprint <= 0.05f) return RayTransportTier::TIER_NEAR;
		return distance <= p_input.middle_distance && contribution >= 0.02f ? RayTransportTier::TIER_MIDDLE : RayTransportTier::TIER_FAR;
	}
	if (p_input.role == RayTransportRole::ROUGH_REFLECTION) {
		return distance <= p_input.middle_distance && roughness < 0.65f && footprint < 0.2f && p_input.path_depth <= 2 ? RayTransportTier::TIER_MIDDLE : RayTransportTier::TIER_FAR;
	}
	return distance <= p_input.near_distance && footprint < 0.08f && p_input.path_depth == 0 && contribution >= 0.1f ? RayTransportTier::TIER_MIDDLE : RayTransportTier::TIER_FAR;
}

const VirtualGeometryRayHierarchy::RuntimeGroup *VirtualGeometryRayHierarchy::_best_active(uint64_t p_region_id, RayTransportTier p_desired) const {
	const RuntimeGroup *best = nullptr;
	for (const KeyValue<uint64_t, RuntimeGroup> &entry : groups) {
		const RuntimeGroup &runtime = entry.value;
		if (!runtime.descriptor || runtime.descriptor->transport_region_id != p_region_id || runtime.active_version == 0 || uint32_t(runtime.descriptor->tier) > uint32_t(p_desired)) continue;
		if (!best || uint32_t(runtime.descriptor->tier) > uint32_t(best->descriptor->tier)) best = &runtime;
	}
	return best;
}

RayTransportSelection VirtualGeometryRayHierarchy::select(const RayTransportSelectionInput &p_input) const {
	RayTransportSelection result;
	const RayTransportTier desired_tier = select_desired_tier(p_input);
	HashSet<uint64_t> regions;
	for (const RayGroupDescriptor &descriptor : manifest.ray_groups) regions.insert(descriptor.transport_region_id);
	Vector<uint64_t> ordered_regions;
	for (uint64_t region : regions) ordered_regions.push_back(region);
	ordered_regions.sort();
	for (uint64_t region : ordered_regions) {
		RayTransportRegionSelection selection;
		selection.transport_region_id = region;
		selection.desired_tier = desired_tier;
		selection.off_screen_retained = p_input.off_screen_influence;
		for (const RayGroupDescriptor &descriptor : manifest.ray_groups) if (descriptor.transport_region_id == region && descriptor.tier == desired_tier) selection.desired_group_id = descriptor.stable_id;
		const RuntimeGroup *active = _best_active(region, desired_tier);
		if (!active) {
			// A manifest without an active persistent far group is intentionally
			// omitted rather than pretending an empty AS is valid coverage.
			continue;
		}
		selection.active_group_id = active->descriptor->stable_id;
		selection.active_tier = active->descriptor->tier;
		selection.cluster_ids = active->descriptor->cluster_ids;
		selection.coarse_fallback = uint32_t(selection.active_tier) < uint32_t(selection.desired_tier);
		result.coarse_fallbacks += selection.coarse_fallback;
		result.off_screen_retained += selection.off_screen_retained;
		switch (selection.active_tier) {
			case RayTransportTier::TIER_NEAR: result.near_regions++; break;
			case RayTransportTier::TIER_MIDDLE: result.middle_regions++; break;
			case RayTransportTier::TIER_FAR: result.far_regions++; break;
		}
		result.regions.push_back(selection);
	}
	return result;
}

const RayGroupDescriptor *VirtualGeometryRayHierarchy::_descriptor(uint64_t p_group_id) const {
	for (const RayGroupDescriptor &descriptor : manifest.ray_groups) if (descriptor.stable_id == p_group_id) return &descriptor;
	return nullptr;
}

Error VirtualGeometryRayHierarchy::queue_build(uint64_t p_group_id, uint64_t p_version) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(p_version == 0, ERR_INVALID_PARAMETER);
	if (runtime->state == RayGroupBuildState::ACTIVE && runtime->active_version == p_version) {
		diagnostics.reuses++;
		return OK;
	}
	ERR_FAIL_COND_V(runtime->state != RayGroupBuildState::UNAVAILABLE && runtime->state != RayGroupBuildState::ACTIVE && runtime->state != RayGroupBuildState::FAILED, ERR_BUSY);
	runtime->requested_version = p_version;
	runtime->state = RayGroupBuildState::BUILD_PENDING;
	_update_pending_count();
	return OK;
}

Error VirtualGeometryRayHierarchy::begin_build(uint64_t p_group_id, uint64_t p_submission_serial, uint64_t p_uncompacted_bytes, bool p_compaction_supported) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(runtime->state != RayGroupBuildState::BUILD_PENDING || p_submission_serial == 0 || p_uncompacted_bytes == 0, ERR_INVALID_PARAMETER);
	runtime->build_serial = p_submission_serial;
	runtime->uncompacted_bytes = p_uncompacted_bytes;
	runtime->compaction_supported = p_compaction_supported;
	runtime->state = RayGroupBuildState::BUILDING;
	diagnostics.builds++;
	diagnostics.uncompacted_bytes += p_uncompacted_bytes;
	_update_pending_count();
	return OK;
}

Error VirtualGeometryRayHierarchy::complete_build(uint64_t p_group_id, uint64_t p_completed_serial, uint64_t p_compacted_size) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(runtime->state != RayGroupBuildState::BUILDING || p_completed_serial < runtime->build_serial, ERR_BUSY);
	if (runtime->compaction_supported && p_compacted_size > 0 && p_compacted_size < runtime->uncompacted_bytes) {
		runtime->compacted_bytes = p_compacted_size;
		runtime->state = RayGroupBuildState::COMPACTION_SIZE_PENDING;
	} else {
		runtime->ready_serial = runtime->build_serial;
		runtime->state = RayGroupBuildState::PENDING_SWAP;
	}
	_update_pending_count();
	return OK;
}

Error VirtualGeometryRayHierarchy::begin_compaction(uint64_t p_group_id, uint64_t p_submission_serial) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(runtime->state != RayGroupBuildState::COMPACTION_SIZE_PENDING || p_submission_serial == 0, ERR_INVALID_PARAMETER);
	runtime->compact_serial = p_submission_serial;
	runtime->state = RayGroupBuildState::COMPACTING;
	diagnostics.compactions++;
	_update_pending_count();
	return OK;
}

Error VirtualGeometryRayHierarchy::complete_compaction(uint64_t p_group_id, uint64_t p_completed_serial) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(runtime->state != RayGroupBuildState::COMPACTING || p_completed_serial < runtime->compact_serial, ERR_BUSY);
	runtime->ready_serial = runtime->compact_serial;
	runtime->state = RayGroupBuildState::PENDING_SWAP;
	diagnostics.compacted_bytes += runtime->compacted_bytes;
	_update_pending_count();
	return OK;
}

Error VirtualGeometryRayHierarchy::fail_build(uint64_t p_group_id) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	if (runtime->active_version != 0) runtime->state = RayGroupBuildState::ACTIVE;
	else runtime->state = RayGroupBuildState::FAILED;
	runtime->requested_version = 0;
	diagnostics.failures++;
	_update_pending_count();
	return OK;
}

void VirtualGeometryRayHierarchy::activate_frame_boundary(uint64_t p_completed_serial, uint64_t p_frame_submission_serial) {
	for (KeyValue<uint64_t, RuntimeGroup> &entry : groups) {
		RuntimeGroup &runtime = entry.value;
		if (runtime.state != RayGroupBuildState::PENDING_SWAP || runtime.ready_serial > p_completed_serial) continue;
		if (runtime.active_version != 0 && runtime.active_version != runtime.requested_version) retirements.push_back({ entry.key, runtime.active_version, MAX(runtime.last_use_serial, p_frame_submission_serial) });
		runtime.active_version = runtime.requested_version;
		runtime.requested_version = 0;
		runtime.last_use_serial = p_frame_submission_serial;
		runtime.state = RayGroupBuildState::ACTIVE;
		diagnostics.swaps++;
	}
	_update_pending_count();
}

void VirtualGeometryRayHierarchy::mark_used(uint64_t p_group_id, uint64_t p_submission_serial) {
	RuntimeGroup *runtime = groups.getptr(p_group_id);
	if (runtime && runtime->state == RayGroupBuildState::ACTIVE) runtime->last_use_serial = MAX(runtime->last_use_serial, p_submission_serial);
}

void VirtualGeometryRayHierarchy::process_retirements(uint64_t p_completed_serial) {
	for (int index = retirements.size() - 1; index >= 0; index--) {
		if (retirements[index].last_use_serial <= p_completed_serial) {
			retirements.remove_at(index);
			diagnostics.retirements++;
		}
	}
}

RayGroupBuildState VirtualGeometryRayHierarchy::get_state(uint64_t p_group_id) const {
	const RuntimeGroup *runtime = groups.getptr(p_group_id);
	return runtime ? runtime->state : RayGroupBuildState::UNAVAILABLE;
}

uint64_t VirtualGeometryRayHierarchy::get_active_version(uint64_t p_group_id) const {
	const RuntimeGroup *runtime = groups.getptr(p_group_id);
	return runtime ? runtime->active_version : 0;
}

void VirtualGeometryRayHierarchy::_update_pending_count() {
	diagnostics.pending = 0;
	for (const KeyValue<uint64_t, RuntimeGroup> &entry : groups) if (entry.value.state != RayGroupBuildState::UNAVAILABLE && entry.value.state != RayGroupBuildState::ACTIVE && entry.value.state != RayGroupBuildState::FAILED) diagnostics.pending++;
}

} // namespace RendererVirtualGeometry
