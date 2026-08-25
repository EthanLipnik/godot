/**************************************************************************/
/*  virtual_geometry_compiler.cpp                                         */
/**************************************************************************/

#include "virtual_geometry_compiler.h"

#include "core/error/error_macros.h"

#include <cstring>
#include <limits>

#include <thirdparty/meshoptimizer/meshoptimizer.h>

using namespace RendererVirtualGeometry;

namespace {

struct ClusterBuild {
	uint64_t stable_id = 0;
	uint32_t material_slot = 0;
	Vector<uint32_t> indices;
	AABB bounds;
};

struct HierarchyNode {
	Vector<uint64_t> cluster_ids;
	Vector<uint32_t> indices;
	uint32_t material_slot = 0;
	uint64_t group_id = 0;
	AABB bounds;
	real_t geometric_error = 0.0;
};

static bool _finite_color(const Color &p_color) {
	return Math::is_finite(p_color.r) && Math::is_finite(p_color.g) && Math::is_finite(p_color.b) && Math::is_finite(p_color.a);
}

static bool _append_bytes(PackedByteArray &r_bytes, const void *p_data, uint64_t p_size) {
	if (p_size > uint64_t(INT64_MAX) || uint64_t(r_bytes.size()) > uint64_t(INT64_MAX) - p_size) {
		return false;
	}
	const int64_t offset = r_bytes.size();
	if (r_bytes.resize(offset + int64_t(p_size)) != OK) {
		return false;
	}
	memcpy(r_bytes.ptrw() + offset, p_data, size_t(p_size));
	return true;
}

template <typename T>
static bool _append_scalar(PackedByteArray &r_bytes, const T &p_value) {
	return _append_bytes(r_bytes, &p_value, sizeof(T));
}

static uint64_t _hash_u64(uint64_t p_seed, uint64_t p_value) {
	return hash_bytes(reinterpret_cast<const uint8_t *>(&p_value), sizeof(p_value), p_seed);
}

static uint64_t _nonzero_id(uint64_t p_hash) {
	p_hash &= uint64_t(INT64_MAX);
	return p_hash == 0 ? 1 : p_hash;
}

static const RefinementGroupDescriptor *_find_group(const Manifest &p_manifest, uint64_t p_group_id) {
	for (const RefinementGroupDescriptor &group : p_manifest.groups) if (group.stable_id == p_group_id) return &group;
	return nullptr;
}

static void _append_finest_clusters(const Manifest &p_manifest, const RefinementGroupDescriptor &p_group, HashSet<uint64_t> &r_clusters) {
	if (p_group.child_group_ids.is_empty()) {
		for (uint64_t cluster_id : p_group.fine_cluster_ids) r_clusters.insert(cluster_id);
		return;
	}
	for (uint64_t child_id : p_group.child_group_ids) {
		const RefinementGroupDescriptor *child = _find_group(p_manifest, child_id);
		if (child) _append_finest_clusters(p_manifest, *child, r_clusters);
	}
}

static uint64_t _ray_group_id(const Manifest &p_manifest, uint64_t p_region_id, RayTransportTier p_tier, const Vector<uint64_t> &p_clusters) {
	uint64_t hash = _hash_u64(p_manifest.source_primitive_identity, 0x52415947524f5550ull); // "RAYGROUP"
	hash = _hash_u64(hash, p_manifest.ray_hint_schema_version);
	hash = _hash_u64(hash, p_region_id);
	hash = _hash_u64(hash, uint64_t(p_tier));
	for (uint64_t cluster_id : p_clusters) hash = _hash_u64(hash, cluster_id);
	return _nonzero_id(hash);
}

static void _emit_ray_groups(Manifest &r_manifest) {
	for (uint64_t root_id : r_manifest.root_group_ids) {
		const RefinementGroupDescriptor *root = _find_group(r_manifest, root_id);
		if (!root) continue;
		Vector<uint64_t> tier_clusters[3];
		tier_clusters[uint32_t(RayTransportTier::TIER_FAR)] = root->coarse_cluster_ids;
		tier_clusters[uint32_t(RayTransportTier::TIER_MIDDLE)] = root->fine_cluster_ids;
		HashSet<uint64_t> finest;
		_append_finest_clusters(r_manifest, *root, finest);
		for (uint64_t cluster_id : finest) tier_clusters[uint32_t(RayTransportTier::TIER_NEAR)].push_back(cluster_id);
		for (uint32_t tier_index = 0; tier_index < 3; tier_index++) {
			Vector<uint64_t> &clusters = tier_clusters[tier_index];
			clusters.sort();
			RayGroupDescriptor ray_group;
			ray_group.transport_region_id = root_id;
			ray_group.tier = RayTransportTier(tier_index);
			ray_group.cluster_ids = clusters;
			ray_group.bounds = root->bounds;
			ray_group.persistent_coarse = ray_group.tier == RayTransportTier::TIER_FAR;
			ray_group.stable_id = _ray_group_id(r_manifest, root_id, ray_group.tier, clusters);
			ray_group.revision = _hash_u64(ray_group.stable_id, root->revision);
			r_manifest.ray_groups.push_back(ray_group);
		}
	}
}

static uint64_t _input_digest(const VirtualGeometryCompiler::Input &p_input, uint32_t p_schema) {
	uint64_t hash = 1469598103934665603ull;
	for (int i = 0; i < p_input.positions.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.positions[i]), sizeof(Vector3), hash);
	for (int i = 0; i < p_input.indices.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.indices[i]), sizeof(int32_t), hash);
	for (int i = 0; i < p_input.normals.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.normals[i]), sizeof(Vector3), hash);
	for (int i = 0; i < p_input.tangents.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.tangents[i]), sizeof(float), hash);
	for (int i = 0; i < p_input.uv0.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.uv0[i]), sizeof(Vector2), hash);
	for (int i = 0; i < p_input.uv1.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.uv1[i]), sizeof(Vector2), hash);
	for (int i = 0; i < p_input.colors.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.colors[i]), sizeof(Color), hash);
	for (int i = 0; i < p_input.joints.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.joints[i]), sizeof(int32_t), hash);
	for (int i = 0; i < p_input.weights.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.weights[i]), sizeof(float), hash);
	for (int i = 0; i < p_input.triangle_materials.size(); i++) hash = hash_bytes(reinterpret_cast<const uint8_t *>(&p_input.triangle_materials[i]), sizeof(int32_t), hash);
	hash = _hash_u64(hash, p_schema);
	return _nonzero_id(hash);
}

