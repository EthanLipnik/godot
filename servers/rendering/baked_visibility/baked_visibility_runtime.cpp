/**************************************************************************/
/*  baked_visibility_runtime.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "baked_visibility_runtime.h"

#include "core/io/resource_uid.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "scene/3d/baked_visibility_volume_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/baked_visibility/baked_visibility_baker.h"

struct BakedVisibilityRuntime::Volume {
	struct Mapping {
		uint64_t rid = 0;
		uint8_t kind = 0;
		uint32_t flags = 0;
		AABB local_bounds;
		uint64_t version = 0;
		NodePath path;
		PackedByteArray signature_sha256;
		ObjectID node_id;
	};
	struct CachedLeaf {
		uint32_t tile_index = UINT32_MAX;
		Vector<uint32_t> cell_indices;
		Vector<BakedVisibilityData3DData::Cell> cells;
		Vector<Vector<uint32_t>> sets;
		uint64_t last_used = 0;
	};
	static constexpr uint32_t TILE_CACHE_CAPACITY = 8;

	bool valid = false;
	uint64_t scenario_id = 0;
	String error;
	Transform3D world_from_local;
	AABB local_bounds;
	Vector3 cell_size;
	Vector3i grid_size;
	Vector3i tile_grid_size;
	uint32_t hierarchy_depth = 0;
	uint32_t bake_mask = 0;
	float transport_distance = -1.0f;
	float lookup_margin = 0.1f;
	String source_identity;
	bool verify_signatures = false;
	// Keep the immutable resource, not a second monolithic runtime copy. Tile
	// routing and per-query admission always use the validated resource payload.
	Ref<BakedVisibilityData3D> data;
	Vector<CachedLeaf> tile_cache;
	uint64_t tile_cache_clock = 0;
	uint64_t tile_cache_decodes = 0;
	uint64_t tile_cache_evictions = 0;
	Vector<Mapping> mappings;
};

BakedVisibilityRuntime::BakedVisibilityRuntime() {
	mutex = memnew(Mutex);
}

BakedVisibilityRuntime::~BakedVisibilityRuntime() {
	memdelete(mutex);
}

BakedVisibilityRuntime &BakedVisibilityRuntime::get_singleton() {
	static BakedVisibilityRuntime singleton;
	return singleton;
}

void BakedVisibilityRuntime::_register_volume(BakedVisibilityVolume3D *p_volume, bool p_test) {
	ERR_FAIL_NULL(p_volume);
	Volume volume;
	Ref<BakedVisibilityData3D> resource = p_volume->get_data();
	const BakedVisibilityData3DData *data = resource.is_valid() ? resource->get_baked_data() : nullptr;
	if (p_volume->is_inside_tree()) {
		Ref<World3D> world = p_volume->get_world_3d();
		volume.scenario_id = world.is_valid() ? world->get_scenario().get_id() : 0;
	}
	if (!p_test && volume.scenario_id == 0) {
		volume.error = "anchor has no scenario";
	} else if (!p_volume->is_enabled() || !data) {
		volume.error = "disabled or invalid data";
	} else if ((p_volume->is_inside_tree() ? p_volume->get_global_transform() : p_volume->get_transform()).basis.determinant() == 0.0f) {
		volume.error = "noninvertible anchor transform";
	} else {
		ResourceUID *uid = ResourceUID::get_singleton();
		if (!p_test && (!uid || uid->get_path_id(data->source_path) != data->source_uid || BakedVisibilityBaker::make_source_digest(data->source_path) != data->source_sha256)) {
			volume.error = "source UID or dependency digest changed";
		}
		Node *root = p_volume->get_node_or_null(p_volume->get_bake_root());
		if (!root) {
			volume.error = "unresolved bake root";
		} else {
			volume.world_from_local = p_volume->is_inside_tree() ? p_volume->get_global_transform() : p_volume->get_transform();
			volume.local_bounds = data->local_bounds;
			volume.cell_size = data->cell_size;
			volume.grid_size = data->grid_size;
			volume.tile_grid_size = data->tile_grid_size;
			volume.hierarchy_depth = data->hierarchy_depth;
			volume.bake_mask = data->bake_mask;
			volume.transport_distance = data->transport_distance;
			volume.lookup_margin = data->lookup_margin;
			volume.source_identity = String::hex_encode_buffer(data->source_sha256.ptr(), data->source_sha256.size());
			volume.verify_signatures = !p_test;
			if (p_volume->get_bake_mask() != data->bake_mask || (p_volume->get_transport_distance() > 0.0f && !Math::is_equal_approx(p_volume->get_transport_distance(), data->transport_distance)) || !Math::is_equal_approx(p_volume->get_lookup_margin(), data->lookup_margin)) {
				volume.valid = false;
				volume.error = "anchor settings differ from baked data";
			}
			volume.data = resource;
			volume.mappings.resize(data->instances.size());
			volume.valid = volume.error.is_empty();
			for (int i = 0; i < data->instances.size(); i++) {
				const BakedVisibilityData3DData::Instance &entry = data->instances[i];
				Node *node = root->get_node_or_null(entry.path);
				String identity = volume.source_identity;
				if (entry.kind == BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY) identity += ":" + BakedVisibilityBaker::make_geometry_identity(Object::cast_to<MeshInstance3D>(node));
				if (!p_test && BakedVisibilityBaker::make_instance_signature(entry.path, entry.kind, entry.local_bounds, entry.flags, identity) != entry.signature_sha256) {
					volume.valid = false;
					volume.error = "invalid persisted instance signature";
					break;
				}
				VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(node);
				Light3D *light = Object::cast_to<Light3D>(node);
				const bool kind_matches = (entry.kind == BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY && visual && !light) ||
						(entry.kind != BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY && light);
				if (!kind_matches) {
					volume.valid = false;
					volume.error = "unresolved baked path or kind";
					break;
				}
				volume.mappings.write[i].rid = (light ? light->get_instance() : visual->get_instance()).get_id();
				volume.mappings.write[i].kind = entry.kind;
				volume.mappings.write[i].flags = entry.flags;
				volume.mappings.write[i].local_bounds = entry.local_bounds;
				volume.mappings.write[i].path = entry.path;
				volume.mappings.write[i].signature_sha256 = entry.signature_sha256;
				volume.mappings.write[i].node_id = node->get_instance_id();
			}
		}
	}
	MutexLock lock(*mutex);
	volumes[p_volume->get_instance_id()] = volume;
}

void BakedVisibilityRuntime::register_volume(BakedVisibilityVolume3D *p_volume) {
	_register_volume(p_volume, false);
}

void BakedVisibilityRuntime::register_test_volume(BakedVisibilityVolume3D *p_volume) {
	_register_volume(p_volume, true);
}

void BakedVisibilityRuntime::unregister_volume(ObjectID p_volume_id) {
	MutexLock lock(*mutex);
	volumes.erase(p_volume_id);
}

BakedVisibilityRuntimeResult BakedVisibilityRuntime::query(uint64_t p_scenario_id, const Vector<Vector3> &p_eye_origins, uint32_t p_visible_layers, const Vector<BakedVisibilityRuntimeCandidate> &p_candidates) {
	BakedVisibilityRuntimeResult result;
	HashMap<uint64_t, const BakedVisibilityRuntimeCandidate *> candidates;
	for (int i = 0; i < p_candidates.size(); i++) {
		candidates[p_candidates[i].rid] = &p_candidates[i];
	}
	MutexLock lock(*mutex);
	if (volumes.is_empty()) {
		result.reason = "no volumes";
		return result;
	}
	if (p_eye_origins.is_empty()) {
		result.fail_open = true;
		result.reason = "no eye origins";
		return result;
	}
	Vector<bool> eye_covered;
	eye_covered.resize(p_eye_origins.size());
	for (int i = 0; i < eye_covered.size(); i++) {
		eye_covered.write[i] = false;
	}
	for (KeyValue<ObjectID, Volume> &E : volumes) {
		Volume &volume = E.value;
		if (volume.scenario_id != p_scenario_id) {
			continue;
		}
		result.volume_count++;
		if (!volume.valid || volume.world_from_local.basis.determinant() == 0.0f) {
			result.fail_open = true;
			result.reason = volume.error.is_empty() ? "invalid volume" : volume.error;
			return result;
		}
		const BakedVisibilityData3DData *data = volume.data.is_valid() ? volume.data->get_baked_data() : nullptr;
		if (!data) {
			result.fail_open = true;
			result.reason = "tile resource became unavailable";
			return result;
		}
		auto acquire_leaf = [&](uint32_t p_tile_index, Volume::CachedLeaf *&r_leaf) -> bool {
			for (int cache_index = 0; cache_index < volume.tile_cache.size(); cache_index++) {
				Volume::CachedLeaf &cached = volume.tile_cache.write[cache_index];
				if (cached.tile_index == p_tile_index) {
					cached.last_used = ++volume.tile_cache_clock;
					r_leaf = &cached;
					return true;
				}
			}
			if (p_tile_index >= uint32_t(data->tiles.size())) {
				return false;
			}
			const BakedVisibilityData3DData::Tile &source_tile = data->tiles[p_tile_index];
			if (!(source_tile.flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) {
				return false;
			}
			if (volume.tile_cache.size() >= int(Volume::TILE_CACHE_CAPACITY)) {
				int oldest = 0;
				for (int cache_index = 1; cache_index < volume.tile_cache.size(); cache_index++) {
					if (volume.tile_cache[cache_index].last_used < volume.tile_cache[oldest].last_used) oldest = cache_index;
				}
				volume.tile_cache.remove_at(oldest);
				volume.tile_cache_evictions++;
			}
			Volume::CachedLeaf cached;
			cached.tile_index = p_tile_index;
			cached.last_used = ++volume.tile_cache_clock;
			if (!volume.data->decode_leaf_payload(p_tile_index, cached.cell_indices, cached.cells, cached.sets)) return false;
			volume.tile_cache_decodes++;
			volume.tile_cache.push_back(cached);
			r_leaf = &volume.tile_cache.write[volume.tile_cache.size() - 1];
			return true;
		};
		const float volume_distance = volume.transport_distance > 0.0f ? volume.transport_distance : -1.0f;
		if (p_visible_layers != volume.bake_mask) {
			result.fail_open = true;
			result.reason = "visible layer mask differs from bake";
			return result;
		}
		if (result.transport_distance < 0.0f) {
			result.transport_distance = volume_distance;
		} else if (!Math::is_equal_approx(result.transport_distance, volume_distance)) {
			result.transport_distance = -1.0f;
		}
		Vector<uint32_t> selected_primary;
		Vector<uint32_t> selected_transport;
		bool volume_covered = false;
		const Transform3D local_from_world = volume.world_from_local.affine_inverse();
		for (int eye_index = 0; eye_index < p_eye_origins.size(); eye_index++) {
			const Vector3 &eye = p_eye_origins[eye_index];
			const Vector3 local = local_from_world.xform(eye);
			const Vector3 rel = local - volume.local_bounds.position;
			const Vector3 min_rel = rel - Vector3(volume.lookup_margin, volume.lookup_margin, volume.lookup_margin);
			const Vector3 max_rel = rel + Vector3(volume.lookup_margin, volume.lookup_margin, volume.lookup_margin);
			if (min_rel.x < 0.0f || min_rel.y < 0.0f || min_rel.z < 0.0f || max_rel.x >= volume.local_bounds.size.x || max_rel.y >= volume.local_bounds.size.y || max_rel.z >= volume.local_bounds.size.z) {
				continue;
			}
			eye_covered.write[eye_index] = true;
			volume_covered = true;
			const Vector3i min_cell(int(Math::floor(min_rel.x / volume.cell_size.x)), int(Math::floor(min_rel.y / volume.cell_size.y)), int(Math::floor(min_rel.z / volume.cell_size.z)));
			const Vector3i max_cell(int(Math::floor(max_rel.x / volume.cell_size.x)), int(Math::floor(max_rel.y / volume.cell_size.y)), int(Math::floor(max_rel.z / volume.cell_size.z)));
			for (int z = min_cell.z; z <= max_cell.z; z++) for (int y = min_cell.y; y <= max_cell.y; y++) for (int x = min_cell.x; x <= max_cell.x; x++) {
				const uint32_t cell_index = x + volume.grid_size.x * (y + volume.grid_size.y * z);
				const Vector3i tile_coordinate(x / int(BakedVisibilityData3DData::TILE_SIZE), y / int(BakedVisibilityData3DData::TILE_SIZE), z / int(BakedVisibilityData3DData::TILE_SIZE));
				const uint32_t tile_index = tile_coordinate.x + volume.tile_grid_size.x * (tile_coordinate.y + volume.tile_grid_size.y * tile_coordinate.z);
				const uint64_t total_cells = uint64_t(volume.grid_size.x) * volume.grid_size.y * volume.grid_size.z;
				if (cell_index >= total_cells || tile_index >= uint32_t(data->tiles.size())) {
					result.fail_open = true;
					result.reason = "invalid tile or cell lookup";
					return result;
				}
				const BakedVisibilityData3DData::Tile &leaf = data->tiles[tile_index];
				bool contains_cell = leaf.coordinate == tile_coordinate && leaf.level == 0 && (leaf.flags & BakedVisibilityData3DData::Tile::FLAG_LEAF);
				int hierarchy_index = tile_index;
				for (uint32_t level = 1; contains_cell && level < volume.hierarchy_depth; level++) {
					const int parent = data->tiles[hierarchy_index].parent;
					if (parent < 0 || parent >= data->tiles.size() || data->tiles[parent].level != level) {
						contains_cell = false;
						break;
					}
					hierarchy_index = parent;
				}
				Volume::CachedLeaf *cached_leaf = nullptr;
				int cached_cell = -1;
				if (contains_cell && acquire_leaf(tile_index, cached_leaf)) {
					for (int cached_index = 0; cached_index < cached_leaf->cell_indices.size(); cached_index++) {
						if (cached_leaf->cell_indices[cached_index] == cell_index) {
							cached_cell = cached_index;
							break;
						}
					}
				}
				if (!contains_cell || cached_cell < 0 || data->tiles[hierarchy_index].parent != -1 || (cached_leaf->cells[cached_cell].flags & (BakedVisibilityData3DData::CELL_FLAG_DEGRADED | BakedVisibilityData3DData::CELL_FLAG_FAIL_OPEN))) {
					result.fail_open = true;
					result.reason = "invalid, unavailable, or degraded tile";
					return result;
				}
				const BakedVisibilityData3DData::Cell &cached = cached_leaf->cells[cached_cell];
				if (cached.primary_set >= uint32_t(cached_leaf->sets.size()) || cached.transport_set >= uint32_t(cached_leaf->sets.size())) {
					result.fail_open = true;
					result.reason = "invalid cached tile set";
					return result;
				}
				selected_primary.append_array(cached_leaf->sets[cached.primary_set]);
				selected_transport.append_array(cached_leaf->sets[cached.transport_set]);
				result.cell_count++;
			}
		}
		if (!volume_covered) {
			continue;
		}
		for (int index = 0; index < volume.mappings.size(); index++) {
			Volume::Mapping &mapping = volume.mappings.write[index];
			const BakedVisibilityRuntimeCandidate *const *candidate_ptr = candidates.getptr(mapping.rid);
			const BakedVisibilityRuntimeCandidate *candidate = candidate_ptr ? *candidate_ptr : nullptr;
			if (!candidate || candidate->kind != mapping.kind || !(candidate->layer_mask & volume.bake_mask)) {
				result.fail_open = true;
				result.reason = "unresolved runtime mapping";
				return result;
			}
			if (!candidate->visible) {
				if (mapping.flags & BakedVisibilityData3DData::INSTANCE_FLAG_CERTIFIED_BLOCKER) {
					result.fail_open = true;
					result.reason = "certified blocker hidden";
					return result;
				}
				result.dynamic_count++;
				continue;
			}
			if (mapping.kind == BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY) {
				const Transform3D local_from_world = volume.world_from_local.affine_inverse();
				const AABB current_local_bounds = (local_from_world * candidate->world_transform).xform(candidate->base_aabb);
				String identity = volume.source_identity;
				if (const MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(mapping.node_id))) identity += ":" + BakedVisibilityBaker::make_geometry_identity(mesh);
				if (volume.verify_signatures && BakedVisibilityBaker::make_instance_signature(mapping.path, mapping.kind, current_local_bounds, mapping.flags, identity) != mapping.signature_sha256) {
					if (mapping.flags & BakedVisibilityData3DData::INSTANCE_FLAG_CERTIFIED_BLOCKER) {
						result.fail_open = true;
						result.reason = "certified blocker signature changed";
						return result;
					}
					result.dynamic_count++;
					continue;
				}
				if (!current_local_bounds.is_equal_approx(mapping.local_bounds)) {
					if (mapping.flags & BakedVisibilityData3DData::INSTANCE_FLAG_CERTIFIED_BLOCKER) {
						result.fail_open = true;
						result.reason = "certified blocker changed";
						return result;
					}
					result.dynamic_count++;
					continue;
				}
			}
			if (mapping.version == 0) {
				mapping.version = candidate->version;
			} else if (mapping.version != candidate->version) {
				if (mapping.flags & BakedVisibilityData3DData::INSTANCE_FLAG_CERTIFIED_BLOCKER) {
					result.fail_open = true;
					result.reason = "certified blocker version changed";
					return result;
				}
				result.dynamic_count++;
				continue;
			}
			if (candidate->dynamic) {
				result.dynamic_count++;
				continue;
			}
			result.registered_static.insert(mapping.rid);
			if (selected_primary.has(index)) result.primary.insert(mapping.rid);
			if (selected_transport.has(index)) result.transport.insert(mapping.rid);
		}
	}
	for (int i = 0; i < eye_covered.size(); i++) {
		if (!eye_covered[i]) {
			result.fail_open = true;
			result.reason = "uncovered eye or outer boundary margin";
			return result;
		}
	}
	if (result.volume_count == 0) {
		result.reason = "no volumes for scenario";
		return result;
	}
	result.active = true;
	result.primary_count = result.primary.size();
	result.transport_count = result.transport.size();
	for (const KeyValue<ObjectID, Volume> &E : volumes) {
		if (E.value.scenario_id == p_scenario_id && E.value.data.is_valid() && E.value.data->get_baked_data()) result.tile_count += E.value.data->get_baked_data()->tiles.size();
	}
	for (const BakedVisibilityRuntimeCandidate &candidate : p_candidates) {
		if (!result.registered_static.has(candidate.rid)) {
			continue;
		}
		const bool geometry = candidate.kind == BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY;
		if (geometry) {
			result.registered_static_geometry_count++;
		} else {
			result.registered_static_light_count++;
		}
		if (result.primary.has(candidate.rid)) {
			if (geometry) {
				result.primary_geometry_count++;
			} else {
				result.primary_light_count++;
			}
		}
		if (result.transport.has(candidate.rid)) {
			if (geometry) {
				result.transport_geometry_count++;
			} else {
				result.transport_light_count++;
			}
		}
	}
	result.reason = "active";
	BakedVisibilityRuntimeStats stats;
	stats.available = true;
	stats.active = true;
	stats.registered_static_geometry_count = result.registered_static_geometry_count;
	stats.tile_count = result.tile_count;
	stats.primary_geometry_count = result.primary_geometry_count;
	stats.transport_geometry_count = result.transport_geometry_count;
	for (const BakedVisibilityRuntimeCandidate &candidate : p_candidates) {
		if (candidate.kind == BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY) {
			stats.transport_geometry_eligible_count++;
		} else {
			stats.transport_light_eligible_count++;
		}
	}
	stats.transport_light_count = result.transport_light_count;
	for (const KeyValue<ObjectID, Volume> &entry : volumes) {
		if (entry.value.scenario_id == p_scenario_id) {
			stats.cached_leaf_count += entry.value.tile_cache.size();
			stats.leaf_decode_count += entry.value.tile_cache_decodes;
			stats.leaf_eviction_count += entry.value.tile_cache_evictions;
		}
	}
	stats.reason = result.reason;
	scenario_stats[p_scenario_id] = stats;
	return result;
}

void BakedVisibilityRuntime::set_last_transport_stats(uint64_t p_scenario_id, uint32_t p_geometry_count, uint32_t p_geometry_eligible_count, uint32_t p_light_count, uint32_t p_light_eligible_count) {
	MutexLock lock(*mutex);
	BakedVisibilityRuntimeStats *stats = scenario_stats.getptr(p_scenario_id);
	if (!stats) {
		return;
	}
	stats->transport_geometry_count = p_geometry_count;
	stats->transport_geometry_eligible_count = p_geometry_eligible_count;
	stats->transport_light_count = p_light_count;
	stats->transport_light_eligible_count = p_light_eligible_count;
}

BakedVisibilityRuntimeStats BakedVisibilityRuntime::get_last_query_stats(uint64_t p_scenario_id) {
	MutexLock lock(*mutex);
	const BakedVisibilityRuntimeStats *stats = scenario_stats.getptr(p_scenario_id);
	return stats ? *stats : BakedVisibilityRuntimeStats();
}
