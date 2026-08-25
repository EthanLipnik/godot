/**************************************************************************/
/*  virtual_geometry.cpp                                                  */
/**************************************************************************/

#include "virtual_geometry.h"
#include "virtual_geometry_compiler.h"

#include "core/object/class_db.h"
#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"
#include "scene/resources/mesh.h"

#include <cstring>

namespace {

static constexpr uint32_t VG_SERIALIZATION_VERSION = 1;
static constexpr uint32_t VG_SERIALIZATION_MAX_VECTOR = 1000000;
static const uint8_t VG_SERIALIZATION_MAGIC[8] = { 'V', 'G', '1', 'P', 'K', 'G', 0, 1 };

class VGBlobWriter {
	PackedByteArray bytes;

	void u8(uint8_t p_value) { bytes.push_back(p_value); }
	void u32(uint32_t p_value) { for (int i = 0; i < 4; i++) u8(uint8_t(p_value >> (i * 8))); }
	void u64(uint64_t p_value) { for (int i = 0; i < 8; i++) u8(uint8_t(p_value >> (i * 8))); }
	void f64(double p_value) { uint64_t bits = 0; memcpy(&bits, &p_value, sizeof(bits)); u64(bits); }
	void vec3(const Vector3 &p_value) { f64(p_value.x); f64(p_value.y); f64(p_value.z); }
	void aabb(const AABB &p_value) { vec3(p_value.position); vec3(p_value.size); }
	void u64_vector(const Vector<uint64_t> &p_values) { u32(uint32_t(p_values.size())); for (uint64_t value : p_values) u64(value); }
	void string(const String &p_value) {
		const Vector<uint8_t> utf8 = p_value.to_utf8_buffer();
		u32(uint32_t(utf8.size()));
		if (!utf8.is_empty()) bytes.append_array(utf8);
	}

public:
	void write_package(const RendererVirtualGeometry::Package &p_package, uint64_t p_package_revision) {
		for (uint8_t value : VG_SERIALIZATION_MAGIC) u8(value);
		u32(VG_SERIALIZATION_VERSION);
		u64(p_package_revision);
		const RendererVirtualGeometry::Manifest &m = p_package.manifest;
		u32(m.format_version); u32(m.page_payload_version); u32(m.compiler_semantic_generation); u32(m.compression_generation); u32(m.material_schema_version); u32(m.ray_hint_schema_version);
		u64(m.source_asset_identity); u64(m.source_primitive_identity); u64(m.source_digest); aabb(m.resource_bounds); u64(m.build_settings_hash);
		u32(uint32_t(m.stream_schemas.size()));
		for (const auto &s : m.stream_schemas) { u64(s.stable_id); u32(s.flags); u32(s.revision); }
		u32(uint32_t(m.materials.size()));
		for (const auto &material : m.materials) { u64(material.semantic_id); u32(material.closure_class); u32(material.revision); }
		u32(uint32_t(m.clusters.size()));
		for (const auto &c : m.clusters) {
			u64(c.stable_id); u64(c.topology_revision); u64(c.attribute_revision); u64(c.material_semantic_revision); u64(c.page_id); u64(c.payload_offset); u64(c.payload_size);
			u32(c.vertex_count); u32(c.triangle_count); u64(c.stream_schema_id); u32(c.material_slot); aabb(c.bounds); vec3(c.sphere_center); f64(c.sphere_radius); vec3(c.position_decode_scale); vec3(c.position_decode_bias); u64(c.feature_remap_offset); u32(c.feature_remap_count); u32(c.flags);
		}
		u32(uint32_t(m.groups.size()));
		for (const auto &g : m.groups) { u64(g.stable_id); u64(g.revision); u64_vector(g.coarse_cluster_ids); u64_vector(g.fine_cluster_ids); u64_vector(g.parent_group_ids); u64_vector(g.child_group_ids); aabb(g.bounds); f64(g.geometric_error); f64(g.attribute_error); u64_vector(g.atomic_dependencies); u8(g.persistent_root ? 1 : 0); }
		u64_vector(m.root_group_ids);
		u32(uint32_t(m.ray_groups.size()));
		for (const auto &g : m.ray_groups) { u64(g.stable_id); u64(g.transport_region_id); u64(g.revision); u8(uint8_t(g.tier)); u64_vector(g.cluster_ids); aabb(g.bounds); u8(g.persistent_coarse ? 1 : 0); u8(g.fixed_topology_refit ? 1 : 0); }
		u32(uint32_t(m.pages.size()));
		for (int i = 0; i < m.pages.size(); i++) {
			const auto &p = m.pages[i];
			u64(p.stable_id); u64(p.content_hash); u64(p.compressed_size); u64(p.decoded_size); u64(p.file_offset); u64(p.file_size); u32(p.compression_scheme); u32(p.compression_generation); u64_vector(p.cluster_ids); u64_vector(p.required_parent_or_root_pages); u64_vector(p.material_dependency_hashes); u32(p.priority_class); u8(p.persistent ? 1 : 0);
			const PackedByteArray &payload = i < p_package.compressed_pages.size() ? p_package.compressed_pages[i] : PackedByteArray();
			u64(uint64_t(payload.size()));
			if (!payload.is_empty()) bytes.append_array(payload);
		}
		u32(uint32_t(m.dependencies.size()));
		for (const auto &d : m.dependencies) { u64(d.source_identity); u64(d.source_digest); u64(d.compiler_settings_hash); u64(d.dependency_hash); }
		u32(uint32_t(m.conventional_path_diagnostics.size()));
		for (const String &diagnostic : m.conventional_path_diagnostics) string(diagnostic);
	}
	PackedByteArray finish() const { return bytes; }
};

class VGBlobReader {
	const PackedByteArray &bytes;
	int64_t offset = 0;
	bool valid = true;