static AABB _bounds_for_indices(const VirtualGeometryCompiler::Input &p_input, const Vector<uint32_t> &p_indices) {
	AABB bounds;
	bool initialized = false;
	for (uint32_t index : p_indices) {
		const Vector3 position = p_input.positions[index];
		if (!initialized) { bounds = AABB(position, Vector3()); initialized = true; } else { bounds.expand_to(position); }
	}
	return bounds;
}

static uint64_t _coverage_hash(uint64_t p_seed, const Vector<uint32_t> &p_indices, uint32_t p_material) {
	uint64_t hash = _hash_u64(p_seed, p_material);
	for (uint32_t index : p_indices) hash = _hash_u64(hash, index);
	return _nonzero_id(hash);
}

static Error _validate_input(const VirtualGeometryCompiler::Input &p_input, const VirtualGeometryCompiler::Settings &p_settings, uint32_t &r_schema, String &r_reason) {
	ERR_FAIL_COND_V_MSG(p_settings.max_vertices_per_cluster == 0 || p_settings.max_vertices_per_cluster > 256 || p_settings.max_triangles_per_cluster < 1 || p_settings.clusters_per_group < 2 || p_settings.max_decoded_page_bytes == 0, ERR_INVALID_PARAMETER, "Virtual geometry compiler settings are out of range.");
	if (p_input.conventional_path_only || !p_input.opaque || !p_input.immutable) {
		r_reason = p_input.conventional_path_reason.is_empty() ? "Input is outside the VG1 static opaque immutable profile." : p_input.conventional_path_reason;
		return OK;
	}
	ERR_FAIL_COND_V_MSG(p_input.source_asset_identity == 0 || p_input.source_primitive_identity == 0, ERR_INVALID_PARAMETER, "Virtual geometry requires stable source asset and primitive identities.");
	ERR_FAIL_COND_V_MSG(p_input.positions.is_empty() || p_input.indices.is_empty() || p_input.indices.size() % 3 != 0, ERR_INVALID_DATA, "Virtual geometry requires indexed triangle topology.");
	for (int i = 0; i < p_input.positions.size(); i++) ERR_FAIL_COND_V_MSG(!p_input.positions[i].is_finite(), ERR_INVALID_DATA, "Virtual geometry positions must be finite.");
	for (int i = 0; i < p_input.indices.size(); i++) ERR_FAIL_COND_V_MSG(p_input.indices[i] < 0 || p_input.indices[i] >= p_input.positions.size(), ERR_INVALID_DATA, "Virtual geometry index is out of range.");
	r_schema = STREAM_POSITION;
	if (!p_input.normals.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.normals.size() != p_input.positions.size(), ERR_INVALID_DATA, "Normal stream has the wrong vertex count."); for (int i = 0; i < p_input.normals.size(); i++) ERR_FAIL_COND_V_MSG(!p_input.normals[i].is_finite(), ERR_INVALID_DATA, "Normal stream contains non-finite values."); r_schema |= STREAM_NORMAL; }
	if (!p_input.tangents.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.tangents.size() != p_input.positions.size() * 4, ERR_INVALID_DATA, "Tangent stream must contain xyzw per vertex."); for (int i = 0; i < p_input.tangents.size(); i++) ERR_FAIL_COND_V_MSG(!Math::is_finite(p_input.tangents[i]), ERR_INVALID_DATA, "Tangent stream contains non-finite values."); r_schema |= STREAM_TANGENT; }
	if (!p_input.uv0.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.uv0.size() != p_input.positions.size(), ERR_INVALID_DATA, "UV0 stream has the wrong vertex count."); for (int i = 0; i < p_input.uv0.size(); i++) ERR_FAIL_COND_V_MSG(!p_input.uv0[i].is_finite(), ERR_INVALID_DATA, "UV0 stream contains non-finite values."); r_schema |= STREAM_UV0; }
	if (!p_input.uv1.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.uv1.size() != p_input.positions.size(), ERR_INVALID_DATA, "UV1 stream has the wrong vertex count."); for (int i = 0; i < p_input.uv1.size(); i++) ERR_FAIL_COND_V_MSG(!p_input.uv1[i].is_finite(), ERR_INVALID_DATA, "UV1 stream contains non-finite values."); r_schema |= STREAM_UV1; }
	if (!p_input.colors.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.colors.size() != p_input.positions.size(), ERR_INVALID_DATA, "Color stream has the wrong vertex count."); for (int i = 0; i < p_input.colors.size(); i++) ERR_FAIL_COND_V_MSG(!_finite_color(p_input.colors[i]), ERR_INVALID_DATA, "Color stream contains non-finite values."); r_schema |= STREAM_COLOR; }
	if (!p_input.joints.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.joints.size() != p_input.positions.size() * 4, ERR_INVALID_DATA, "Joint stream must contain four values per vertex."); r_schema |= STREAM_JOINTS; }
	if (!p_input.weights.is_empty()) { ERR_FAIL_COND_V_MSG(p_input.weights.size() != p_input.positions.size() * 4, ERR_INVALID_DATA, "Weight stream must contain four values per vertex."); for (int i = 0; i < p_input.weights.size(); i++) ERR_FAIL_COND_V_MSG(!Math::is_finite(p_input.weights[i]), ERR_INVALID_DATA, "Weight stream contains non-finite values."); r_schema |= STREAM_WEIGHTS; }
	ERR_FAIL_COND_V_MSG((r_schema & STREAM_JOINTS) != (r_schema & STREAM_WEIGHTS), ERR_INVALID_DATA, "Joint and weight streams must be declared together.");
	ERR_FAIL_COND_V_MSG(!p_input.triangle_materials.is_empty() && p_input.triangle_materials.size() != p_input.indices.size() / 3, ERR_INVALID_DATA, "Triangle material semantic stream has the wrong triangle count.");
	return OK;
}

