/**************************************************************************/
/*  virtual_geometry_spatial.cpp                                          */
/**************************************************************************/

#include "virtual_geometry_spatial.h"

#include "core/templates/hashfuncs.h"

#include <cmath>

namespace RendererVirtualGeometry {

static constexpr uint64_t SPATIAL_ID_SEED = 0x5653475f53504154ULL;

static uint64_t _mix_spatial_id(uint64_t p_value, uint64_t p_seed) {
	return hash64_murmur3_64(p_value, p_seed);
}

uint64_t VirtualGeometrySpatialIdentity::cell_id(uint64_t p_package_identity, const SpatialCellKey &p_key) {
	uint64_t hash = _mix_spatial_id(p_package_identity, SPATIAL_ID_SEED);
	hash = _mix_spatial_id(uint64_t(p_key.x), hash);
	hash = _mix_spatial_id(uint64_t(p_key.y), hash);
	hash = _mix_spatial_id(uint64_t(p_key.z), hash);
	hash = _mix_spatial_id(p_key.level, hash);
	return hash == 0 ? 1 : hash;
}

uint64_t VirtualGeometrySpatialIdentity::repeated_family_id(uint64_t p_canonical_family_identity, uint64_t p_semantic_identity) {
	uint64_t hash = _mix_spatial_id(p_canonical_family_identity, SPATIAL_ID_SEED ^ 0x46414d494c59ULL);
	hash = _mix_spatial_id(p_semantic_identity, hash);
	return hash == 0 ? 1 : hash;
}

uint64_t VirtualGeometrySpatialIdentity::repeated_instance_id(uint64_t p_family_id, uint64_t p_authored_instance_identity) {
	uint64_t hash = _mix_spatial_id(p_family_id, SPATIAL_ID_SEED ^ 0x494e5354414e4345ULL);
	hash = _mix_spatial_id(p_authored_instance_identity, hash);
	return hash == 0 ? 1 : hash;
}

VirtualGeometrySpatialRuntime::SemanticBudgetRuntime::SemanticBudgetRuntime() {
	for (uint32_t i = 0; i < SPATIAL_WORKING_SET_CLASS_COUNT; i++) {
		capacity_bytes[i] = UINT64_MAX;
		used_bytes[i] = 0;
	}
}

VirtualGeometrySpatialRuntime::VirtualGeometrySpatialRuntime() {
	// These are intentionally independent defaults.  A caller may replace all
	// of them, but a geometry burst can never consume another pipeline's pool.
	budgets[int(SpatialWorkingSetClass::GEOMETRY)] = { 128 * 1024 * 1024, 8 * 1024 * 1024 };
	budgets[int(SpatialWorkingSetClass::MATERIAL)] = { 64 * 1024 * 1024, 4 * 1024 * 1024 };
	budgets[int(SpatialWorkingSetClass::EMISSION)] = { 32 * 1024 * 1024, 2 * 1024 * 1024 };
	budgets[int(SpatialWorkingSetClass::RAY)] = { 128 * 1024 * 1024, 8 * 1024 * 1024 };
	budgets[int(SpatialWorkingSetClass::COLLISION)] = { 32 * 1024 * 1024, 2 * 1024 * 1024 };
	budgets[int(SpatialWorkingSetClass::VISIBILITY)] = { 32 * 1024 * 1024, 2 * 1024 * 1024 };
	budgets[int(SpatialWorkingSetClass::SEMANTIC)] = { 16 * 1024 * 1024, 1 * 1024 * 1024 };
}

int VirtualGeometrySpatialRuntime::_class_index(SpatialWorkingSetClass p_class) {
	const int index = int(p_class);
	return index >= 0 && index < int(SPATIAL_WORKING_SET_CLASS_COUNT) ? index : -1;
}

SpatialCellKey VirtualGeometrySpatialRuntime::cell_key_for_world_position(const Vector3 &p_world_position, real_t p_cell_size, uint32_t p_level) const {
	SpatialCellKey key;
	if (p_cell_size <= real_t(0.0)) {
		return key;
	}
	key.x = int64_t(std::floor(double(p_world_position.x / p_cell_size)));
	key.y = int64_t(std::floor(double(p_world_position.y / p_cell_size)));
	key.z = int64_t(std::floor(double(p_world_position.z / p_cell_size)));
	key.level = p_level;
	return key;
}

SpatialCellIdentity VirtualGeometrySpatialRuntime::identify_cell(uint64_t p_package_identity, const Vector3 &p_world_position, real_t p_cell_size, uint32_t p_level) const {
	SpatialCellIdentity identity;
	identity.package_identity = p_package_identity;
	identity.key = cell_key_for_world_position(p_world_position, p_cell_size, p_level);
	identity.stable_id = VirtualGeometrySpatialIdentity::cell_id(p_package_identity, identity.key);
	return identity;
}

SpatialRebaseResult VirtualGeometrySpatialRuntime::rebase(const Vector3 &p_new_world_offset) {
	SpatialRebaseResult result;
	result.previous_offset = origin.world_offset;
	result.new_offset = p_new_world_offset;
	result.delta = p_new_world_offset - origin.world_offset;
	result.changed = result.delta != Vector3();
	if (result.changed) {
		origin.world_offset = p_new_world_offset;
		origin.revision++;
		diagnostics.origin_rebases++;
	}
	result.revision = origin.revision;
	return result;
}

Transform3D VirtualGeometrySpatialRuntime::to_camera_relative(const Transform3D &p_world_transform) const {
	Transform3D relative = p_world_transform;
	relative.origin -= origin.world_offset;
	return relative;
}

void VirtualGeometrySpatialRuntime::_append_cell(Vector<SpatialCellIdentity> &r_cells, uint64_t p_package_identity, const SpatialCellKey &p_key, uint32_t p_limit, uint32_t &r_overflow) {
	for (const SpatialCellIdentity &existing : r_cells) {
		if (existing.key == p_key && existing.package_identity == p_package_identity) {
			return;
		}
	}
	if (r_cells.size() >= int(p_limit)) {
		r_overflow++;
		return;
	}
	SpatialCellIdentity identity;
	identity.package_identity = p_package_identity;
	identity.key = p_key;
	identity.stable_id = VirtualGeometrySpatialIdentity::cell_id(p_package_identity, p_key);
	r_cells.push_back(identity);
}

SpatialTraversalPrediction VirtualGeometrySpatialRuntime::predict_traversal(const SpatialTraversalInput &p_input) const {
	SpatialTraversalPrediction prediction;
	const real_t horizon = MAX(real_t(0.0), p_input.horizon_seconds);
	const real_t cell_size = MAX(real_t(0.001), p_input.cell_size);
	const uint32_t limit = MAX(uint32_t(1), p_input.maximum_cells);
	const uint32_t sample_count = 16;
	Vector3 minimum = p_input.position;
	Vector3 maximum = p_input.position;

	for (uint32_t sample = 0; sample <= sample_count; sample++) {
		const real_t t = horizon * real_t(sample) / real_t(sample_count);
		const Vector3 position = p_input.position + p_input.velocity * t + p_input.acceleration * (real_t(0.5) * t * t);
		minimum = minimum.min(position);
		maximum = maximum.max(position);
		prediction.sampled_positions++;
		_append_cell(prediction.cells, p_input.package_identity, cell_key_for_world_position(position, cell_size, p_input.cell_level), limit, prediction.overflow_cells);
	}

	const real_t margin = MAX(real_t(0.0), p_input.corridor_radius) + MAX(real_t(0.0), p_input.stereo_head_radius) + cell_size * real_t(0.05);
	minimum -= Vector3(margin, margin, margin);
	maximum += Vector3(margin, margin, margin);
	const SpatialCellKey minimum_key = cell_key_for_world_position(minimum, cell_size, p_input.cell_level);
	const SpatialCellKey maximum_key = cell_key_for_world_position(maximum, cell_size, p_input.cell_level);

	const int64_t span_x = maximum_key.x - minimum_key.x + 1;
	const int64_t span_y = maximum_key.y - minimum_key.y + 1;
	const int64_t span_z = maximum_key.z - minimum_key.z + 1;
	const uint64_t possible_cells = span_x > 0 && span_y > 0 && span_z > 0 ? uint64_t(span_x) * uint64_t(span_y) * uint64_t(span_z) : 0;
	if (possible_cells > uint64_t(limit) * 16ULL) {
		prediction.overflow_cells += uint32_t(MIN(possible_cells - uint64_t(limit) * 16ULL, uint64_t(UINT32_MAX)));
	} else {
		for (int64_t z = minimum_key.z; z <= maximum_key.z; z++) {
			for (int64_t y = minimum_key.y; y <= maximum_key.y; y++) {
				for (int64_t x = minimum_key.x; x <= maximum_key.x; x++) {
					SpatialCellKey key;
					key.x = x;
					key.y = y;
					key.z = z;
					key.level = p_input.cell_level;
					_append_cell(prediction.cells, p_input.package_identity, key, limit, prediction.overflow_cells);
				}
			}
		}
	}

	for (const SpatialCellKey &key : p_input.declared_corridor) {
		_append_cell(prediction.cells, p_input.package_identity, key, limit, prediction.overflow_cells);
	}

	const real_t angular_distance = p_input.angular_velocity.length() * horizon;
	const bool rapid_turn = angular_distance >= Math::deg_to_rad(real_t(90.0));
	if (rapid_turn) {
		prediction.omnidirectional_coarse_reserve = true;
		Vector3 direction = p_input.view_direction;
		if (direction.length_squared() < real_t(0.000001)) {
			direction = Vector3(0, 0, -1);
		} else {
			direction.normalize();
		}
		const real_t travel = p_input.velocity.length() * horizon + real_t(0.5) * p_input.acceleration.length() * horizon * horizon + margin;
		const uint32_t coarse_limit = MAX(uint32_t(1), limit / 4);
		_append_cell(prediction.coarse_reserve_cells, p_input.package_identity, cell_key_for_world_position(p_input.position + direction * travel, cell_size, p_input.cell_level), coarse_limit, prediction.overflow_cells);
		_append_cell(prediction.coarse_reserve_cells, p_input.package_identity, cell_key_for_world_position(p_input.position - direction * travel, cell_size, p_input.cell_level), coarse_limit, prediction.overflow_cells);
		_append_cell(prediction.coarse_reserve_cells, p_input.package_identity, cell_key_for_world_position(p_input.position + Vector3(travel, 0, 0), cell_size, p_input.cell_level), coarse_limit, prediction.overflow_cells);
		_append_cell(prediction.coarse_reserve_cells, p_input.package_identity, cell_key_for_world_position(p_input.position - Vector3(travel, 0, 0), cell_size, p_input.cell_level), coarse_limit, prediction.overflow_cells);
	}
	prediction.coarse_fallback_required = prediction.overflow_cells != 0;
	diagnostics.traversal_predictions++;
	diagnostics.coarse_fallback_predictions += prediction.coarse_fallback_required;
	return prediction;
}

void VirtualGeometrySpatialRuntime::set_budget(SpatialWorkingSetClass p_class, const SpatialBudget &p_budget) {
	const int index = _class_index(p_class);
	if (index < 0) {
		return;
	}
	budgets[index] = p_budget;
	budgets[index].persistent_reserve_bytes = MIN(budgets[index].persistent_reserve_bytes, budgets[index].capacity_bytes);
}

SpatialBudget VirtualGeometrySpatialRuntime::get_budget(SpatialWorkingSetClass p_class) const {
	const int index = _class_index(p_class);
	return index < 0 ? SpatialBudget() : budgets[index];
}

void VirtualGeometrySpatialRuntime::set_semantic_budget(uint64_t p_semantic_class_id, SpatialWorkingSetClass p_class, uint64_t p_capacity_bytes) {
	if (p_semantic_class_id == 0) {
		return;
	}
	const int index = _class_index(p_class);
	if (index < 0) {
		return;
	}
	SemanticBudgetRuntime *runtime = semantic_budgets.getptr(p_semantic_class_id);
	if (!runtime) {
		semantic_budgets.insert(p_semantic_class_id, SemanticBudgetRuntime());
		runtime = semantic_budgets.getptr(p_semantic_class_id);
	}
	runtime->capacity_bytes[index] = p_capacity_bytes;
}

bool VirtualGeometrySpatialRuntime::reserve(const SpatialWorkingSetRequest &p_request, SpatialWorkingSetReservation &r_reservation) {
	r_reservation = {};
	r_reservation.stable_id = p_request.stable_id;
	r_reservation.working_set_class = p_request.working_set_class;
	r_reservation.bytes = p_request.bytes;
	r_reservation.persistent_coarse = p_request.persistent_coarse;
	const int index = _class_index(p_request.working_set_class);
	if (index < 0 || p_request.bytes == 0 || p_request.stable_id == 0) {
		if (index >= 0) {
			failed_reservations[index]++;
			diagnostics.reservation_failures++;
		}
		return false;
	}
	const SpatialBudget &budget = budgets[index];
	const bool capacity_ok = p_request.bytes <= budget.capacity_bytes && used_bytes[index] <= budget.capacity_bytes - p_request.bytes;
	const uint64_t available = used_bytes[index] < budget.capacity_bytes ? budget.capacity_bytes - used_bytes[index] : 0;
	const uint64_t ordinary_available = available > budget.persistent_reserve_bytes ? available - budget.persistent_reserve_bytes : 0;
	const bool reserve_ok = p_request.persistent_coarse || p_request.bytes <= ordinary_available;
	SemanticBudgetRuntime *semantic = p_request.semantic_class_id == 0 ? nullptr : semantic_budgets.getptr(p_request.semantic_class_id);
	const bool semantic_ok = !semantic || (p_request.bytes <= semantic->capacity_bytes[index] && semantic->used_bytes[index] <= semantic->capacity_bytes[index] - p_request.bytes);
	if (!capacity_ok || !reserve_ok || !semantic_ok) {
		failed_reservations[index]++;
		diagnostics.reservation_failures++;
		return false;
	}

	const uint64_t token = next_reservation_token++;
	ReservationRuntime runtime;
	runtime.request = p_request;
	reservations.insert(token, runtime);
	used_bytes[index] += p_request.bytes;
	if (p_request.persistent_coarse) {
		persistent_used_bytes[index] += p_request.bytes;
	}
	if (semantic) {
		semantic->used_bytes[index] += p_request.bytes;
	}
	diagnostics.working_set_used_bytes[index] = used_bytes[index];
	diagnostics.working_set_reserved_bytes[index] = budget.persistent_reserve_bytes;
	r_reservation.token = token;
	r_reservation.accepted = true;
	return true;
}

bool VirtualGeometrySpatialRuntime::release(uint64_t p_token) {
	ReservationRuntime *runtime = reservations.getptr(p_token);
	if (!runtime) {
		return false;
	}
	const int index = _class_index(runtime->request.working_set_class);
	if (index >= 0) {
		used_bytes[index] -= MIN(used_bytes[index], runtime->request.bytes);
		if (runtime->request.persistent_coarse) {
			persistent_used_bytes[index] -= MIN(persistent_used_bytes[index], runtime->request.bytes);
		}
		SemanticBudgetRuntime *semantic = runtime->request.semantic_class_id == 0 ? nullptr : semantic_budgets.getptr(runtime->request.semantic_class_id);
		if (semantic) {
			semantic->used_bytes[index] -= MIN(semantic->used_bytes[index], runtime->request.bytes);
		}
		diagnostics.working_set_used_bytes[index] = used_bytes[index];
	}
	reservations.erase(p_token);
	return true;
}

SpatialBudgetSnapshot VirtualGeometrySpatialRuntime::get_budget_snapshot(SpatialWorkingSetClass p_class) const {
	SpatialBudgetSnapshot snapshot;
	const int index = _class_index(p_class);
	if (index < 0) {
		return snapshot;
	}
	snapshot.capacity_bytes = budgets[index].capacity_bytes;
	snapshot.used_bytes = used_bytes[index];
	snapshot.reserved_bytes = budgets[index].persistent_reserve_bytes;
	snapshot.persistent_used_bytes = persistent_used_bytes[index];
	snapshot.available_bytes = snapshot.capacity_bytes > snapshot.used_bytes ? snapshot.capacity_bytes - snapshot.used_bytes : 0;
	snapshot.failed_reservations = failed_reservations[index];
	return snapshot;
}

SpatialSemanticBudgetSnapshot VirtualGeometrySpatialRuntime::get_semantic_budget_snapshot(uint64_t p_semantic_class_id) const {
	SpatialSemanticBudgetSnapshot snapshot;
	snapshot.semantic_class_id = p_semantic_class_id;
	const SemanticBudgetRuntime *runtime = semantic_budgets.getptr(p_semantic_class_id);
	for (uint32_t i = 0; i < SPATIAL_WORKING_SET_CLASS_COUNT; i++) {
		snapshot.capacity_bytes[i] = runtime ? runtime->capacity_bytes[i] : UINT64_MAX;
		snapshot.used_bytes[i] = runtime ? runtime->used_bytes[i] : 0;
	}
	return snapshot;
}

bool VirtualGeometrySpatialRuntime::register_repeated_instance(const SpatialRepeatedInstance &p_instance) {
	if (p_instance.family_id == 0 || p_instance.instance_id == 0 || instance_to_family.has(p_instance.instance_id)) {
		diagnostics.repeated_instance_rejections++;
		return false;
	}
	FamilyRuntime *family = families.getptr(p_instance.family_id);
	if (!family) {
		FamilyRuntime created;
		created.semantic_class_id = p_instance.semantic_class_id;
		families.insert(p_instance.family_id, created);
		family = families.getptr(p_instance.family_id);
	} else if (family->semantic_class_id != p_instance.semantic_class_id) {
		diagnostics.repeated_instance_rejections++;
		return false;
	}
	instance_to_family.insert(p_instance.instance_id, p_instance.family_id);
	instance_bytes.insert(p_instance.instance_id, p_instance.resident_bytes);
	family->instance_count++;
	family->resident_bytes += p_instance.resident_bytes;
	return true;
}

bool VirtualGeometrySpatialRuntime::unregister_repeated_instance(uint64_t p_instance_id) {
	const uint64_t *family_id = instance_to_family.getptr(p_instance_id);
	if (!family_id) {
		return false;
	}
	FamilyRuntime *family = families.getptr(*family_id);
	if (family && family->instance_count > 0) {
		family->instance_count--;
		const uint64_t *bytes = instance_bytes.getptr(p_instance_id);
		if (bytes) {
			family->resident_bytes -= MIN(family->resident_bytes, *bytes);
		}
		// A family is retained until its final instance, avoiding a scene node
		// per repeat.
	}
	if (family && family->instance_count == 0) {
		families.erase(*family_id);
	}
	instance_to_family.erase(p_instance_id);
	instance_bytes.erase(p_instance_id);
	return true;
}

Vector<SpatialRepeatedFamilyDiagnostics> VirtualGeometrySpatialRuntime::get_repeated_family_diagnostics() const {
	Vector<uint64_t> ids;
	for (const KeyValue<uint64_t, FamilyRuntime> &entry : families) {
		ids.push_back(entry.key);
	}
	ids.sort();
	Vector<SpatialRepeatedFamilyDiagnostics> result;
	for (uint64_t id : ids) {
		const FamilyRuntime *family = families.getptr(id);
		if (!family) {
			continue;
		}
		SpatialRepeatedFamilyDiagnostics diagnostic;
		diagnostic.family_id = id;
		diagnostic.semantic_class_id = family->semantic_class_id;
		diagnostic.instance_count = family->instance_count;
		diagnostic.resident_bytes = family->resident_bytes;
		result.push_back(diagnostic);
	}
	return result;
}

SpatialCoarseEligibility VirtualGeometrySpatialRuntime::evaluate_coarse_eligibility(const SpatialCoarseEligibilityInput &p_input) const {
	SpatialCoarseEligibility result;
	if (!p_input.package_eligible) {
		result.fail_open = true;
		result.reason = "Spatial package is not eligible; retain the caller's coarse world fallback.";
		return result;
	}
	if (!p_input.persistent_raster_coarse_available || (p_input.transport_relevant && !p_input.persistent_ray_coarse_available)) {
		result.fail_open = true;
		result.reason = "Persistent coarse coverage is unavailable for a required pipeline.";
		return result;
	}
	result.eligible = true;
	if (!p_input.fine_page_resident || !p_input.dependencies_ready || !p_input.visibility_data_valid) {
		result.use_coarse_fallback = true;
		result.fail_open = !p_input.visibility_data_valid;
		result.reason = p_input.visibility_data_valid ? "Fine detail or dependency is unavailable; use persistent coarse coverage." : "Visibility data is invalid; fail open to persistent coarse coverage.";
	}
	return result;
}

} // namespace RendererVirtualGeometry