	bool take(uint64_t p_size, const uint8_t *&r_data) {
		if (!valid || p_size > uint64_t(bytes.size()) || uint64_t(offset) > uint64_t(bytes.size()) - p_size) { valid = false; return false; }
		r_data = bytes.ptr() + offset; offset += int64_t(p_size); return true;
	}
	uint8_t u8() { const uint8_t *p = nullptr; if (!take(1, p)) return 0; return p[0]; }
	uint32_t u32() { const uint8_t *p = nullptr; if (!take(4, p)) return 0; uint32_t value = 0; for (int i = 0; i < 4; i++) value |= uint32_t(p[i]) << (i * 8); return value; }
	uint64_t u64() { const uint8_t *p = nullptr; if (!take(8, p)) return 0; uint64_t value = 0; for (int i = 0; i < 8; i++) value |= uint64_t(p[i]) << (i * 8); return value; }
	double f64() { const uint64_t bits = u64(); double value = 0.0; memcpy(&value, &bits, sizeof(value)); return value; }
	Vector3 vec3() { return Vector3(real_t(f64()), real_t(f64()), real_t(f64())); }
	AABB aabb() { return AABB(vec3(), vec3()); }
	bool boolean(bool &r_value) { const uint8_t value = u8(); if (!valid || value > 1) { valid = false; return false; } r_value = value != 0; return true; }
	bool count(uint32_t &r_count) { r_count = u32(); return valid && r_count <= VG_SERIALIZATION_MAX_VECTOR; }
	bool u64_vector(Vector<uint64_t> &r_values) { uint32_t count_value = 0; if (!count(count_value)) return false; r_values.resize(count_value); for (uint32_t i = 0; i < count_value; i++) r_values.write[i] = u64(); return valid; }
	String string() { uint32_t length = 0; if (!count(length)) return String(); const uint8_t *p = nullptr; if (!take(length, p)) return String(); return String::utf8(reinterpret_cast<const char *>(p), length); }

public:
	explicit VGBlobReader(const PackedByteArray &p_bytes) : bytes(p_bytes) {}
	Error read_package(RendererVirtualGeometry::Package &r_package, uint64_t &r_package_revision) {
		const uint8_t *magic = nullptr;
		if (!take(sizeof(VG_SERIALIZATION_MAGIC), magic) || memcmp(magic, VG_SERIALIZATION_MAGIC, sizeof(VG_SERIALIZATION_MAGIC)) != 0) return ERR_FILE_UNRECOGNIZED;
		if (u32() != VG_SERIALIZATION_VERSION) return ERR_UNAVAILABLE;
		r_package_revision = u64();
		if (r_package_revision == 0) return ERR_INVALID_DATA;
		RendererVirtualGeometry::Manifest &m = r_package.manifest;
		m.format_version = u32(); m.page_payload_version = u32(); m.compiler_semantic_generation = u32(); m.compression_generation = u32(); m.material_schema_version = u32(); m.ray_hint_schema_version = u32();
		m.source_asset_identity = u64(); m.source_primitive_identity = u64(); m.source_digest = u64(); m.resource_bounds = aabb(); m.build_settings_hash = u64();
		uint32_t count_value = 0;
		if (!count(count_value)) return ERR_INVALID_DATA; m.stream_schemas.resize(count_value); for (auto &s : m.stream_schemas) { s.stable_id = u64(); s.flags = u32(); s.revision = u32(); }
		if (!count(count_value)) return ERR_INVALID_DATA; m.materials.resize(count_value); for (auto &material : m.materials) { material.semantic_id = u64(); material.closure_class = u32(); material.revision = u32(); }
		if (!count(count_value)) return ERR_INVALID_DATA; m.clusters.resize(count_value); for (auto &c : m.clusters) { c.stable_id = u64(); c.topology_revision = u64(); c.attribute_revision = u64(); c.material_semantic_revision = u64(); c.page_id = u64(); c.payload_offset = u64(); c.payload_size = u64(); c.vertex_count = u32(); c.triangle_count = u32(); c.stream_schema_id = u64(); c.material_slot = u32(); c.bounds = aabb(); c.sphere_center = vec3(); c.sphere_radius = real_t(f64()); c.position_decode_scale = vec3(); c.position_decode_bias = vec3(); c.feature_remap_offset = u64(); c.feature_remap_count = u32(); c.flags = u32(); }
		if (!count(count_value)) return ERR_INVALID_DATA; m.groups.resize(count_value); for (auto &g : m.groups) { g.stable_id = u64(); g.revision = u64(); if (!u64_vector(g.coarse_cluster_ids) || !u64_vector(g.fine_cluster_ids) || !u64_vector(g.parent_group_ids) || !u64_vector(g.child_group_ids)) return ERR_INVALID_DATA; g.bounds = aabb(); g.geometric_error = real_t(f64()); g.attribute_error = real_t(f64()); if (!u64_vector(g.atomic_dependencies) || !boolean(g.persistent_root)) return ERR_INVALID_DATA; }
		if (!u64_vector(m.root_group_ids)) return ERR_INVALID_DATA;
		if (!count(count_value)) return ERR_INVALID_DATA; m.ray_groups.resize(count_value); for (auto &g : m.ray_groups) { g.stable_id = u64(); g.transport_region_id = u64(); g.revision = u64(); g.tier = RendererVirtualGeometry::RayTransportTier(u8()); if (!u64_vector(g.cluster_ids)) return ERR_INVALID_DATA; g.bounds = aabb(); if (!boolean(g.persistent_coarse) || !boolean(g.fixed_topology_refit)) return ERR_INVALID_DATA; }
		if (!count(count_value)) return ERR_INVALID_DATA; m.pages.resize(count_value); r_package.compressed_pages.resize(count_value); for (uint32_t i = 0; i < count_value; i++) { auto &p = m.pages.write[i]; p.stable_id = u64(); p.content_hash = u64(); p.compressed_size = u64(); p.decoded_size = u64(); p.file_offset = u64(); p.file_size = u64(); p.compression_scheme = u32(); p.compression_generation = u32(); if (!u64_vector(p.cluster_ids) || !u64_vector(p.required_parent_or_root_pages) || !u64_vector(p.material_dependency_hashes)) return ERR_INVALID_DATA; p.priority_class = u32(); if (!boolean(p.persistent)) return ERR_INVALID_DATA; const uint64_t payload_size = u64(); if (payload_size > uint64_t(INT32_MAX)) return ERR_OUT_OF_MEMORY; const uint8_t *payload = nullptr; if (!take(payload_size, payload)) return ERR_FILE_CORRUPT; PackedByteArray &out = r_package.compressed_pages.write[i]; if (out.resize(int64_t(payload_size)) != OK) return ERR_OUT_OF_MEMORY; if (payload_size) memcpy(out.ptrw(), payload, payload_size); }
		if (!count(count_value)) return ERR_INVALID_DATA; m.dependencies.resize(count_value); for (auto &d : m.dependencies) { d.source_identity = u64(); d.source_digest = u64(); d.compiler_settings_hash = u64(); d.dependency_hash = u64(); }
		if (!count(count_value)) return ERR_INVALID_DATA; m.conventional_path_diagnostics.resize(count_value); for (String &diagnostic : m.conventional_path_diagnostics) diagnostic = string();
		if (!valid || offset != bytes.size()) return ERR_FILE_CORRUPT;
		return RendererVirtualGeometry::validate_package(r_package, true);
	}
};

} // namespace

