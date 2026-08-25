/**************************************************************************/
/*  virtual_geometry_instance_3d.cpp                                      */
/**************************************************************************/

#include "virtual_geometry_instance_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

void VirtualGeometryInstance3D::_resource_changed() {
	instance_revision++;
	update_gizmos();
	notify_property_list_changed();
}

AABB VirtualGeometryInstance3D::get_aabb() const {
	return virtual_geometry.is_valid() ? virtual_geometry->get_compiled_package().manifest.resource_bounds : AABB();
}

void VirtualGeometryInstance3D::set_virtual_geometry(const Ref<VirtualGeometry> &p_geometry) {
	if (virtual_geometry == p_geometry) return;
	if (virtual_geometry.is_valid()) virtual_geometry->disconnect_changed(callable_mp(this, &VirtualGeometryInstance3D::_resource_changed));
	virtual_geometry = p_geometry;
	if (virtual_geometry.is_valid()) virtual_geometry->connect_changed(callable_mp(this, &VirtualGeometryInstance3D::_resource_changed));
	set_base(virtual_geometry.is_valid() ? virtual_geometry->get_rid() : RID());
	_resource_changed();
}

void VirtualGeometryInstance3D::set_virtual_geometry_visibility_layer(uint32_t p_layer) {
	if (visibility_layer == p_layer) return;
	visibility_layer = p_layer;
	set_layer_mask(p_layer);
	instance_revision++;
}

void VirtualGeometryInstance3D::set_semantic_instance_id(int64_t p_id) {
	ERR_FAIL_COND_MSG(p_id < 0, "Virtual geometry semantic IDs must be non-negative signed 64-bit values.");
	if (semantic_instance_id == p_id) return;
	semantic_instance_id = p_id;
	RenderingServer::get_singleton()->instance_set_semantic_id(get_instance(), semantic_instance_id);
	instance_revision++;
}

void VirtualGeometryInstance3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_virtual_geometry", "geometry"), &VirtualGeometryInstance3D::set_virtual_geometry);
	ClassDB::bind_method(D_METHOD("get_virtual_geometry"), &VirtualGeometryInstance3D::get_virtual_geometry);
	ClassDB::bind_method(D_METHOD("get_instance_revision"), &VirtualGeometryInstance3D::get_instance_revision);
	ClassDB::bind_method(D_METHOD("get_resource_revision"), &VirtualGeometryInstance3D::get_resource_revision);
	ClassDB::bind_method(D_METHOD("set_semantic_instance_id", "id"), &VirtualGeometryInstance3D::set_semantic_instance_id);
	ClassDB::bind_method(D_METHOD("get_semantic_instance_id"), &VirtualGeometryInstance3D::get_semantic_instance_id);
	ClassDB::bind_method(D_METHOD("set_virtual_geometry_visibility_layer", "layer"), &VirtualGeometryInstance3D::set_virtual_geometry_visibility_layer);
	ClassDB::bind_method(D_METHOD("get_virtual_geometry_visibility_layer"), &VirtualGeometryInstance3D::get_virtual_geometry_visibility_layer);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "virtual_geometry", PROPERTY_HINT_RESOURCE_TYPE, "VirtualGeometry"), "set_virtual_geometry", "get_virtual_geometry");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "semantic_instance_id", PROPERTY_HINT_RANGE, "0,9223372036854775807,1,or_greater"), "set_semantic_instance_id", "get_semantic_instance_id");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "virtual_geometry_visibility_layer", PROPERTY_HINT_LAYERS_3D_RENDER), "set_virtual_geometry_visibility_layer", "get_virtual_geometry_visibility_layer");
}