static bool _serialize_cluster(const VirtualGeometryCompiler::Input &p_input, uint32_t p_schema, const ClusterBuild &p_cluster, PackedByteArray &r_payload, uint32_t &r_vertex_count) {
	HashMap<uint32_t, uint32_t> local_by_global;
	Vector<uint32_t> local_to_global;
	Vector<uint32_t> local_indices;
	for (uint32_t global : p_cluster.indices) {
			uint32_t *existing = local_by_global.getptr(global);
			uint32_t local = 0;
			if (existing) { local = *existing; } else { local = local_to_global.size(); local_by_global.insert(global, local); local_to_global.push_back(global); }
			local_indices.push_back(local);
	}
	r_vertex_count = local_to_global.size();
	const uint32_t magic = 0x31434756; // "VGC1"
	const uint32_t triangle_count = p_cluster.indices.size() / 3;
	if (!_append_scalar(r_payload, magic) || !_append_scalar(r_payload, p_schema) || !_append_scalar(r_payload, r_vertex_count) || !_append_scalar(r_payload, triangle_count)) return false;
	for (uint32_t global : local_to_global) {
		const Vector3 position = p_input.positions[global];
		if (!_append_bytes(r_payload, &position, sizeof(position))) return false;
		if ((p_schema & STREAM_NORMAL) && !_append_bytes(r_payload, &p_input.normals[global], sizeof(Vector3))) return false;
		if ((p_schema & STREAM_TANGENT) && !_append_bytes(r_payload, &p_input.tangents[global * 4], sizeof(float) * 4)) return false;
		if ((p_schema & STREAM_UV0) && !_append_bytes(r_payload, &p_input.uv0[global], sizeof(Vector2))) return false;
		if ((p_schema & STREAM_UV1) && !_append_bytes(r_payload, &p_input.uv1[global], sizeof(Vector2))) return false;
		if ((p_schema & STREAM_COLOR) && !_append_bytes(r_payload, &p_input.colors[global], sizeof(Color))) return false;
		if ((p_schema & STREAM_JOINTS) && !_append_bytes(r_payload, &p_input.joints[global * 4], sizeof(int32_t) * 4)) return false;
		if ((p_schema & STREAM_WEIGHTS) && !_append_bytes(r_payload, &p_input.weights[global * 4], sizeof(float) * 4)) return false;
	}
	for (uint32_t index : local_indices) if (!_append_scalar(r_payload, index)) return false;
	return true;
}

