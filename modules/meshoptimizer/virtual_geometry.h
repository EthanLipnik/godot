/**************************************************************************/
/*  virtual_geometry.h                                                    */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_format.h"
#include "scene/resources/material.h"
#include "core/variant/typed_array.h"

class Mesh;

// Immutable portable VG1 package owner. Runtime residency is intentionally
// absent from this resource: it belongs to renderer-side VirtualGeometryStorage.
class VirtualGeometry : public Resource {
	GDCLASS(VirtualGeometry, Resource);

	RendererVirtualGeometry::Package package;
	uint64_t revision = 1;
	uint64_t package_revision = 1;
	Vector<Ref<Material>> material_bindings;
	RID virtual_geometry;

	void _material_binding_changed();
	void _update_rendering_resource(bool p_package_changed);

protected:
	static void _bind_methods();

public:
	Error set_compiled_package(const RendererVirtualGeometry::Package &p_package);
	Error set_serialized_package(const PackedByteArray &p_blob);
	PackedByteArray get_serialized_package() const;
	const RendererVirtualGeometry::Package &get_compiled_package() const { return package; }
	RID get_rid() const override { return virtual_geometry; }
	uint64_t get_revision() const { return revision; }
	uint64_t get_package_revision() const { return package_revision; }
	uint64_t get_source_asset_identity() const { return package.manifest.source_asset_identity; }
	uint64_t get_source_primitive_identity() const { return package.manifest.source_primitive_identity; }
	bool is_valid_virtual_geometry() const { return !package.manifest.pages.is_empty() && RendererVirtualGeometry::validate_package(package, true) == OK; }
	void set_material_bindings(const TypedArray<Material> &p_bindings);
	TypedArray<Material> get_material_bindings() const;
	Ref<Material> get_material_binding(uint32_t p_slot) const;
	bool has_complete_material_bindings() const;
	// Runtime-accessible compiler bridge used by procedural/import pipelines and
	// native validation fixtures. It compiles one indexed triangle surface into
	// the same portable package as the offline compiler.
	Error compile_from_mesh(const Ref<Mesh> &p_mesh, int32_t p_surface, int64_t p_source_asset_identity, int64_t p_source_primitive_identity, int64_t p_max_decoded_page_bytes = 64 * 1024);

	VirtualGeometry();
	~VirtualGeometry();
};