void VirtualGeometry::_update_rendering_resource(bool p_package_changed) {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	if (p_package_changed && !package.manifest.pages.is_empty()) {
		ERR_FAIL_COND_MSG(RenderingServer::get_singleton()->virtual_geometry_set_package(virtual_geometry, package, revision) != OK, "Virtual geometry resource rejected its validated package.");
	}

	Vector<RID> material_rids;
	material_rids.reserve(material_bindings.size());
	for (const Ref<Material> &material : material_bindings) {
		material_rids.push_back(material.is_valid() ? material->get_rid() : RID());
	}
	RenderingServer::get_singleton()->virtual_geometry_set_material_bindings(virtual_geometry, material_rids, revision);
}

void VirtualGeometry::_material_binding_changed() {
	revision++;
	_update_rendering_resource(false);
	emit_changed();
}

Error VirtualGeometry::set_compiled_package(const RendererVirtualGeometry::Package &p_package) {
	ERR_FAIL_COND_V(RendererVirtualGeometry::validate_package(p_package, true) != OK, ERR_INVALID_DATA);
	package = p_package;
	package_revision++;
	revision++;
	_update_rendering_resource(true);
	emit_changed();
	return OK;
}

Error VirtualGeometry::set_serialized_package(const PackedByteArray &p_blob) {
	if (p_blob.is_empty()) {
		// Empty is the explicit legacy/no-package state. It is not a valid VG
		// package, but it remains safe to load so old resources do not crash.
		package = RendererVirtualGeometry::Package();
		package_revision = 1;
		revision++;
		_update_rendering_resource(false);
		emit_changed();
		return OK;
	}
	RendererVirtualGeometry::Package decoded;
	uint64_t decoded_revision = 0;
	VGBlobReader reader(p_blob);
	const Error error = reader.read_package(decoded, decoded_revision);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Virtual geometry compiled package blob is corrupt, truncated, or unsupported.");
	// Commit only after decoding and complete payload validation. Runtime
	// residency is deliberately absent from the blob and is rebuilt by storage.
	package = decoded;
	package_revision = decoded_revision;
	revision++;
	_update_rendering_resource(true);
	emit_changed();
	return OK;
}