static Error _emit_clusters(const VirtualGeometryCompiler::Input &p_input, const VirtualGeometryCompiler::Settings &p_settings, uint32_t p_schema, uint64_t p_source_digest, uint32_t p_material, const Vector<uint32_t> &p_indices, Vector<ClusterBuild> &r_builds) {
	ClusterBuild current;
	current.material_slot = p_material;
	HashSet<uint32_t> vertices;
	for (int index_offset = 0; index_offset < p_indices.size(); index_offset += 3) {
		uint32_t new_vertices = 0;
		for (uint32_t corner = 0; corner < 3; corner++) if (!vertices.has(p_indices[index_offset + corner])) new_vertices++;
		if (!current.indices.is_empty() && (current.indices.size() / 3 >= int(p_settings.max_triangles_per_cluster) || vertices.size() + new_vertices > p_settings.max_vertices_per_cluster)) {
			current.bounds = _bounds_for_indices(p_input, current.indices);
			current.stable_id = _coverage_hash(p_source_digest, current.indices, p_material);
			r_builds.push_back(current);
			current = ClusterBuild(); current.material_slot = p_material; vertices.clear();
		}
		for (uint32_t corner = 0; corner < 3; corner++) { current.indices.push_back(p_indices[index_offset + corner]); vertices.insert(p_indices[index_offset + corner]); }
	}
	if (!current.indices.is_empty()) { current.bounds = _bounds_for_indices(p_input, current.indices); current.stable_id = _coverage_hash(p_source_digest, current.indices, p_material); r_builds.push_back(current); }
	return OK;
}

} // namespace

