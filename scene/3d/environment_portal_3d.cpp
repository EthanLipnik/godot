/**************************************************************************/
/*  environment_portal_3d.cpp                                             */
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

#include "environment_portal_3d.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "servers/rendering/path_tracing/environment_portal_runtime.h"

void EnvironmentPortal3D::_refresh_runtime_registration() {
	if (is_inside_tree()) {
		RendererPathTracing::EnvironmentPortalRuntime::get_singleton().register_portal(this);
	}
}

void EnvironmentPortal3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			set_notify_transform(true);
			_refresh_runtime_registration();
			break;
		case NOTIFICATION_TRANSFORM_CHANGED:
			_refresh_runtime_registration();
			break;
		case NOTIFICATION_EXIT_TREE:
			RendererPathTracing::EnvironmentPortalRuntime::get_singleton().unregister_portal(get_instance_id());
			break;
	}
}

void EnvironmentPortal3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &EnvironmentPortal3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &EnvironmentPortal3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &EnvironmentPortal3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &EnvironmentPortal3D::get_size);
	ClassDB::bind_method(D_METHOD("set_proposal_weight", "weight"), &EnvironmentPortal3D::set_proposal_weight);
	ClassDB::bind_method(D_METHOD("get_proposal_weight"), &EnvironmentPortal3D::get_proposal_weight);
	ClassDB::bind_method(D_METHOD("set_cull_mask", "mask"), &EnvironmentPortal3D::set_cull_mask);
	ClassDB::bind_method(D_METHOD("get_cull_mask"), &EnvironmentPortal3D::get_cull_mask);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_LINK), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "proposal_weight", PROPERTY_HINT_RANGE, "0,1000,0.01,or_greater"), "set_proposal_weight", "get_proposal_weight");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_cull_mask", "get_cull_mask");
}

void EnvironmentPortal3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	_refresh_runtime_registration();
}

bool EnvironmentPortal3D::is_enabled() const {
	return enabled;
}

void EnvironmentPortal3D::set_size(const Vector2 &p_size) {
	size = p_size.maxf(0.0f);
	_refresh_runtime_registration();
}

Vector2 EnvironmentPortal3D::get_size() const {
	return size;
}

void EnvironmentPortal3D::set_proposal_weight(float p_weight) {
	proposal_weight = Math::is_finite(p_weight) ? MAX(0.0f, p_weight) : 0.0f;
	_refresh_runtime_registration();
}

float EnvironmentPortal3D::get_proposal_weight() const {
	return proposal_weight;
}

void EnvironmentPortal3D::set_cull_mask(uint32_t p_cull_mask) {
	cull_mask = p_cull_mask;
	_refresh_runtime_registration();
}

uint32_t EnvironmentPortal3D::get_cull_mask() const {
	return cull_mask;
}