PackedByteArray VirtualGeometry::get_serialized_package() const {
	if (package.manifest.pages.is_empty() || RendererVirtualGeometry::validate_package(package, true) != OK) {
		return PackedByteArray();
	}
	VGBlobWriter writer;
	writer.write_package(package, package_revision);
	return writer.finish();
}

void VirtualGeometry::set_material_bindings(const TypedArray<Material> &p_bindings) {
	const Callable changed = callable_mp(this, &VirtualGeometry::_material_binding_changed);
	for (const Ref<Material> &binding : material_bindings) {
		if (binding.is_valid() && binding->is_connected(SNAME("changed"), changed)) {
			binding->disconnect_changed(changed);
		}
	}
	material_bindings.clear();
	for (int i = 0; i < p_bindings.size(); i++) {
		Ref<Material> binding = p_bindings[i];
		material_bindings.push_back(binding);
		if (binding.is_valid() && !binding->is_connected(SNAME("changed"), changed)) {
			binding->connect_changed(changed);
		}
	}
	revision++;
	_update_rendering_resource(false);
	emit_changed();
}

TypedArray<Material> VirtualGeometry::get_material_bindings() const {
	TypedArray<Material> result;
	for (const Ref<Material> &binding : material_bindings) result.push_back(binding);
	return result;
}

Ref<Material> VirtualGeometry::get_material_binding(uint32_t p_slot) const { return p_slot < uint32_t(material_bindings.size()) ? material_bindings[p_slot] : Ref<Material>(); }
bool VirtualGeometry::has_complete_material_bindings() const { if (package.manifest.materials.is_empty()) return true; if (material_bindings.size() != package.manifest.materials.size()) return false; for (const Ref<Material> &binding : material_bindings) if (binding.is_null()) return false; return true; }