Error VirtualGeometryCompiler::compile(const Input &p_input, const Settings &p_settings, Package &r_package) {
	diagnostics.clear();
	r_package = Package();
	uint32_t schema_flags = STREAM_POSITION;
	String conventional_reason;
	const Error input_error = _validate_input(p_input, p_settings, schema_flags, conventional_reason);
	ERR_FAIL_COND_V(input_error != OK, input_error);
	Manifest &manifest = r_package.manifest;
	manifest.source_asset_identity = p_input.source_asset_identity;
	manifest.source_primitive_identity = p_input.source_primitive_identity;
	manifest.compiler_semantic_generation = p_settings.compiler_semantic_generation;
	if (!conventional_reason.is_empty()) {
		manifest.conventional_path_diagnostics.push_back(conventional_reason);
		diagnostics.push_back(conventional_reason);
		return OK;
	}
	manifest.source_digest = _input_digest(p_input, schema_flags);
	Vector<float> simplify_positions;
	simplify_positions.resize(p_input.positions.size() * 3);
	for (int i = 0; i < p_input.positions.size(); i++) {
		simplify_positions.write[i * 3 + 0] = p_input.positions[i].x;
		simplify_positions.write[i * 3 + 1] = p_input.positions[i].y;
		simplify_positions.write[i * 3 + 2] = p_input.positions[i].z;
	}
	manifest.resource_bounds = _bounds_for_indices(p_input, Vector<uint32_t>());
	// The helper above needs an actual coverage list for non-empty bounds.
	Vector<uint32_t> all_triangles;
	all_triangles.resize(p_input.indices.size() / 3);
	for (int i = 0; i < all_triangles.size(); i++) all_triangles.write[i] = i;
	Vector<uint32_t> all_indices;
	all_indices.resize(p_input.indices.size());
	for (int i = 0; i < all_indices.size(); i++) all_indices.write[i] = uint32_t(p_input.indices[i]);
	manifest.resource_bounds = _bounds_for_indices(p_input, all_indices);
	StreamSchema stream_schema;
	stream_schema.flags = schema_flags;
	stream_schema.stable_id = _nonzero_id(_hash_u64(manifest.source_digest, schema_flags));
	manifest.stream_schemas.push_back(stream_schema);

	HashMap<int32_t, Vector<uint32_t>> partitions;
	for (uint32_t triangle = 0; triangle < uint32_t(all_triangles.size()); triangle++) {
		const int32_t semantic = p_input.triangle_materials.is_empty() ? 0 : p_input.triangle_materials[triangle];
		partitions[semantic].push_back(triangle);
	}
	Vector<int32_t> material_semantics;
	for (const KeyValue<int32_t, Vector<uint32_t>> &entry : partitions) material_semantics.push_back(entry.key);
	material_semantics.sort();
	Vector<ClusterBuild> builds;
	for (int material_index = 0; material_index < material_semantics.size(); material_index++) {
		const int32_t semantic = material_semantics[material_index];
		MaterialDescriptor material;
		material.semantic_id = _nonzero_id(_hash_u64(manifest.source_digest, uint64_t(uint32_t(semantic))));
		manifest.materials.push_back(material);
		Vector<uint32_t> partition_indices;
		for (uint32_t triangle : partitions[semantic]) for (uint32_t corner = 0; corner < 3; corner++) partition_indices.push_back(uint32_t(p_input.indices[triangle * 3 + corner]));
		ERR_FAIL_COND_V(_emit_clusters(p_input, p_settings, schema_flags, manifest.source_digest, material_index, partition_indices, builds) != OK, ERR_CANT_CREATE);
	}
	ERR_FAIL_COND_V(builds.is_empty(), ERR_CANT_CREATE);
	HashSet<uint64_t> cluster_ids;
	for (const ClusterBuild &build : builds) {
		ERR_FAIL_COND_V_MSG(cluster_ids.has(build.stable_id), ERR_CANT_CREATE, "Stable virtual geometry cluster identity collision.");
		cluster_ids.insert(build.stable_id);
	}

	// Emit one complete high-precision canonical payload per logical cluster.
	Vector<PackedByteArray> decoded_cluster_payloads;
	for (int i = 0; i < builds.size(); i++) {
		PackedByteArray payload;
		uint32_t vertex_count = 0;
		ERR_FAIL_COND_V(!_serialize_cluster(p_input, schema_flags, builds[i], payload, vertex_count), ERR_OUT_OF_MEMORY);
		ClusterDescriptor descriptor;
		descriptor.stable_id = builds[i].stable_id;
		descriptor.topology_revision = _coverage_hash(manifest.source_digest, builds[i].indices, builds[i].material_slot);
		descriptor.attribute_revision = manifest.source_digest;
		descriptor.material_semantic_revision = manifest.materials[builds[i].material_slot].semantic_id;
		descriptor.vertex_count = vertex_count;
		descriptor.triangle_count = builds[i].indices.size() / 3;
		descriptor.stream_schema_id = stream_schema.stable_id;
		descriptor.material_slot = builds[i].material_slot;
		descriptor.bounds = builds[i].bounds;
		descriptor.sphere_center = builds[i].bounds.get_center();
		descriptor.sphere_radius = builds[i].bounds.size.length() * 0.5;
		descriptor.position_decode_scale = Vector3(1, 1, 1);
		descriptor.position_decode_bias = Vector3();
		if (p_input.double_sided) descriptor.flags |= CLUSTER_DOUBLE_SIDED;
		manifest.clusters.push_back(descriptor);
		decoded_cluster_payloads.push_back(payload);
	}

	// Create a deep material-safe hierarchy. meshoptimizer's locked-border mode
	// protects every external edge of the submitted group, so independently
	// selected neighboring groups retain matching boundary topology.
	Vector<HierarchyNode> level;
	for (int i = 0; i < builds.size(); i++) {
		HierarchyNode node;
		node.cluster_ids.push_back(builds[i].stable_id);
		node.indices = builds[i].indices;
		node.material_slot = builds[i].material_slot;
		node.bounds = builds[i].bounds;
		level.push_back(node);
	}
	while (level.size() > 1) {
		Vector<HierarchyNode> next;
		bool made_group = false;
		for (int start = 0; start < level.size();) {
			const uint32_t material_slot = level[start].material_slot;
			int end = start;
			while (end < level.size() && level[end].material_slot == material_slot && end - start < int(p_settings.clusters_per_group)) end++;
			if (end - start == 1) {
				next.push_back(level[start]);
				start = end;
				continue;
			}
			HierarchyNode parent;
			made_group = true;
			parent.material_slot = material_slot;
			bool have_bounds = false;
			for (int i = start; i < end; i++) {
				parent.cluster_ids.append_array(level[i].cluster_ids);
				parent.indices.append_array(level[i].indices);
				parent.geometric_error = MAX(parent.geometric_error, level[i].geometric_error);
				parent.bounds = have_bounds ? parent.bounds.merge(level[i].bounds) : level[i].bounds;
				have_bounds = true;
			}
			Vector<uint32_t> simplified;
			simplified.resize(parent.indices.size());
			const size_t target = MAX(size_t(3), (size_t(parent.indices.size()) / 2) / 3 * 3);
			float relative_error = 0.0f;
			const size_t simplified_count = meshopt_simplify(simplified.ptrw(), parent.indices.ptr(), parent.indices.size(), simplify_positions.ptr(), p_input.positions.size(), sizeof(float) * 3, target, 1.0f, meshopt_SimplifyLockBorder, &relative_error);
			const float scale = meshopt_simplifyScale(simplify_positions.ptr(), p_input.positions.size(), sizeof(float) * 3);
			const bool reduced = simplified_count >= 3 && simplified_count % 3 == 0 && simplified_count < size_t(parent.indices.size()) && Math::is_finite(relative_error) && relative_error >= 0.0f && Math::is_finite(scale) && scale >= 0.0f;
			if (reduced) {
				simplified.resize(simplified_count);
				parent.geometric_error += real_t(relative_error * scale);
			} else {
				simplified = parent.indices;
				diagnostics.push_back(vformat("VG1 retained exact coverage for material partition %d: border-locked simplification could not reduce %d triangles.", material_slot, parent.indices.size() / 3));
			}
			Vector<ClusterBuild> coarse_builds;
			ERR_FAIL_COND_V(_emit_clusters(p_input, p_settings, schema_flags, _hash_u64(manifest.source_digest, uint64_t(manifest.groups.size() + 1)), material_slot, simplified, coarse_builds) != OK, ERR_CANT_CREATE);
			for (const ClusterBuild &coarse : coarse_builds) {
				ERR_FAIL_COND_V(cluster_ids.has(coarse.stable_id), ERR_CANT_CREATE);
				cluster_ids.insert(coarse.stable_id);
				PackedByteArray payload; uint32_t vertices = 0;
				ERR_FAIL_COND_V(!_serialize_cluster(p_input, schema_flags, coarse, payload, vertices), ERR_OUT_OF_MEMORY);
				ClusterDescriptor descriptor;
				descriptor.stable_id = coarse.stable_id;
				descriptor.topology_revision = _coverage_hash(manifest.source_digest, coarse.indices, material_slot);
				descriptor.attribute_revision = manifest.source_digest;
				descriptor.material_semantic_revision = manifest.materials[material_slot].semantic_id;
				descriptor.vertex_count = vertices; descriptor.triangle_count = coarse.indices.size() / 3; descriptor.stream_schema_id = stream_schema.stable_id; descriptor.material_slot = material_slot; descriptor.bounds = parent.bounds; descriptor.sphere_center = parent.bounds.get_center(); descriptor.sphere_radius = parent.bounds.size.length() * 0.5; descriptor.position_decode_scale = Vector3(1, 1, 1); if (p_input.double_sided) descriptor.flags |= CLUSTER_DOUBLE_SIDED;
				manifest.clusters.push_back(descriptor); decoded_cluster_payloads.push_back(payload); parent.cluster_ids.push_back(coarse.stable_id);
			}
			RefinementGroupDescriptor group;
			group.fine_cluster_ids = parent.cluster_ids;
			// First half are fine children; coarse IDs were appended above.
			const int fine_count = parent.cluster_ids.size() - coarse_builds.size();
			group.fine_cluster_ids.resize(fine_count);
			for (const ClusterBuild &coarse : coarse_builds) group.coarse_cluster_ids.push_back(coarse.stable_id);
			group.bounds = parent.bounds;
			group.geometric_error = parent.geometric_error;
			group.atomic_dependencies = group.coarse_cluster_ids;
			group.stable_id = _coverage_hash(_hash_u64(_hash_u64(manifest.source_digest, 0x47524f5550ull), uint64_t(manifest.groups.size())), simplified, material_slot);
			group.revision = group.stable_id;
			for (int i = start; i < end; i++) {
				if (level[i].group_id == 0) {
					continue;
				}
				group.child_group_ids.push_back(level[i].group_id);
				for (RefinementGroupDescriptor &child : manifest.groups) {
					if (child.stable_id == level[i].group_id) {
						child.parent_group_ids.push_back(group.stable_id);
						break;
					}
				}
			}
			HashSet<uint64_t> existing_group_ids;
			for (const RefinementGroupDescriptor &existing : manifest.groups) existing_group_ids.insert(existing.stable_id);
			ERR_FAIL_COND_V(existing_group_ids.has(group.stable_id), ERR_CANT_CREATE);
			manifest.groups.push_back(group);
			parent.cluster_ids = group.coarse_cluster_ids;
			parent.indices = simplified;
			parent.group_id = group.stable_id;
			next.push_back(parent);
			start = end;
		}
		level = next;
		if (!made_group) {
			break;
		}
	}
	if (manifest.groups.is_empty()) {
		// A one-leaf source still has an explicit persistent fallback contract.
		RefinementGroupDescriptor group;
		group.fine_cluster_ids = level[0].cluster_ids;
		group.coarse_cluster_ids = level[0].cluster_ids;
		group.atomic_dependencies = group.coarse_cluster_ids;
		group.bounds = level[0].bounds;
		group.persistent_root = true;
		group.stable_id = _nonzero_id(_hash_u64(manifest.source_digest, 0x524f4f5447524f55ull));
		group.revision = group.stable_id;
		manifest.groups.push_back(group);
		level.write[0].group_id = group.stable_id;
	}
	for (HierarchyNode &root : level) {
		if (root.group_id == 0) {
			RefinementGroupDescriptor group;
			group.fine_cluster_ids = root.cluster_ids;
			group.coarse_cluster_ids = root.cluster_ids;
			group.atomic_dependencies = root.cluster_ids;
			group.bounds = root.bounds;
			group.stable_id = _nonzero_id(_hash_u64(_hash_u64(manifest.source_digest, 0x524f4f5447524f55ull), uint64_t(manifest.groups.size())));
			group.revision = group.stable_id;
			group.persistent_root = true;
			manifest.groups.push_back(group);
			root.group_id = group.stable_id;
		}
		manifest.root_group_ids.push_back(root.group_id);
		for (RefinementGroupDescriptor &group : manifest.groups) if (group.stable_id == root.group_id) group.persistent_root = true;
	}
	ERR_FAIL_COND_V(manifest.root_group_ids.is_empty(), ERR_CANT_CREATE);
	// Stable transport cuts are compiled before physical page packing. Their
	// identity therefore cannot change when pages are repacked or raster cuts
	// select a different subset of the same hierarchy.
	_emit_ray_groups(manifest);

	// Deterministic page packing. Logical IDs are already fixed; only physical
	// page IDs and offsets depend on the selected packing target.
	uint64_t package_offset = 0;
	PackedByteArray decoded_page;
	Vector<int> page_cluster_indices;
	auto flush_page = [&]() -> Error {
		if (decoded_page.is_empty()) return OK;
		PageDescriptor page;
		page.content_hash = hash_bytes(decoded_page.ptr(), decoded_page.size());
		page.stable_id = _nonzero_id(_hash_u64(page.content_hash, uint64_t(manifest.pages.size())));
		page.decoded_size = decoded_page.size(); page.file_offset = package_offset;
		PackedByteArray compressed;
		const int64_t max_size = Compression::get_max_compressed_buffer_size(decoded_page.size(), Compression::MODE_ZSTD);
		ERR_FAIL_COND_V(max_size <= 0 || compressed.resize(max_size) != OK, ERR_OUT_OF_MEMORY);
		const int64_t compressed_size = Compression::compress(compressed.ptrw(), decoded_page.ptr(), decoded_page.size(), Compression::MODE_ZSTD);
		ERR_FAIL_COND_V(compressed_size <= 0 || compressed.resize(compressed_size) != OK, ERR_CANT_CREATE);
		page.compressed_size = compressed.size(); page.file_size = page.compressed_size;
		ERR_FAIL_COND_V(!checked_add_u64(package_offset, page.file_size, package_offset), ERR_OUT_OF_MEMORY);
		for (int cluster_index : page_cluster_indices) { manifest.clusters.write[cluster_index].page_id = page.stable_id; page.cluster_ids.push_back(manifest.clusters[cluster_index].stable_id); }
		for (uint64_t root : manifest.root_group_ids) {
			for (const RefinementGroupDescriptor &group : manifest.groups) if (group.stable_id == root) for (uint64_t cluster : group.coarse_cluster_ids) if (page.cluster_ids.has(cluster)) page.persistent = true;
		}
		manifest.pages.push_back(page); r_package.compressed_pages.push_back(compressed); decoded_page.clear(); page_cluster_indices.clear();
		return OK;
	};
	for (int cluster_index = 0; cluster_index < manifest.clusters.size(); cluster_index++) {
		const PackedByteArray &payload = decoded_cluster_payloads[cluster_index];
		ERR_FAIL_COND_V(uint64_t(payload.size()) > p_settings.max_decoded_page_bytes, ERR_OUT_OF_MEMORY);
		if (!decoded_page.is_empty() && uint64_t(decoded_page.size()) + uint64_t(payload.size()) > p_settings.max_decoded_page_bytes) { ERR_FAIL_COND_V(flush_page() != OK, ERR_CANT_CREATE); }
		manifest.clusters.write[cluster_index].payload_offset = decoded_page.size();
		manifest.clusters.write[cluster_index].payload_size = payload.size();
		ERR_FAIL_COND_V(!_append_bytes(decoded_page, payload.ptr(), payload.size()), ERR_OUT_OF_MEMORY);
		page_cluster_indices.push_back(cluster_index);
	}
	ERR_FAIL_COND_V(flush_page() != OK, ERR_CANT_CREATE);
	DependencyRecord dependency;
	dependency.source_identity = p_input.source_asset_identity; dependency.source_digest = manifest.source_digest; dependency.compiler_settings_hash = _hash_u64(_hash_u64(p_settings.max_vertices_per_cluster, p_settings.max_triangles_per_cluster), p_settings.max_decoded_page_bytes); dependency.dependency_hash = _hash_u64(dependency.source_digest, dependency.compiler_settings_hash); manifest.dependencies.push_back(dependency); manifest.build_settings_hash = dependency.compiler_settings_hash;
	return validate_package(r_package, true);
}
