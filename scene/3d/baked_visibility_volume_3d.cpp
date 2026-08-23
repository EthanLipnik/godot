/**************************************************************************/
/*  baked_visibility_volume_3d.cpp                                       */
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

#include "baked_visibility_volume_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/baked_visibility/baked_visibility_runtime.h"

void BakedVisibilityVolume3D::_refresh_runtime_registration() {
	if (is_inside_tree()) {
		BakedVisibilityRuntime::get_singleton().register_volume(this);
	}
}

void BakedVisibilityVolume3D::_data_changed() {
	_refresh_runtime_registration();
}

void BakedVisibilityVolume3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			set_notify_transform(true);
			BakedVisibilityRuntime::get_singleton().register_volume(this);
			break;
		case NOTIFICATION_TRANSFORM_CHANGED:
			BakedVisibilityRuntime::get_singleton().register_volume(this);
			break;
		case NOTIFICATION_EXIT_TREE:
			BakedVisibilityRuntime::get_singleton().unregister_volume(get_instance_id());
			break;
	}
}

void BakedVisibilityVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &BakedVisibilityVolume3D::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &BakedVisibilityVolume3D::get_data);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &BakedVisibilityVolume3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &BakedVisibilityVolume3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &BakedVisibilityVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &BakedVisibilityVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_cell_size", "cell_size"), &BakedVisibilityVolume3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &BakedVisibilityVolume3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_bake_root", "bake_root"), &BakedVisibilityVolume3D::set_bake_root);
	ClassDB::bind_method(D_METHOD("get_bake_root"), &BakedVisibilityVolume3D::get_bake_root);
	ClassDB::bind_method(D_METHOD("set_bake_mask", "bake_mask"), &BakedVisibilityVolume3D::set_bake_mask);
	ClassDB::bind_method(D_METHOD("get_bake_mask"), &BakedVisibilityVolume3D::get_bake_mask);
	ClassDB::bind_method(D_METHOD("set_transport_distance", "distance"), &BakedVisibilityVolume3D::set_transport_distance);
	ClassDB::bind_method(D_METHOD("get_transport_distance"), &BakedVisibilityVolume3D::get_transport_distance);
	ClassDB::bind_method(D_METHOD("set_lookup_margin", "margin"), &BakedVisibilityVolume3D::set_lookup_margin);
	ClassDB::bind_method(D_METHOD("get_lookup_margin"), &BakedVisibilityVolume3D::get_lookup_margin);
	ClassDB::bind_method(D_METHOD("set_max_cells", "max_cells"), &BakedVisibilityVolume3D::set_max_cells);
	ClassDB::bind_method(D_METHOD("get_max_cells"), &BakedVisibilityVolume3D::get_max_cells);
	ClassDB::bind_method(D_METHOD("set_max_work_units_per_cell", "max_work_units_per_cell"), &BakedVisibilityVolume3D::set_max_work_units_per_cell);
	ClassDB::bind_method(D_METHOD("get_max_work_units_per_cell"), &BakedVisibilityVolume3D::get_max_work_units_per_cell);
	ClassDB::bind_method(D_METHOD("set_max_blocker_triangles", "max_blocker_triangles"), &BakedVisibilityVolume3D::set_max_blocker_triangles);
	ClassDB::bind_method(D_METHOD("get_max_blocker_triangles"), &BakedVisibilityVolume3D::get_max_blocker_triangles);
	ClassDB::bind_method(D_METHOD("set_max_output_bytes", "max_output_bytes"), &BakedVisibilityVolume3D::set_max_output_bytes);
	ClassDB::bind_method(D_METHOD("get_max_output_bytes"), &BakedVisibilityVolume3D::get_max_output_bytes);
	ClassDB::bind_method(D_METHOD("get_runtime_stats"), &BakedVisibilityVolume3D::get_runtime_stats);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "BakedVisibilityData3D"), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_LINK), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "cell_size", PROPERTY_HINT_LINK), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "bake_root", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_bake_root", "get_bake_root");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bake_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_bake_mask", "get_bake_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transport_distance", PROPERTY_HINT_RANGE, "-1,10000,0.1,or_greater"), "set_transport_distance", "get_transport_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lookup_margin", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater"), "set_lookup_margin", "get_lookup_margin");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cells", PROPERTY_HINT_RANGE, "1,65536,1"), "set_max_cells", "get_max_cells");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_work_units_per_cell", PROPERTY_HINT_RANGE, "1,65536,1"), "set_max_work_units_per_cell", "get_max_work_units_per_cell");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_blocker_triangles", PROPERTY_HINT_RANGE, "0,1048576,1"), "set_max_blocker_triangles", "get_max_blocker_triangles");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_output_bytes", PROPERTY_HINT_RANGE, "1,536870912,1"), "set_max_output_bytes", "get_max_output_bytes");
}