Error VirtualGeometry::compile_from_mesh(const Ref<Mesh> &p_mesh, int32_t p_surface, int64_t p_source_asset_identity, int64_t p_source_primitive_identity, int64_t p_max_decoded_page_bytes) {
	ERR_FAIL_COND_V(p_mesh.is_null() || p_surface < 0 || p_surface >= p_mesh->get_surface_count(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_mesh->surface_get_primitive_type(p_surface) != Mesh::PRIMITIVE_TRIANGLES, ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(p_source_asset_identity <= 0 || p_source_primitive_identity <= 0 || p_max_decoded_page_bytes < 256, ERR_INVALID_PARAMETER);
	const Array arrays = p_mesh->surface_get_arrays(p_surface);
	ERR_FAIL_COND_V(arrays.size() != Mesh::ARRAY_MAX, ERR_INVALID_DATA);
	VirtualGeometryCompiler::Input input;
	input.source_asset_identity = uint64_t(p_source_asset_identity);
	input.source_primitive_identity = uint64_t(p_source_primitive_identity);
	input.positions = arrays[Mesh::ARRAY_VERTEX];
	input.indices = arrays[Mesh::ARRAY_INDEX];
	if (arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY) input.normals = arrays[Mesh::ARRAY_NORMAL];
	if (arrays[Mesh::ARRAY_TANGENT].get_type() == Variant::PACKED_FLOAT32_ARRAY) input.tangents = arrays[Mesh::ARRAY_TANGENT];
	if (arrays[Mesh::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY) input.uv0 = arrays[Mesh::ARRAY_TEX_UV];
	if (arrays[Mesh::ARRAY_TEX_UV2].get_type() == Variant::PACKED_VECTOR2_ARRAY) input.uv1 = arrays[Mesh::ARRAY_TEX_UV2];
	if (arrays[Mesh::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY) input.colors = arrays[Mesh::ARRAY_COLOR];
	if (arrays[Mesh::ARRAY_BONES].get_type() == Variant::PACKED_INT32_ARRAY) input.joints = arrays[Mesh::ARRAY_BONES];
	if (arrays[Mesh::ARRAY_WEIGHTS].get_type() == Variant::PACKED_FLOAT32_ARRAY) input.weights = arrays[Mesh::ARRAY_WEIGHTS];
	VirtualGeometryCompiler::Settings settings;
	settings.max_decoded_page_bytes = uint64_t(p_max_decoded_page_bytes);
	RendererVirtualGeometry::Package compiled;
	VirtualGeometryCompiler compiler;
	const Error result = compiler.compile(input, settings, compiled);
	ERR_FAIL_COND_V(result != OK, result);
	return set_compiled_package(compiled);
}

void VirtualGeometry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_revision"), &VirtualGeometry::get_revision);
	ClassDB::bind_method(D_METHOD("get_package_revision"), &VirtualGeometry::get_package_revision);
	ClassDB::bind_method(D_METHOD("get_source_asset_identity"), &VirtualGeometry::get_source_asset_identity);
	ClassDB::bind_method(D_METHOD("get_source_primitive_identity"), &VirtualGeometry::get_source_primitive_identity);
	ClassDB::bind_method(D_METHOD("is_valid_virtual_geometry"), &VirtualGeometry::is_valid_virtual_geometry);
	ClassDB::bind_method(D_METHOD("set_material_bindings", "bindings"), &VirtualGeometry::set_material_bindings);
	ClassDB::bind_method(D_METHOD("get_material_bindings"), &VirtualGeometry::get_material_bindings);
	ClassDB::bind_method(D_METHOD("has_complete_material_bindings"), &VirtualGeometry::has_complete_material_bindings);
	ClassDB::bind_method(D_METHOD("compile_from_mesh", "mesh", "surface", "source_asset_identity", "source_primitive_identity", "max_decoded_page_bytes"), &VirtualGeometry::compile_from_mesh, DEFVAL(int64_t(64 * 1024)));
	ClassDB::bind_method(D_METHOD("set_serialized_package", "blob"), &VirtualGeometry::set_serialized_package);
	ClassDB::bind_method(D_METHOD("get_serialized_package"), &VirtualGeometry::get_serialized_package);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "material_bindings", PROPERTY_HINT_ARRAY_TYPE, "Material"), "set_material_bindings", "get_material_bindings");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "compiled_package", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_serialized_package", "get_serialized_package");
}

VirtualGeometry::VirtualGeometry() {
	virtual_geometry = RenderingServer::get_singleton()->virtual_geometry_create();
}

VirtualGeometry::~VirtualGeometry() {
	const Callable changed = callable_mp(this, &VirtualGeometry::_material_binding_changed);
	for (const Ref<Material> &binding : material_bindings) {
		if (binding.is_valid() && binding->is_connected(SNAME("changed"), changed)) {
			binding->disconnect_changed(changed);
		}
	}
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RenderingServer::get_singleton()->free_rid(virtual_geometry);
}
