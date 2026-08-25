/**************************************************************************/
/*  baked_visibility_runtime.h                                            */
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

#pragma once

#include "core/math/aabb.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "servers/rendering/baked_visibility/baked_visibility_codec.h"

class BakedVisibilityVolume3D;

struct BakedVisibilityRuntimeCandidate {
	uint64_t rid = 0;
	uint8_t kind = 0;
	uint32_t layer_mask = 0;
	AABB world_aabb;
	AABB base_aabb;
	Transform3D world_transform;
	uint64_t version = 0;
	bool visible = false;
	bool dynamic = false;
};

struct BakedVisibilityRuntimeResult {
	bool active = false;
	bool fail_open = false;
	uint32_t volume_count = 0;
	uint32_t cell_count = 0;
	uint32_t primary_count = 0;
	uint32_t transport_count = 0;
	uint32_t registered_static_geometry_count = 0;
	uint32_t tile_count = 0;
	uint32_t registered_static_light_count = 0;
	uint32_t primary_geometry_count = 0;
	uint32_t primary_light_count = 0;
	uint32_t transport_geometry_count = 0;
	uint32_t transport_light_count = 0;
	uint32_t dynamic_count = 0;
	float transport_distance = -1.0f;
	String reason;
	HashSet<uint64_t> registered_static;
	HashSet<uint64_t> primary;
	HashSet<uint64_t> transport;

	bool admits_primary(uint64_t p_rid) const {
		return fail_open || !registered_static.has(p_rid) || primary.has(p_rid);
	}
	bool admits_transport(uint64_t p_rid) const {
		return fail_open || !registered_static.has(p_rid) || transport.has(p_rid);
	}
};

struct BakedVisibilityRuntimeStats {
	bool available = false;
	bool active = false;
	bool fail_open = false;
	uint32_t registered_static_geometry_count = 0;
	uint32_t tile_count = 0;
	uint32_t primary_geometry_count = 0;
	uint32_t transport_geometry_count = 0;
	uint32_t transport_geometry_eligible_count = 0;
	uint32_t transport_light_count = 0;
	uint32_t transport_light_eligible_count = 0;
	uint32_t cached_leaf_count = 0;
	uint64_t leaf_decode_count = 0;
	uint64_t leaf_eviction_count = 0;
	String reason;
};

class BakedVisibilityRuntime {
	struct Volume;
	Mutex *mutex = nullptr;
	HashMap<ObjectID, Volume> volumes;
	HashMap<uint64_t, BakedVisibilityRuntimeStats> scenario_stats;
	void _register_volume(BakedVisibilityVolume3D *p_volume, bool p_test);

	BakedVisibilityRuntime();
	~BakedVisibilityRuntime();

public:
	static BakedVisibilityRuntime &get_singleton();
	void register_volume(BakedVisibilityVolume3D *p_volume);
	void register_test_volume(BakedVisibilityVolume3D *p_volume);
	void unregister_volume(ObjectID p_volume_id);
	BakedVisibilityRuntimeResult query(uint64_t p_scenario_id, const Vector<Vector3> &p_eye_origins, uint32_t p_visible_layers, const Vector<BakedVisibilityRuntimeCandidate> &p_candidates);
	void set_last_transport_stats(uint64_t p_scenario_id, uint32_t p_geometry_count, uint32_t p_geometry_eligible_count, uint32_t p_light_count, uint32_t p_light_eligible_count);
	BakedVisibilityRuntimeStats get_last_query_stats(uint64_t p_scenario_id);
};