void BakedVisibilityVolume3D::set_data(const Ref<BakedVisibilityData3D> &p_data) {
	if (data.is_valid()) {
		data->disconnect_changed(callable_mp(this, &BakedVisibilityVolume3D::_data_changed));
	}
	data = p_data;
	if (data.is_valid()) {
		data->connect_changed(callable_mp(this, &BakedVisibilityVolume3D::_data_changed));
	}
	_refresh_runtime_registration();
}
Ref<BakedVisibilityData3D> BakedVisibilityVolume3D::get_data() const {
	return data;
}
void BakedVisibilityVolume3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	_refresh_runtime_registration();
}
bool BakedVisibilityVolume3D::is_enabled() const {
	return enabled;
}
void BakedVisibilityVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.max(Vector3());
	_refresh_runtime_registration();
}
Vector3 BakedVisibilityVolume3D::get_size() const {
	return size;
}
void BakedVisibilityVolume3D::set_cell_size(const Vector3 &p_cell_size) {
	cell_size = p_cell_size.max(Vector3(0.001f, 0.001f, 0.001f));
	_refresh_runtime_registration();
}
Vector3 BakedVisibilityVolume3D::get_cell_size() const {
	return cell_size;
}
void BakedVisibilityVolume3D::set_bake_root(const NodePath &p_bake_root) {
	bake_root = p_bake_root;
	_refresh_runtime_registration();
}
NodePath BakedVisibilityVolume3D::get_bake_root() const {
	return bake_root;
}
void BakedVisibilityVolume3D::set_bake_mask(uint32_t p_bake_mask) {
	bake_mask = p_bake_mask;
	_refresh_runtime_registration();
}
uint32_t BakedVisibilityVolume3D::get_bake_mask() const {
	return bake_mask;
}
void BakedVisibilityVolume3D::set_transport_distance(float p_distance) {
	transport_distance = MAX(-1.0f, p_distance);
	_refresh_runtime_registration();
}
float BakedVisibilityVolume3D::get_transport_distance() const {
	return transport_distance;
}
void BakedVisibilityVolume3D::set_lookup_margin(float p_margin) {
	lookup_margin = MAX(0.0f, p_margin);
	_refresh_runtime_registration();
}
float BakedVisibilityVolume3D::get_lookup_margin() const {
	return lookup_margin;
}
void BakedVisibilityVolume3D::set_max_cells(int p_max_cells) {
	max_cells = CLAMP(p_max_cells, 1, 65536);
}
int BakedVisibilityVolume3D::get_max_cells() const {
	return max_cells;
}
void BakedVisibilityVolume3D::set_max_work_units_per_cell(int p_max_work_units_per_cell) {
	max_work_units_per_cell = CLAMP(p_max_work_units_per_cell, 1, 65536);
}
int BakedVisibilityVolume3D::get_max_work_units_per_cell() const {
	return max_work_units_per_cell;
}
void BakedVisibilityVolume3D::set_max_blocker_triangles(int p_max_blocker_triangles) {
	max_blocker_triangles = CLAMP(p_max_blocker_triangles, 0, 1048576);
}
int BakedVisibilityVolume3D::get_max_blocker_triangles() const {
	return max_blocker_triangles;
}
void BakedVisibilityVolume3D::set_max_output_bytes(int p_max_output_bytes) {
	max_output_bytes = CLAMP(p_max_output_bytes, 1, 512 * 1024 * 1024);
}
int BakedVisibilityVolume3D::get_max_output_bytes() const {
	return max_output_bytes;
}
Dictionary BakedVisibilityVolume3D::get_runtime_stats() const {
	Dictionary result;
	uint64_t scenario_id = 0;
	if (is_inside_tree()) {
		Ref<World3D> world = get_world_3d();
		scenario_id = world.is_valid() ? world->get_scenario().get_id() : 0;
	}
	const BakedVisibilityRuntimeStats stats = BakedVisibilityRuntime::get_singleton().get_last_query_stats(scenario_id);
	result["available"] = stats.available;
	result["active"] = stats.active;
	result["fail_open"] = stats.fail_open;
	result["registered_static_geometry"] = stats.registered_static_geometry_count;
	result["primary_geometry"] = stats.primary_geometry_count;
	result["transport_geometry"] = stats.transport_geometry_count;
	result["transport_geometry_eligible"] = stats.transport_geometry_eligible_count;
	result["transport_lights"] = stats.transport_light_count;
	result["transport_lights_eligible"] = stats.transport_light_eligible_count;
	result["reason"] = stats.reason;
	return result;
}
