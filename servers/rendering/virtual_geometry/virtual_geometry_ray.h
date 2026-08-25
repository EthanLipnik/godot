/**************************************************************************/
/*  virtual_geometry_ray.h                                                */
/**************************************************************************/

#pragma once

#include "servers/rendering/virtual_geometry/virtual_geometry_format.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

namespace RendererVirtualGeometry {

enum class RayTransportRole : uint8_t {
	PRIMARY,
	SHARP_REFLECTION,
	ROUGH_REFLECTION,
	DIRECT_VISIBILITY,
	DIFFUSE_INDIRECT,
	EMISSIVE_VISIBILITY,
	REFERENCE,
};

struct RayTransportSelectionInput {
	RayTransportRole role = RayTransportRole::DIFFUSE_INDIRECT;
	float distance = 0.0f;
	float ray_footprint = 0.0f;
	float roughness = 1.0f;
	uint32_t path_depth = 0;
	float expected_contribution = 1.0f;
	bool off_screen_influence = false;
	float near_distance = 30.0f;
	float middle_distance = 180.0f;
};

struct RayTransportRegionSelection {
	uint64_t transport_region_id = 0;
	uint64_t desired_group_id = 0;
	uint64_t active_group_id = 0;
	RayTransportTier desired_tier = RayTransportTier::TIER_FAR;
	RayTransportTier active_tier = RayTransportTier::TIER_FAR;
	Vector<uint64_t> cluster_ids;
	bool coarse_fallback = false;
	bool off_screen_retained = false;
};

struct RayTransportSelection {
	Vector<RayTransportRegionSelection> regions;
	uint32_t near_regions = 0;
	uint32_t middle_regions = 0;
	uint32_t far_regions = 0;
	uint32_t coarse_fallbacks = 0;
	uint32_t off_screen_retained = 0;
};

enum class RayGroupBuildState : uint8_t {
	UNAVAILABLE,
	BUILD_PENDING,
	BUILDING,
	COMPACTION_SIZE_PENDING,
	COMPACTING,
	PENDING_SWAP,
	ACTIVE,
	FAILED,
};

struct RayGroupLifecycleDiagnostics {
	uint64_t builds = 0;
	uint64_t compactions = 0;
	uint64_t swaps = 0;
	uint64_t reuses = 0;
	uint64_t retirements = 0;
	uint64_t failures = 0;
	uint64_t pending = 0;
	uint64_t uncompacted_bytes = 0;
	uint64_t compacted_bytes = 0;
};

class VirtualGeometryRayHierarchy {
public:
	Error set_manifest(const Manifest &p_manifest);
	static RayTransportTier select_desired_tier(const RayTransportSelectionInput &p_input);
	RayTransportSelection select(const RayTransportSelectionInput &p_input) const;

	Error queue_build(uint64_t p_group_id, uint64_t p_version);
	Error begin_build(uint64_t p_group_id, uint64_t p_submission_serial, uint64_t p_uncompacted_bytes, bool p_compaction_supported);
	Error complete_build(uint64_t p_group_id, uint64_t p_completed_serial, uint64_t p_compacted_size);
	Error begin_compaction(uint64_t p_group_id, uint64_t p_submission_serial);
	Error complete_compaction(uint64_t p_group_id, uint64_t p_completed_serial);
	Error fail_build(uint64_t p_group_id);
	void activate_frame_boundary(uint64_t p_completed_serial, uint64_t p_frame_submission_serial);
	void mark_used(uint64_t p_group_id, uint64_t p_submission_serial);
	void process_retirements(uint64_t p_completed_serial);

	RayGroupBuildState get_state(uint64_t p_group_id) const;
	uint64_t get_active_version(uint64_t p_group_id) const;
	const RayGroupLifecycleDiagnostics &get_diagnostics() const { return diagnostics; }

private:
	struct RuntimeGroup {
		const RayGroupDescriptor *descriptor = nullptr;
		RayGroupBuildState state = RayGroupBuildState::UNAVAILABLE;
		uint64_t requested_version = 0;
		uint64_t active_version = 0;
		uint64_t build_serial = 0;
		uint64_t compact_serial = 0;
		uint64_t ready_serial = 0;
		uint64_t last_use_serial = 0;
		uint64_t uncompacted_bytes = 0;
		uint64_t compacted_bytes = 0;
		bool compaction_supported = false;
	};
	struct Retirement {
		uint64_t group_id = 0;
		uint64_t version = 0;
		uint64_t last_use_serial = 0;
	};

	const RayGroupDescriptor *_descriptor(uint64_t p_group_id) const;
	const RuntimeGroup *_best_active(uint64_t p_region_id, RayTransportTier p_desired) const;
	void _update_pending_count();

	Manifest manifest;
	HashMap<uint64_t, RuntimeGroup> groups;
	Vector<Retirement> retirements;
	RayGroupLifecycleDiagnostics diagnostics;
};

} // namespace RendererVirtualGeometry
