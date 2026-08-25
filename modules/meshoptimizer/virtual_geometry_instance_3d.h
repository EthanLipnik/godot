/**************************************************************************/
/*  virtual_geometry_instance_3d.h                                        */
/**************************************************************************/

#pragma once

#include "scene/3d/visual_instance_3d.h"
#include "virtual_geometry.h"

// Exactly one scene node per virtual geometry instance. It never creates page
// or cluster children; renderer integration consumes resource/instance IDs.
class VirtualGeometryInstance3D : public VisualInstance3D {
	GDCLASS(VirtualGeometryInstance3D, VisualInstance3D);

	Ref<VirtualGeometry> virtual_geometry;
	uint64_t instance_revision = 1;
	int64_t semantic_instance_id = 0;
	uint32_t visibility_layer = 1;

	void _resource_changed();

protected:
	static void _bind_methods();
	virtual AABB get_aabb() const override;

public:
	void set_virtual_geometry(const Ref<VirtualGeometry> &p_geometry);
	Ref<VirtualGeometry> get_virtual_geometry() const { return virtual_geometry; }
	uint64_t get_instance_revision() const { return instance_revision; }
	uint64_t get_resource_revision() const { return virtual_geometry.is_valid() ? virtual_geometry->get_revision() : 0; }
	void set_semantic_instance_id(int64_t p_id);
	int64_t get_semantic_instance_id() const { return semantic_instance_id; }
	void set_virtual_geometry_visibility_layer(uint32_t p_layer);
	uint32_t get_virtual_geometry_visibility_layer() const { return visibility_layer; }
};
