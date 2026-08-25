/**************************************************************************/
/*  virtual_geometry_spatial.h                                            */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

namespace RendererVirtualGeometry {

// These identities are part of the portable world contract.  They are based
// on authored identities and integer cell coordinates, never on scene-node or
// page allocation order.
struct SpatialCellKey {
	int64_t x = 0;
	int64_t y = 0;
	int64_t z = 0;
	uint32_t level = 0;

	bool operator==(const SpatialCellKey &p_other) const {
		return x == p_other.x && y == p_other.y && z == p_other.z && level == p_other.level;
	}
	bool operator!=(const SpatialCellKey &p_other) const { return !(*this == p_other); }
	bool less_than(const SpatialCellKey &p_other) const {
		if (level != p_other.level) {
			return level < p_other.level;
		}
		if (x != p_other.x) {
			return x < p_other.x;
		}
		if (y != p_other.y) {
			return y < p_other.y;
		}
		return z < p_other.z;
	}
};

struct SpatialCellIdentity {
	uint64_t stable_id = 0;
	uint64_t package_identity = 0;
	SpatialCellKey key;
};

class VirtualGeometrySpatialIdentity {
public:
	static uint64_t cell_id(uint64_t p_package_identity, const SpatialCellKey &p_key);
	static uint64_t repeated_family_id(uint64_t p_canonical_family_identity, uint64_t p_semantic_identity);
	static uint64_t repeated_instance_id(uint64_t p_family_id, uint64_t p_authored_instance_identity);
};

struct SpatialOriginState {
	Vector3 world_offset;
	uint64_t revision = 0;
};

struct SpatialRebaseResult {
	Vector3 previous_offset;
	Vector3 new_offset;
	Vector3 delta;
	uint64_t revision = 0;
	bool changed = false;
};

enum class SpatialWorkingSetClass : uint8_t {
	GEOMETRY,
	MATERIAL,
	EMISSION,
	RAY,
	COLLISION,
	VISIBILITY,
	SEMANTIC,
	COUNT,
};

static constexpr uint32_t SPATIAL_WORKING_SET_CLASS_COUNT = uint32_t(SpatialWorkingSetClass::COUNT);

struct SpatialBudget {
	uint64_t capacity_bytes = 0;
	uint64_t persistent_reserve_bytes = 0;
};

struct SpatialBudgetSnapshot {
	uint64_t capacity_bytes = 0;
	uint64_t used_bytes = 0;
	uint64_t reserved_bytes = 0;
	uint64_t persistent_used_bytes = 0;
	uint64_t available_bytes = 0;
	uint64_t failed_reservations = 0;
};

struct SpatialWorkingSetRequest {
	uint64_t stable_id = 0;
	uint64_t semantic_class_id = 0;
	SpatialWorkingSetClass working_set_class = SpatialWorkingSetClass::GEOMETRY;
	uint64_t bytes = 0;
	uint32_t priority = 0;
	bool persistent_coarse = false;
};

struct SpatialWorkingSetReservation {
	uint64_t token = 0;
	uint64_t stable_id = 0;
	SpatialWorkingSetClass working_set_class = SpatialWorkingSetClass::GEOMETRY;
	uint64_t bytes = 0;
	bool persistent_coarse = false;
	bool accepted = false;
};

struct SpatialSemanticBudgetSnapshot {
	uint64_t semantic_class_id = 0;
	uint64_t capacity_bytes[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
	uint64_t used_bytes[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
};

struct SpatialTraversalInput {
	Vector3 position;
	Vector3 velocity;
	Vector3 acceleration;
	Vector3 view_direction = Vector3(0, 0, -1);
	Vector3 angular_velocity;
	real_t horizon_seconds = 0.75;
	real_t corridor_radius = 8.0;
	real_t stereo_head_radius = 0.08;
	real_t cell_size = 128.0;
	uint32_t cell_level = 0;
	uint64_t package_identity = 0;
	uint32_t maximum_cells = 256;
	// A world streamer may provide already-authorized coarse corridor cells.
	// The spatial runtime does not interpret an external package schema.
	Vector<SpatialCellKey> declared_corridor;
};

struct SpatialTraversalPrediction {
	Vector<SpatialCellIdentity> cells;
	Vector<SpatialCellIdentity> coarse_reserve_cells;
	uint32_t sampled_positions = 0;
	uint32_t overflow_cells = 0;
	bool omnidirectional_coarse_reserve = false;
	bool coarse_fallback_required = false;
};

struct SpatialRepeatedInstance {
	uint64_t family_id = 0;
	uint64_t instance_id = 0;
	uint64_t semantic_class_id = 0;
	SpatialCellKey cell;
	AABB bounds;
	uint64_t resident_bytes = 0;
};

struct SpatialRepeatedFamilyDiagnostics {
	uint64_t family_id = 0;
	uint64_t semantic_class_id = 0;
	uint32_t instance_count = 0;
	uint64_t resident_bytes = 0;
};

struct SpatialCoarseEligibilityInput {
	bool package_eligible = true;
	bool persistent_raster_coarse_available = true;
	bool persistent_ray_coarse_available = true;
	bool fine_page_resident = true;
	bool dependencies_ready = true;
	bool visibility_data_valid = true;
	bool transport_relevant = false;
};

struct SpatialCoarseEligibility {
	bool eligible = false;
	bool use_coarse_fallback = false;
	bool fail_open = false;
	String reason;
};

struct SpatialRuntimeDiagnostics {
	uint64_t origin_rebases = 0;
	uint64_t traversal_predictions = 0;
	uint64_t coarse_fallback_predictions = 0;
	uint64_t repeated_instance_rejections = 0;
	uint64_t reservation_failures = 0;
	uint64_t working_set_used_bytes[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
	uint64_t working_set_reserved_bytes[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
};

// Product-neutral spatial policy core.  It supplies identities, camera
// rebasing, conservative prediction, repeated-family accounting, and bounded
// working-set reservations.  Package decoding and scene integration remain
// outside this class.
class VirtualGeometrySpatialRuntime {
public:
	VirtualGeometrySpatialRuntime();

	SpatialCellKey cell_key_for_world_position(const Vector3 &p_world_position, real_t p_cell_size, uint32_t p_level = 0) const;
	SpatialCellIdentity identify_cell(uint64_t p_package_identity, const Vector3 &p_world_position, real_t p_cell_size, uint32_t p_level = 0) const;

	SpatialRebaseResult rebase(const Vector3 &p_new_world_offset);
	const SpatialOriginState &get_origin_state() const { return origin; }
	Vector3 to_camera_relative(const Vector3 &p_world_position) const { return p_world_position - origin.world_offset; }
	Vector3 to_world_position(const Vector3 &p_camera_relative_position) const { return p_camera_relative_position + origin.world_offset; }
	Transform3D to_camera_relative(const Transform3D &p_world_transform) const;

	SpatialTraversalPrediction predict_traversal(const SpatialTraversalInput &p_input) const;

	void set_budget(SpatialWorkingSetClass p_class, const SpatialBudget &p_budget);
	SpatialBudget get_budget(SpatialWorkingSetClass p_class) const;
	void set_semantic_budget(uint64_t p_semantic_class_id, SpatialWorkingSetClass p_class, uint64_t p_capacity_bytes);
	bool reserve(const SpatialWorkingSetRequest &p_request, SpatialWorkingSetReservation &r_reservation);
	bool release(uint64_t p_token);
	SpatialBudgetSnapshot get_budget_snapshot(SpatialWorkingSetClass p_class) const;
	SpatialSemanticBudgetSnapshot get_semantic_budget_snapshot(uint64_t p_semantic_class_id) const;

	bool register_repeated_instance(const SpatialRepeatedInstance &p_instance);
	bool unregister_repeated_instance(uint64_t p_instance_id);
	Vector<SpatialRepeatedFamilyDiagnostics> get_repeated_family_diagnostics() const;

	SpatialCoarseEligibility evaluate_coarse_eligibility(const SpatialCoarseEligibilityInput &p_input) const;

	const SpatialRuntimeDiagnostics &get_diagnostics() const { return diagnostics; }

private:
	struct SemanticBudgetRuntime {
		uint64_t capacity_bytes[SPATIAL_WORKING_SET_CLASS_COUNT];
		uint64_t used_bytes[SPATIAL_WORKING_SET_CLASS_COUNT];
		SemanticBudgetRuntime();
	};
	struct ReservationRuntime {
		SpatialWorkingSetRequest request;
	};
	struct FamilyRuntime {
		uint64_t semantic_class_id = 0;
		uint32_t instance_count = 0;
		uint64_t resident_bytes = 0;
	};

	static int _class_index(SpatialWorkingSetClass p_class);
	static void _append_cell(Vector<SpatialCellIdentity> &r_cells, uint64_t p_package_identity, const SpatialCellKey &p_key, uint32_t p_limit, uint32_t &r_overflow);

	SpatialOriginState origin;
	SpatialBudget budgets[SPATIAL_WORKING_SET_CLASS_COUNT];
	uint64_t used_bytes[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
	uint64_t persistent_used_bytes[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
	uint64_t failed_reservations[SPATIAL_WORKING_SET_CLASS_COUNT] = {};
	HashMap<uint64_t, SemanticBudgetRuntime> semantic_budgets;
	HashMap<uint64_t, ReservationRuntime> reservations;
	HashMap<uint64_t, uint64_t> instance_to_family;
	HashMap<uint64_t, uint64_t> instance_bytes;
	HashMap<uint64_t, FamilyRuntime> families;
	uint64_t next_reservation_token = 1;
	mutable SpatialRuntimeDiagnostics diagnostics;
};

} // namespace RendererVirtualGeometry
