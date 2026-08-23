/**************************************************************************/
/*  environment_portal_runtime.cpp                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                                                                        */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                               */
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

#include "environment_portal_runtime.h"

#include "core/math/math_funcs.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/environment_portal_3d.h"
#include "scene/resources/3d/world_3d.h"

#include <cstring>

namespace RendererPathTracing {

struct EnvironmentPortalRuntime::Portal {
	uint64_t scenario_id = 0;
	EnvironmentPortal portal;
	uint32_t cull_mask = 0;
	bool enabled = false;
};

namespace {

static uint64_t _hash_float(float p_value, uint64_t p_hash) {
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(p_value));
	memcpy(&bits, &p_value, sizeof(bits));
	return hash_djb2_one_64(bits, p_hash);
}

static uint64_t _hash_vector3(const Vector3 &p_value, uint64_t p_hash) {
	p_hash = _hash_float(p_value.x, p_hash);
	p_hash = _hash_float(p_value.y, p_hash);
	return _hash_float(p_value.z, p_hash);
}

static uint64_t _portal_generation(const Vector<EnvironmentPortal> &p_portals) {
	uint64_t hash = hash_djb2_one_64(p_portals.size());
	for (const EnvironmentPortal &portal : p_portals) {
		hash = hash_djb2_one_64(portal.portal_id, hash);
		hash = _hash_vector3(portal.center, hash);
		hash = _hash_vector3(portal.axis_u, hash);
		hash = _hash_vector3(portal.axis_v, hash);
		hash = _hash_float(portal.weight, hash);
	}
	return hash == 0 ? 1 : hash;
}

} // namespace

EnvironmentPortalRuntime::EnvironmentPortalRuntime() {
	mutex = memnew(Mutex);
}

EnvironmentPortalRuntime::~EnvironmentPortalRuntime() {
	memdelete(mutex);
}

EnvironmentPortalRuntime &EnvironmentPortalRuntime::get_singleton() {
	static EnvironmentPortalRuntime singleton;
	return singleton;
}

void EnvironmentPortalRuntime::register_portal(EnvironmentPortal3D *p_portal) {
	ERR_FAIL_NULL(p_portal);
	Portal snapshot;
	snapshot.portal.portal_id = p_portal->get_instance_id();
	snapshot.portal.weight = p_portal->get_proposal_weight();
	snapshot.enabled = p_portal->is_enabled();
	snapshot.cull_mask = p_portal->get_cull_mask();
	if (p_portal->is_inside_tree()) {
		const Ref<World3D> world = p_portal->get_world_3d();
		snapshot.scenario_id = world.is_valid() ? world->get_scenario().get_id() : 0;
	}
	const Transform3D transform = p_portal->is_inside_tree() ? p_portal->get_global_transform() : p_portal->get_transform();
	const Vector2 size = p_portal->get_size().maxf(0.0f);
	snapshot.portal.center = transform.origin;
	snapshot.portal.axis_u = transform.basis.xform(Vector3(size.x * 0.5f, 0.0f, 0.0f));
	snapshot.portal.axis_v = transform.basis.xform(Vector3(0.0f, size.y * 0.5f, 0.0f));
	if (!snapshot.portal.center.is_finite() || !snapshot.portal.axis_u.is_finite() || !snapshot.portal.axis_v.is_finite() || !Math::is_finite(snapshot.portal.weight) || snapshot.portal.weight < 0.0f || snapshot.portal.axis_u.cross(snapshot.portal.axis_v).length_squared() <= CMP_EPSILON2) {
		snapshot.enabled = false;
	}
	MutexLock lock(*mutex);
	portals[p_portal->get_instance_id()] = snapshot;
}

void EnvironmentPortalRuntime::register_test_portal(EnvironmentPortal3D *p_portal, uint64_t p_scenario_id) {
	register_portal(p_portal);
	MutexLock lock(*mutex);
	Portal *snapshot = portals.getptr(p_portal->get_instance_id());
	ERR_FAIL_NULL(snapshot);
	snapshot->scenario_id = p_scenario_id;
}

void EnvironmentPortalRuntime::unregister_portal(ObjectID p_portal_id) {
	MutexLock lock(*mutex);
	portals.erase(p_portal_id);
}

EnvironmentPortalRuntimeResult EnvironmentPortalRuntime::query(uint64_t p_scenario_id, uint32_t p_visible_layers) const {
	EnvironmentPortalRuntimeResult result;
	MutexLock lock(*mutex);
	for (const KeyValue<ObjectID, Portal> &entry : portals) {
		const Portal &snapshot = entry.value;
		if (snapshot.scenario_id != p_scenario_id) {
			continue;
		}
		result.registered_count++;
		if (!snapshot.enabled || snapshot.portal.weight <= 0.0f || !(snapshot.cull_mask & p_visible_layers)) {
			continue;
		}
		result.portals.push_back(snapshot.portal);
	}
	// HashMap traversal order is deliberately unspecified. Preserve stable source
	// ordering so a scene that did not change cannot reset temporal histories.
	for (int i = 1; i < result.portals.size(); i++) {
		EnvironmentPortal value = result.portals[i];
		int insertion = i;
		while (insertion > 0 && value.portal_id < result.portals[insertion - 1].portal_id) {
			result.portals.write[insertion] = result.portals[insertion - 1];
			insertion--;
		}
		result.portals.write[insertion] = value;
	}
	result.selected_count = result.portals.size();
	result.generation = _portal_generation(result.portals);
	return result;
}

} // namespace RendererPathTracing
