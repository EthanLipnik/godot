/**************************************************************************/
/*  baked_visibility_baker.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "baked_visibility_baker.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/math/math_funcs.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"

namespace {

constexpr float CELL_EPSILON = 0.001f;
constexpr float PATCH_EPSILON = 0.0001f;
constexpr int BVH_LEAF_SIZE = 8;
constexpr int MAX_PATCH_NOMINEES = 8;
constexpr int MAX_BLOCKER_TRIANGLES = 1048576;

struct GeometryEntry {
	String path;
	String identity;
	AABB aabb;
	MeshInstance3D *instance = nullptr;
};

struct LightEntry {
	String path;
	Vector3 position;
	float range = 0.0f;
	bool directional = false;
};

struct TrianglePatch {
	// Source triangles are the common case. Keeping them inline avoids three
	// heap-backed Vector allocations per imported triangle; merged polygons pay
	// for variable storage only after they have passed every certificate check.
	Vector3 triangle_vertices[3];
	Vector<Vector3> merged_vertices;
	Vector3 normal;
	int8_t source_side = 0; // +1 front, -1 back, 0 two-sided.
};

struct BlockerEntry {
	String path;
	AABB aabb;
	Vector<TrianglePatch> patches;
};

struct BlockerSurfaceCandidate {
	String path;
	MeshInstance3D *instance = nullptr;
	AABB aabb;
	int surface = 0;
	int triangle_count = 0;
	uint64_t usefulness_score = 0;
};

struct BvhNode {
	AABB aabb;
	int first = 0;
	int count = 0;
	int subtree_count = 0;
	int left = -1;
	int right = -1;
};

struct BakeContext {
	Vector<GeometryEntry> geometry;
	Vector<LightEntry> lights;
	Vector<BlockerSurfaceCandidate> blocker_surface_candidates;
	Vector<BlockerEntry> blockers;
	Vector<int> blocker_indices;
	Vector<uint32_t> blocker_seen_generation;
	uint32_t blocker_seen_epoch = 0;
	Vector<int> candidate_indices;
	Vector<BvhNode> bvh;
	Vector<BvhNode> candidate_bvh;
	Transform3D local_from_global;
	Node *path_root = nullptr;
	BakedVisibilityBakeOutput::PreprocessStats preprocess;
};

struct GeometryPathSort {
	bool operator()(const GeometryEntry &a, const GeometryEntry &b) const { return a.path < b.path; }
};

struct LightPathSort {
	bool operator()(const LightEntry &a, const LightEntry &b) const { return a.path < b.path; }
};

struct BlockerPathSort {
	bool operator()(const BlockerEntry &a, const BlockerEntry &b) const { return a.path < b.path; }
};

struct BlockerSurfacePrioritySort {
	bool operator()(const BlockerSurfaceCandidate &a, const BlockerSurfaceCandidate &b) const {
		if (a.usefulness_score != b.usefulness_score) {
			return a.usefulness_score > b.usefulness_score;
		}
		if (a.path != b.path) {
			return a.path < b.path;
		}
		return a.surface < b.surface;
	}
};

struct BlockerSurfacePathSort {
	bool operator()(const BlockerSurfaceCandidate &a, const BlockerSurfaceCandidate &b) const {
		if (a.path != b.path) {
			return a.path < b.path;
		}
		return a.surface < b.surface;
	}
};

struct StringPathSort {
	bool operator()(const String &a, const String &b) const { return a < b; }
};

static int _compare_vector3(const Vector3 &p_a, const Vector3 &p_b) {
	if (p_a.x != p_b.x) {
		return p_a.x < p_b.x ? -1 : 1;
	}
	if (p_a.y != p_b.y) {
		return p_a.y < p_b.y ? -1 : 1;
	}
	if (p_a.z != p_b.z) {
		return p_a.z < p_b.z ? -1 : 1;
	}
	return 0;
}

static int _patch_vertex_count(const TrianglePatch &p_patch) {
	return p_patch.merged_vertices.is_empty() ? 3 : p_patch.merged_vertices.size();
}

static const Vector3 &_patch_vertex(const TrianglePatch &p_patch, int p_index) {
	return p_patch.merged_vertices.is_empty() ? p_patch.triangle_vertices[p_index] : p_patch.merged_vertices[p_index];
}

static bool _is_opaque_material(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return true;
	}
	const BaseMaterial3D *base = Object::cast_to<BaseMaterial3D>(p_material.ptr());
	return base && base->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED;
}

static Transform3D _node_transform(const Node3D *p_node, const Transform3D &p_local_from_global) {
	if (p_node->is_inside_tree()) {
		return p_local_from_global * p_node->get_global_transform();
	}
	Transform3D transform = p_node->get_transform();
	for (const Node *parent = p_node->get_parent(); parent; parent = parent->get_parent()) {
		if (const Node3D *parent_3d = Object::cast_to<Node3D>(parent)) {
			transform = parent_3d->get_transform() * transform;
		}
	}
	return p_local_from_global * transform;
}

static bool _is_static_mesh(const MeshInstance3D *p_mesh) {
	if (!p_mesh->is_visible() || p_mesh->get_gi_mode() == GeometryInstance3D::GI_MODE_DYNAMIC) {
		return false;
	}
	if (p_mesh->get_skin().is_valid()) {
		return false;
	}
	Ref<Mesh> mesh = p_mesh->get_mesh();
	return mesh.is_valid() && mesh->get_blend_shape_count() == 0;
}

static float _polygon_area(const Vector<Vector3> &p_vertices, const Vector3 &p_normal) {
	float area = 0.0f;
	for (int i = 0; i < p_vertices.size(); i++) {
		area += p_normal.dot(p_vertices[i].cross(p_vertices[(i + 1) % p_vertices.size()])) * 0.5f;
	}
	return Math::abs(area);
}

static void _order_convex_polygon(Vector<Vector3> &r_vertices, const Vector3 &p_normal) {
	Vector3 center;
	for (const Vector3 &vertex : r_vertices) {
		center += vertex;
	}
	center /= r_vertices.size();
	int first = 0;
	for (int i = 1; i < r_vertices.size(); i++) {
		if (_compare_vector3(r_vertices[i], r_vertices[first]) < 0) {
			first = i;
		}
	}
	SWAP(r_vertices.write[0], r_vertices.write[first]);
	const Vector3 u = (r_vertices[0] - center).normalized();
	const Vector3 v = p_normal.cross(u);
	for (int i = 1; i < r_vertices.size(); i++) {
		const Vector3 value = r_vertices[i];
		const float angle = Math::atan2((value - center).dot(v), (value - center).dot(u));
		int j = i;
		while (j > 0) {
			const float previous = Math::atan2((r_vertices[j - 1] - center).dot(v), (r_vertices[j - 1] - center).dot(u));
			if (previous < angle || (previous == angle && _compare_vector3(r_vertices[j - 1], value) <= 0)) {
				break;
			}
			r_vertices.write[j] = r_vertices[j - 1];
			j--;
		}
		r_vertices.write[j] = value;
	}
}

struct PatchSort {
	bool operator()(const TrianglePatch &p_a, const TrianglePatch &p_b) const {
		Vector3 a[3] = { p_a.triangle_vertices[0], p_a.triangle_vertices[1], p_a.triangle_vertices[2] };
		Vector3 b[3] = { p_b.triangle_vertices[0], p_b.triangle_vertices[1], p_b.triangle_vertices[2] };
		for (int i = 1; i < 3; i++) {
			for (int j = i; j > 0 && _compare_vector3(a[j], a[j - 1]) < 0; j--) {
				SWAP(a[j], a[j - 1]);
			}
			for (int j = i; j > 0 && _compare_vector3(b[j], b[j - 1]) < 0; j--) {
				SWAP(b[j], b[j - 1]);
			}
		}
		for (int i = 0; i < 3; i++) {
			const int compare = _compare_vector3(a[i], b[i]);
			if (compare != 0) {
				return compare < 0;
			}
		}
		if (p_a.source_side != p_b.source_side) {
			return p_a.source_side < p_b.source_side;
		}
		return _compare_vector3(p_a.normal, p_b.normal) < 0;
	}
};

struct PatchEdge {
	Vector3 first_key;
	Vector3 second_key;
	Vector3 from;
	Vector3 to;
	int patch = 0;
	int edge = 0;
	bool forward = false;
};

struct PatchEdgeSort {
	bool operator()(const PatchEdge &p_a, const PatchEdge &p_b) const {
		const int first = _compare_vector3(p_a.first_key, p_b.first_key);
		if (first != 0) {
			return first < 0;
		}
		const int second = _compare_vector3(p_a.second_key, p_b.second_key);
		if (second != 0) {
			return second < 0;
		}
		if (p_a.patch != p_b.patch) {
			return p_a.patch < p_b.patch;
		}
		return p_a.edge < p_b.edge;
	}
};

static Vector3 _edge_key(const Vector3 &p_vertex) {
	return p_vertex.snappedf(PATCH_EPSILON);
}

static bool _try_merge_triangle_pair(const TrianglePatch &p_a, const TrianglePatch &p_b, TrianglePatch &r_merged, uint64_t &r_vertex_visits) {
	// Pair-only merging keeps every attempt bounded to six source vertices. A
	// larger convex union is conservatively represented by its certified quads
	// and triangles rather than repeatedly rebuilding a growing polygon.
	if (!p_a.merged_vertices.is_empty() || !p_b.merged_vertices.is_empty()) {
		return false;
	}
	if (p_a.source_side != p_b.source_side || p_a.normal.dot(p_b.normal) < 0.99999f || Math::abs(p_a.normal.dot(_patch_vertex(p_b, 0) - _patch_vertex(p_a, 0))) > PATCH_EPSILON) {
		return false;
	}
	Vector<Vector3> vertices;
	vertices.reserve(_patch_vertex_count(p_a) + _patch_vertex_count(p_b));
	for (const TrianglePatch *patch : { &p_a, &p_b }) {
		for (int i = 0; i < _patch_vertex_count(*patch); i++) {
			const Vector3 &vertex = _patch_vertex(*patch, i);
			r_vertex_visits++;
			bool duplicate = false;
			for (const Vector3 &existing : vertices) {
				if (existing.is_equal_approx(vertex)) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) {
				vertices.push_back(vertex);
			}
		}
	}
	if (vertices.size() < 3) {
		return false;
	}
	_order_convex_polygon(vertices, p_a.normal);
	bool convex = true;
	for (int vertex = 0; vertex < vertices.size(); vertex++) {
		const Vector3 &a = vertices[vertex];
		const Vector3 &b = vertices[(vertex + 1) % vertices.size()];
		const Vector3 &c = vertices[(vertex + 2) % vertices.size()];
		convex &= p_a.normal.dot((b - a).cross(c - b)) > PATCH_EPSILON;
	}
	Vector<Vector3> a_vertices;
	Vector<Vector3> b_vertices;
	a_vertices.reserve(_patch_vertex_count(p_a));
	b_vertices.reserve(_patch_vertex_count(p_b));
	for (int i = 0; i < _patch_vertex_count(p_a); i++) {
		a_vertices.push_back(_patch_vertex(p_a, i));
	}
	for (int i = 0; i < _patch_vertex_count(p_b); i++) {
		b_vertices.push_back(_patch_vertex(p_b, i));
	}
	const float source_area = _polygon_area(a_vertices, p_a.normal) + _polygon_area(b_vertices, p_a.normal);
	const float union_area = _polygon_area(vertices, p_a.normal);
	if (!convex || Math::abs(source_area - union_area) > MAX(PATCH_EPSILON, source_area * 0.0001f)) {
		return false; // A gap, overlap, hole, or concavity is never a certificate patch.
	}
	r_merged.triangle_vertices[0] = vertices[0];
	r_merged.triangle_vertices[1] = vertices[1];
	r_merged.triangle_vertices[2] = vertices[2];
	r_merged.merged_vertices = vertices;
	r_merged.normal = p_a.normal;
	r_merged.source_side = p_a.source_side;
	return true;
}

static void _merge_coplanar_pairs(Vector<TrianglePatch> &r_patches, BakedVisibilityBakeOutput::PreprocessStats &r_stats) {
	if (r_patches.size() < 2) {
		return;
	}
	const uint64_t merged_before = r_stats.merged_patch_count;
	// Canonical source IDs make triangle/index permutations irrelevant to merge
	// order. Each triangle enters at most one convex pair merge, so edge sorting
	// is O(T log T) and every post-sort certificate check is O(1). This trades
	// some larger valid unions for bounded preprocessing work; unmatched
	// triangles remain conservative certificate patches.
	r_patches.sort_custom<PatchSort>();
	Vector<PatchEdge> edges;
	edges.reserve(r_patches.size() * 3);
	for (int patch = 0; patch < r_patches.size(); patch++) {
		for (int edge = 0; edge < 3; edge++) {
			const Vector3 &from = r_patches[patch].triangle_vertices[edge];
			const Vector3 &to = r_patches[patch].triangle_vertices[(edge + 1) % 3];
			const Vector3 from_key = _edge_key(from);
			const Vector3 to_key = _edge_key(to);
			PatchEdge record;
			record.first_key = _compare_vector3(from_key, to_key) <= 0 ? from_key : to_key;
			record.second_key = _compare_vector3(from_key, to_key) <= 0 ? to_key : from_key;
			record.from = from;
			record.to = to;
			record.patch = patch;
			record.edge = edge;
			record.forward = _compare_vector3(from_key, to_key) <= 0;
			edges.push_back(record);
		}
	}
	edges.sort_custom<PatchEdgeSort>();
	Vector<uint8_t> matched;
	matched.resize(r_patches.size());
	Vector<uint8_t> absorbed_patches;
	absorbed_patches.resize(r_patches.size());
	for (int patch = 0; patch < r_patches.size(); patch++) {
		matched.write[patch] = false;
		absorbed_patches.write[patch] = false;
	}
	for (int begin = 0; begin < edges.size();) {
		int end = begin + 1;
		while (end < edges.size() && _compare_vector3(edges[begin].first_key, edges[end].first_key) == 0 && _compare_vector3(edges[begin].second_key, edges[end].second_key) == 0) {
			end++;
		}
		const bool shared_boundary = end - begin == 2 && edges[begin].forward != edges[begin + 1].forward &&
				edges[begin].from.is_equal_approx(edges[begin + 1].to) && edges[begin].to.is_equal_approx(edges[begin + 1].from);
		if (shared_boundary) {
			r_stats.shared_edge_candidates++;
			const int first = edges[begin].patch;
			const int second = edges[begin + 1].patch;
			if (!matched[first] && !matched[second]) {
				r_stats.merge_attempts++;
				TrianglePatch merged;
				if (_try_merge_triangle_pair(r_patches[first], r_patches[second], merged, r_stats.merge_vertex_visits)) {
					const int retained = MIN(first, second);
					const int absorbed_index = MAX(first, second);
					r_patches.write[retained] = merged;
					matched.write[first] = true;
					matched.write[second] = true;
					absorbed_patches.write[absorbed_index] = true;
					r_stats.merged_patch_count++;
				}
			}
		} else if (end - begin > 1) {
			r_stats.ambiguous_edges++;
		}
		begin = end;
	}
	Vector<TrianglePatch> merged_patches;
	merged_patches.reserve(r_patches.size() - int(r_stats.merged_patch_count - merged_before));
	for (int patch = 0; patch < r_patches.size(); patch++) {
		if (!absorbed_patches[patch]) {
			merged_patches.push_back(std::move(r_patches.write[patch]));
		}
	}
	r_patches = merged_patches;
}

static uint64_t _quantized_max_aabb_face_area(const AABB &p_aabb) {
	const Vector3 size = p_aabb.size.abs();
	const float max_area = MAX(size.x * size.y, MAX(size.x * size.z, size.y * size.z));
	if (!Math::is_finite(max_area) || max_area <= 0.0f) {
		return 0;
	}
	return uint64_t(MIN(Math::floor(double(max_area) * 1024.0), 4294967295.0));
}

static int _surface_triangle_count(const Ref<Mesh> &p_mesh, int p_surface) {
	const BitField<Mesh::ArrayFormat> format = p_mesh->surface_get_format(p_surface);
	if (!(format & Mesh::ARRAY_FORMAT_VERTEX)) {
		return 0;
	}
	const int element_count = (format & Mesh::ARRAY_FORMAT_INDEX) ? p_mesh->surface_get_array_index_len(p_surface) : p_mesh->surface_get_array_len(p_surface);
	return element_count >= 3 && (element_count % 3) == 0 ? element_count / 3 : 0;
}

static void _collect_blocker_surface_candidates(const GeometryEntry &p_geometry, BakeContext &r_context, int &r_rejected_blockers) {
	Ref<Mesh> mesh = p_geometry.instance->get_mesh();
	if (mesh.is_null()) {
		return;
	}
	const uint64_t quantized_area = _quantized_max_aabb_face_area(p_geometry.aabb);
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		const int triangle_count = _surface_triangle_count(mesh, surface);
		Ref<Material> material = p_geometry.instance->get_active_material(surface);
		if (mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES || triangle_count == 0 || !_is_opaque_material(material)) {
			r_context.preprocess.blocker_surfaces_rejected++;
			r_context.preprocess.blocker_triangles_rejected += triangle_count;
			r_rejected_blockers++;
			continue;
		}
		BlockerSurfaceCandidate candidate;
		candidate.path = p_geometry.path;
		candidate.instance = p_geometry.instance;
		candidate.aabb = p_geometry.aabb;
		candidate.surface = surface;
		candidate.triangle_count = triangle_count;
		candidate.usefulness_score = quantized_area / uint64_t(triangle_count);
		r_context.blocker_surface_candidates.push_back(candidate);
		r_context.preprocess.blocker_surface_candidates++;
	}
}

static bool _append_opaque_surface_patches(const BlockerSurfaceCandidate &p_candidate, const Transform3D &p_local_from_global, uint64_t p_remaining_triangles, BlockerEntry &r_blocker, uint64_t &r_actual_triangle_count) {
	Ref<Mesh> mesh = p_candidate.instance->get_mesh();
	if (mesh.is_null() || mesh->get_blend_shape_count() != 0 || p_candidate.instance->get_skin().is_valid()) {
		return false;
	}
	const int surface = p_candidate.surface;
	Ref<Material> material = p_candidate.instance->get_active_material(surface);
	if (mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES || !_is_opaque_material(material)) {
		return false;
	}
	const BaseMaterial3D *base = material.is_valid() ? Object::cast_to<BaseMaterial3D>(material.ptr()) : nullptr;
	const BaseMaterial3D::CullMode cull_mode = base ? base->get_cull_mode() : BaseMaterial3D::CULL_BACK;
	Array arrays = mesh->surface_get_arrays(surface);
	PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
	PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	const BitField<Mesh::ArrayFormat> format = mesh->surface_get_format(surface);
	const bool has_indices = format & Mesh::ARRAY_FORMAT_INDEX;
	const int triangle_index_count = has_indices ? indices.size() : vertices.size();
	const int expected_element_count = has_indices ? mesh->surface_get_array_index_len(surface) : mesh->surface_get_array_len(surface);
	if (vertices.is_empty() || vertices.size() != mesh->surface_get_array_len(surface) || (has_indices != !indices.is_empty()) || triangle_index_count != expected_element_count || (triangle_index_count % 3) != 0) {
		return false;
	}
	r_actual_triangle_count = uint64_t(triangle_index_count / 3);
	if (r_actual_triangle_count > uint64_t(p_candidate.triangle_count) || r_actual_triangle_count > p_remaining_triangles) {
		return false;
	}
	for (int i = 0; i < triangle_index_count; i += 3) {
		const int ia = indices.is_empty() ? i : indices[i];
		const int ib = indices.is_empty() ? i + 1 : indices[i + 1];
		const int ic = indices.is_empty() ? i + 2 : indices[i + 2];
		if (ia < 0 || ib < 0 || ic < 0 || ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
			return false;
		}
	}
	const int patch_count_before = r_blocker.patches.size();
	const Transform3D transform = _node_transform(p_candidate.instance, p_local_from_global);
	for (int i = 0; i < triangle_index_count; i += 3) {
		const int ia = indices.is_empty() ? i : indices[i];
		const int ib = indices.is_empty() ? i + 1 : indices[i + 1];
		const int ic = indices.is_empty() ? i + 2 : indices[i + 2];
		TrianglePatch patch;
		patch.triangle_vertices[0] = transform.xform(vertices[ia]);
		patch.triangle_vertices[1] = transform.xform(vertices[ib]);
		patch.triangle_vertices[2] = transform.xform(vertices[ic]);
		patch.normal = (patch.triangle_vertices[1] - patch.triangle_vertices[0]).cross(patch.triangle_vertices[2] - patch.triangle_vertices[0]);
		if (patch.normal.length_squared() <= CMP_EPSILON2) {
			continue;
		}
		patch.normal.normalize();
		patch.source_side = cull_mode == BaseMaterial3D::CULL_DISABLED ? 0 : (cull_mode == BaseMaterial3D::CULL_BACK ? -1 : 1);
		if (transform.basis.determinant() < 0.0f) {
			patch.source_side = -patch.source_side;
		}
		r_blocker.patches.push_back(patch);
		for (const Vector3 &vertex : patch.triangle_vertices) {
			r_blocker.aabb.expand_to(vertex);
		}
	}
	return r_blocker.patches.size() > patch_count_before;
}

static int _resolve_max_blocker_triangles(const BakedVisibilityBakeInput &p_input) {
	int result = CLAMP(p_input.max_blocker_triangles, 0, MAX_BLOCKER_TRIANGLES);
	if (!p_input.anchor) {
		return result;
	}
	bool valid = false;
	const Variant authored_value = p_input.anchor->get("max_blocker_triangles", &valid);
	if (valid && authored_value.get_type() == Variant::INT) {
		result = CLAMP(int(authored_value), 0, MAX_BLOCKER_TRIANGLES);
	}
	return result;
}

static void _select_and_materialize_blocker_surfaces(BakeContext &r_context, int p_max_blocker_triangles, int &r_rejected_blockers) {
	r_context.blocker_surface_candidates.sort_custom<BlockerSurfacePrioritySort>();
	Vector<BlockerSurfaceCandidate> selected;
	uint64_t remaining_triangles = p_max_blocker_triangles;
	for (const BlockerSurfaceCandidate &candidate : r_context.blocker_surface_candidates) {
		if (uint64_t(candidate.triangle_count) > remaining_triangles) {
			r_context.preprocess.blocker_surfaces_rejected++;
			r_context.preprocess.blocker_triangles_rejected += candidate.triangle_count;
			r_rejected_blockers++;
			continue;
		}
		selected.push_back(candidate);
		remaining_triangles -= candidate.triangle_count;
	}
	selected.sort_custom<BlockerSurfacePathSort>();
	uint64_t materialization_remaining_triangles = p_max_blocker_triangles;
	for (const BlockerSurfaceCandidate &candidate : selected) {
		if (r_context.blockers.is_empty() || r_context.blockers[r_context.blockers.size() - 1].path != candidate.path) {
			BlockerEntry blocker;
			blocker.path = candidate.path;
			blocker.aabb = AABB();
			r_context.blockers.push_back(blocker);
		}
		BlockerEntry &blocker = r_context.blockers.write[r_context.blockers.size() - 1];
		uint64_t actual_triangle_count = 0;
		r_context.preprocess.blocker_arrays_materialized++;
		if (!_append_opaque_surface_patches(candidate, r_context.local_from_global, materialization_remaining_triangles, blocker, actual_triangle_count) || actual_triangle_count > uint64_t(candidate.triangle_count)) {
			r_context.preprocess.blocker_surfaces_rejected++;
			r_context.preprocess.blocker_triangles_rejected += candidate.triangle_count;
			r_rejected_blockers++;
			continue;
		}
		materialization_remaining_triangles -= actual_triangle_count;
		r_context.preprocess.blocker_surfaces_selected++;
		r_context.preprocess.blocker_triangles_selected += actual_triangle_count;
		r_context.preprocess.triangle_count += actual_triangle_count;
	}
	Vector<BlockerEntry> retained_blockers;
	retained_blockers.reserve(r_context.blockers.size());
	for (BlockerEntry &blocker : r_context.blockers) {
		if (!blocker.patches.is_empty()) {
			retained_blockers.push_back(std::move(blocker));
		}
	}
	r_context.blockers = retained_blockers;
}

static void _collect_scene(Node *p_node, BakeContext &r_context, uint32_t p_bake_mask) {
	if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node)) {
		if ((mesh->get_layer_mask() & p_bake_mask) != 0 && _is_static_mesh(mesh)) {
			GeometryEntry geometry;
			geometry.path = String(r_context.path_root->get_path_to(mesh));
			geometry.identity = BakedVisibilityBaker::make_geometry_identity(mesh);
			geometry.instance = mesh;
			geometry.aabb = _node_transform(mesh, r_context.local_from_global).xform(mesh->get_aabb());
			if (geometry.aabb.is_finite() && geometry.aabb.get_volume() > 0.0f) {
				r_context.geometry.push_back(geometry);
			}
		}
	}
	if (Light3D *light = Object::cast_to<Light3D>(p_node)) {
		if ((light->get_cull_mask() & p_bake_mask) != 0 && light->is_visible() && !light->is_editor_only()) {
			LightEntry entry;
			entry.path = String(r_context.path_root->get_path_to(light));
			entry.position = _node_transform(light, r_context.local_from_global).origin;
			entry.directional = light->get_light_type() == RSE::LIGHT_DIRECTIONAL;
			entry.range = entry.directional ? 0.0f : MAX(0.0f, light->get_param(Light3D::PARAM_RANGE));
			r_context.lights.push_back(entry);
		}
	}
	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_scene(p_node->get_child(child), r_context, p_bake_mask);
	}
}

static uint32_t _expand_morton_bits(uint32_t p_value) {
	p_value = (p_value | (p_value << 16)) & 0x030000FF;
	p_value = (p_value | (p_value << 8)) & 0x0300F00F;
	p_value = (p_value | (p_value << 4)) & 0x030C30C3;
	p_value = (p_value | (p_value << 2)) & 0x09249249;
	return p_value;
}

static uint32_t _morton_key(const Vector3 &p_center, const AABB &p_centers) {
	const Vector3 extent = p_centers.size;
	const auto quantize = [](float p_value, float p_start, float p_extent) {
		if (p_extent <= CMP_EPSILON) {
			return uint32_t(0);
		}
		return uint32_t(CLAMP(int(Math::floor((p_value - p_start) / p_extent * 1023.0f)), 0, 1023));
	};
	const uint32_t x = quantize(p_center.x, p_centers.position.x, extent.x);
	const uint32_t y = quantize(p_center.y, p_centers.position.y, extent.y);
	const uint32_t z = quantize(p_center.z, p_centers.position.z, extent.z);
	return (_expand_morton_bits(x) << 2) | (_expand_morton_bits(y) << 1) | _expand_morton_bits(z);
}

struct SpatialIndex {
	uint32_t morton = 0;
	String path;
	int index = 0;
};

struct SpatialIndexSort {
	bool operator()(const SpatialIndex &p_a, const SpatialIndex &p_b) const {
		if (p_a.morton != p_b.morton) {
			return p_a.morton < p_b.morton;
		}
		if (p_a.path != p_b.path) {
			return p_a.path < p_b.path;
		}
		return p_a.index < p_b.index;
	}
};

template <typename T>
static void _order_spatial_bvh_indices(Vector<int> &r_indices, const Vector<T> &p_entries, BakedVisibilityBakeOutput::PreprocessStats &r_stats) {
	if (r_indices.is_empty()) {
		return;
	}
	AABB centers(p_entries[r_indices[0]].aabb.get_center(), Vector3());
	for (int i = 1; i < r_indices.size(); i++) {
		centers.expand_to(p_entries[r_indices[i]].aabb.get_center());
	}
	Vector<SpatialIndex> order;
	order.resize(r_indices.size());
	for (int i = 0; i < r_indices.size(); i++) {
		const int index = r_indices[i];
		SpatialIndex entry;
		entry.morton = _morton_key(p_entries[index].aabb.get_center(), centers);
		entry.path = p_entries[index].path;
		entry.index = index;
		order.write[i] = entry;
	}
	order.sort_custom<SpatialIndexSort>();
	for (int i = 0; i < order.size(); i++) {
		r_indices.write[i] = order[i].index;
	}
	r_stats.bvh_order_entries += order.size();
}

template <typename T>
static int _build_spatial_bvh(Vector<BvhNode> &r_nodes, Vector<int> &r_indices, const Vector<T> &p_entries, int p_first, int p_count) {
	const int node_index = r_nodes.size();
	r_nodes.push_back(BvhNode());
	BvhNode &node = r_nodes.write[node_index];
	node.first = p_first;
	node.count = p_count;
	node.subtree_count = p_count;
	for (int i = 0; i < p_count; i++) {
		const AABB &aabb = p_entries[r_indices[p_first + i]].aabb;
		node.aabb = i == 0 ? aabb : node.aabb.merge(aabb);
	}
	if (p_count > BVH_LEAF_SIZE) {
		// Indices receive a single deterministic Morton order before this linear
		// split recursion. Re-sorting every range would turn construction into
		// O(n log^2 n) work for large imported scenes.
		const int left_count = p_count / 2;
		const int left = _build_spatial_bvh(r_nodes, r_indices, p_entries, p_first, left_count);
		const int right = _build_spatial_bvh(r_nodes, r_indices, p_entries, p_first + left_count, p_count - left_count);
		r_nodes.write[node_index].left = left;
		r_nodes.write[node_index].right = right;
		r_nodes.write[node_index].count = 0;
	}
	return node_index;
}

static void _query_bvh(const Vector<BvhNode> &p_nodes, const Vector<int> &p_indices, int p_node, const Vector3 &p_from, const Vector3 &p_to, Vector<uint32_t> &r_seen_generation, uint32_t p_generation, Vector<int> &r_blockers) {
	const BvhNode &node = p_nodes[p_node];
	if (!node.aabb.intersects_segment(p_from, p_to)) {
		return;
	}
	if (node.count > 0) {
		for (int i = 0; i < node.count; i++) {
			const int blocker = p_indices[node.first + i];
			if (r_seen_generation[blocker] != p_generation) {
				r_seen_generation.write[blocker] = p_generation;
				r_blockers.push_back(blocker);
			}
		}
		return;
	}
	_query_bvh(p_nodes, p_indices, node.left, p_from, p_to, r_seen_generation, p_generation, r_blockers);
	_query_bvh(p_nodes, p_indices, node.right, p_from, p_to, r_seen_generation, p_generation, r_blockers);
}

static uint32_t _next_blocker_seen_generation(BakeContext &r_context) {
	if (r_context.blocker_seen_epoch == 0xffffffffu) {
		for (int i = 0; i < r_context.blocker_seen_generation.size(); i++) {
			r_context.blocker_seen_generation.write[i] = 0;
		}
		r_context.blocker_seen_epoch = 1;
	} else {
		r_context.blocker_seen_epoch++;
	}
	return r_context.blocker_seen_epoch;
}

static bool _intersects_patch(const TrianglePatch &p_patch, const Vector3 &p_from, const Vector3 &p_to, bool p_shrunk) {
	const Vector3 direction = p_to - p_from;
	const float denominator = p_patch.normal.dot(direction);
	if (Math::abs(denominator) <= CMP_EPSILON) {
		return false;
	}
	const float t = p_patch.normal.dot(_patch_vertex(p_patch, 0) - p_from) / denominator;
	if (t <= PATCH_EPSILON || t >= 1.0f - PATCH_EPSILON) {
		return false;
	}
	const Vector3 point = p_from + direction * t;
	const float edge_epsilon = p_shrunk ? PATCH_EPSILON : -PATCH_EPSILON;
	for (int vertex = 0; vertex < _patch_vertex_count(p_patch); vertex++) {
		const Vector3 &a = _patch_vertex(p_patch, vertex);
		const Vector3 &b = _patch_vertex(p_patch, (vertex + 1) % _patch_vertex_count(p_patch));
		if (p_patch.normal.dot((b - a).cross(point - a)) < edge_epsilon) {
			return false;
		}
	}
	return true;
}

static bool _certifies_exclusion(BakeContext &r_context, const AABB &p_cell, const AABB &p_candidate) {
	if (r_context.bvh.is_empty()) {
		return false;
	}
	Vector<int> nearby_blockers;
	const uint32_t seen_generation = _next_blocker_seen_generation(r_context);
	for (int target_corner = 0; target_corner < 8; target_corner++) {
		_query_bvh(r_context.bvh, r_context.blocker_indices, 0, p_cell.get_center(), p_candidate.get_endpoint(target_corner), r_context.blocker_seen_generation, seen_generation, nearby_blockers);
	}
	// Sorting restores lexical blocker order after spatial traversal. The seen
	// set above nominates each blocker only once across all corner rays.
	nearby_blockers.sort();
	r_context.preprocess.blocker_nominations += nearby_blockers.size();
	Vector<const TrianglePatch *> nominees;
	for (int blocker_index : nearby_blockers) {
		const BlockerEntry &blocker = r_context.blockers[blocker_index];
		for (const TrianglePatch &patch : blocker.patches) {
			for (int target_corner = 0; target_corner < 8; target_corner++) {
				if (_intersects_patch(patch, p_cell.get_center(), p_candidate.get_endpoint(target_corner), false)) {
					nominees.push_back(&patch);
					break;
				}
			}
			if (nominees.size() >= MAX_PATCH_NOMINEES) {
				break;
			}
		}
		if (nominees.size() >= MAX_PATCH_NOMINEES) {
			break;
		}
	}
	for (const TrianglePatch *patch : nominees) {
		bool source_front = true;
		bool source_back = true;
		bool target_front = true;
		bool target_back = true;
		for (int i = 0; i < 8; i++) {
			const float source_side = patch->normal.dot(p_cell.get_endpoint(i) - _patch_vertex(*patch, 0));
			const float target_side = patch->normal.dot(p_candidate.get_endpoint(i) - _patch_vertex(*patch, 0));
			source_front &= source_side > PATCH_EPSILON;
			source_back &= source_side < -PATCH_EPSILON;
			target_front &= target_side > PATCH_EPSILON;
			target_back &= target_side < -PATCH_EPSILON;
		}
		const bool separates = (source_front && target_back) || (source_back && target_front);
		const bool orientation_matches = patch->source_side == 0 ||
				(patch->source_side > 0 && source_front && target_back) ||
				(patch->source_side < 0 && source_back && target_front);
		if (!separates || !orientation_matches) {
			continue;
		}
		bool complete = true;
		for (int source = 0; source < 8 && complete; source++) {
			for (int target = 0; target < 8; target++) {
				if (!_intersects_patch(*patch, p_cell.get_endpoint(source), p_candidate.get_endpoint(target), true)) {
					complete = false;
					break;
				}
			}
		}
		if (complete) {
			return true;
		}
	}
	return false;
}

static AABB _cell_bounds(const AABB &p_bounds, const Vector3i &p_cell, float p_size) {
	return AABB(p_bounds.position + Vector3(p_cell.x, p_cell.y, p_cell.z) * p_size, Vector3(p_size, p_size, p_size)).intersection(p_bounds).grow(CELL_EPSILON);
}

static int _intern_set(Vector<BakedVisibilityBakeSet> &r_sets, const BakedVisibilityBakeSet &p_set) {
	for (int i = 0; i < r_sets.size(); i++) {
		if (r_sets[i].geometry == p_set.geometry && r_sets[i].lights == p_set.lights) {
			return i;
		}
	}
	r_sets.push_back(p_set);
	return r_sets.size() - 1;
}

static bool _light_reaches_aabb(const LightEntry &p_light, const AABB &p_aabb) {
	if (p_light.directional) {
		return true;
	}
	const Vector3 closest = p_light.position.clamp(p_aabb.position, p_aabb.get_end());
	return closest.distance_to(p_light.position) <= p_light.range;
}

static PackedByteArray _sha256(const String &p_text) {
	PackedByteArray result;
	result.resize(32);
	const CharString utf8 = p_text.utf8();
	CryptoCore::sha256((const uint8_t *)utf8.ptr(), utf8.length(), result.ptrw());
	return result;
}

// ResourceLoader encodes UID dependencies as "uid://...::type::fallback-path".
// Always resolve that representation before using it as a digest input: hashing the
// UID token would make the result depend on the loader's transient UID registry.
static bool _resolve_dependency_path(const String &p_dependency, const String &p_owner_path, String &r_path) {
	String path;
	if (p_dependency.get_slice_count("::") >= 3) {
		const ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(p_dependency.get_slice("::", 0));
		if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid)) {
			path = ResourceUID::get_singleton()->get_id_path(uid);
		} else {
			path = p_dependency.get_slice("::", 2); // Text resource UID fallback path.
		}
	} else {
		path = p_dependency.get_slice("::", 0);
	}

	if (path.is_empty() || path.begins_with("uid://")) {
		return false;
	}
	if (!path.contains("://") && path.is_relative_path()) {
		path = ProjectSettings::get_singleton()->localize_path(p_owner_path.get_base_dir().path_join(path));
	}
	r_path = path.simplify_path();
	return !r_path.is_empty();
}

static bool _collect_dependency_paths(const String &p_dependency, const String &p_owner_path, HashSet<String> &r_seen, Vector<String> &r_paths) {
	String path;
	if (!_resolve_dependency_path(p_dependency, p_owner_path, path)) {
		return false;
	}
	if (path.ends_with(".bvis") || r_seen.has(path)) {
		return true;
	}
	r_seen.insert(path);
	r_paths.push_back(path);
	List<String> dependencies;
	ResourceLoader::get_dependencies(path, &dependencies, false);
	for (const String &dependency : dependencies) {
		if (!_collect_dependency_paths(dependency, path, r_seen, r_paths)) {
			return false;
		}
	}
	return true;
}

static String _quoted_attribute(const String &p_line, const String &p_name) {
	const String attribute = p_name + "=\"";
	int begin = p_line.find(attribute);
	while (begin >= 0) {
		// Attribute names are space-separated in the text scene format. Do not
		// let `id` match the suffix of `uid` in an ext_resource declaration.
		if (begin == 0 || p_line[begin - 1] == ' ' || p_line[begin - 1] == '[') {
			const int value_begin = begin + attribute.length();
			const int value_end = p_line.find("\"", value_begin);
			return value_end < 0 ? String() : p_line.substr(value_begin, value_end - value_begin);
		}
		begin = p_line.find(attribute, begin + 1);
	}
	return String();
}

static String _canonical_scene_digest(const String &p_path, const String &p_read_path = String()) {
	const String read_path = p_read_path.is_empty() ? p_path : p_read_path;
	if (!p_path.ends_with(".tscn")) {
		return FileAccess::get_sha256(read_path);
	}
	Error error = OK;
	const String source = FileAccess::get_file_as_string(read_path, &error);
	if (error != OK) {
		return String();
	}
	const PackedStringArray lines = source.split("\n", false);
	HashSet<String> baked_ids;
	for (const String &line : lines) {
		if (line.contains(".bvis")) {
			const String id = _quoted_attribute(line, "id");
			if (!id.is_empty()) {
				baked_ids.insert(id);
			}
		}
	}
	String canonical;
	for (String line : lines) {
		// Whitespace-only lines are not meaningful in Godot text resources. Remove
		// them after generated linkage stripping so first-save linkage does not
		// change the persisted source digest through separator blank lines.
		if (line.strip_edges().is_empty()) {
			continue;
		}
		if (line.contains(".bvis")) {
			continue;
		}
		if (line.begins_with("[gd_scene ")) {
			const int start = line.find("load_steps=");
			if (start >= 0) {
				int end = line.find(" ", start);
				if (end < 0) {
					end = line.find("]", start);
				}
				if (end > start) {
					const int length = end - start + (end < line.length() && line[end] == ' ' ? 1 : 0);
					line = line.erase(start, length);
				}
			}
		}
		const int reference_start = line.find("data = ExtResource(\"");
		if (reference_start >= 0) {
			const int id_begin = reference_start + 20;
			const int id_end = line.find("\"", id_begin);
			if (id_end > id_begin && baked_ids.has(line.substr(id_begin, id_end - id_begin))) {
				continue;
			}
		}
		canonical += line + "\n";
	}
	return canonical.sha256_text();
}

static PackedByteArray _dependency_digest(const String &p_source_path, const String &p_staged_source_path = String()) {
	HashSet<String> seen;
	Vector<String> paths;
	if (!_collect_dependency_paths(p_source_path, String(), seen, paths)) {
		return PackedByteArray();
	}
	paths.sort_custom<StringPathSort>();
	String aggregate;
	for (const String &path : paths) {
		const String digest = _canonical_scene_digest(path, path == p_source_path ? p_staged_source_path : String());
		if (digest.is_empty()) {
			return PackedByteArray();
		}
		aggregate += path + ":" + digest + "\n";
	}
	return _sha256(aggregate);
}

static Vector<uint32_t> _combined_set(const BakedVisibilityBakeSet &p_set, uint32_t p_light_offset) {
	Vector<uint32_t> result;
	result.reserve(p_set.geometry.size() + p_set.lights.size());
	for (int index : p_set.geometry) {
		result.push_back(index);
	}
	for (int index : p_set.lights) {
		result.push_back(p_light_offset + index);
	}
	result.sort();
	return result;
}

static uint32_t _intern_combined_set(Vector<Vector<uint32_t>> &r_sets, const Vector<uint32_t> &p_set) {
	for (int i = 0; i < r_sets.size(); i++) {
		if (r_sets[i].size() != p_set.size()) {
			continue;
		}
		bool equal = true;
		for (int j = 0; j < p_set.size(); j++) {
			if (r_sets[i][j] != p_set[j]) {
				equal = false;
				break;
			}
		}
		if (equal) {
			return i;
		}
	}
	r_sets.push_back(p_set);
	return r_sets.size() - 1;
}

static void _include_candidate_subtree(const BvhNode &p_node, const Vector<int> &p_indices, BakedVisibilityBakeSet &r_set) {
	for (int i = 0; i < p_node.subtree_count; i++) {
		r_set.geometry.push_back(p_indices[p_node.first + i]);
	}
}

static void _select_primary_candidates(BakeContext &r_context, int p_node_index, const AABB &p_cell_aabb, int p_work_cap, int &r_work, BakedVisibilityBakeCell &r_cell) {
	const BvhNode &node = r_context.candidate_bvh[p_node_index];
	const AABB candidate_bounds = node.aabb.grow(CELL_EPSILON);
	if (r_work + 64 > p_work_cap) {
		r_cell.flags |= BakedVisibilityBakeCell::FLAG_WORK_CAP_FALLBACK;
		if (node.count > 0) {
			_include_candidate_subtree(node, r_context.candidate_indices, r_cell.primary);
		} else {
			_select_primary_candidates(r_context, node.left, p_cell_aabb, p_work_cap, r_work, r_cell);
			_select_primary_candidates(r_context, node.right, p_cell_aabb, p_work_cap, r_work, r_cell);
		}
		return;
	}
	r_work += 64;
	if (_certifies_exclusion(r_context, p_cell_aabb, candidate_bounds)) {
		return;
	}
	if (node.count == 0) {
		_select_primary_candidates(r_context, node.left, p_cell_aabb, p_work_cap, r_work, r_cell);
		_select_primary_candidates(r_context, node.right, p_cell_aabb, p_work_cap, r_work, r_cell);
		return;
	}
	for (int i = 0; i < node.count; i++) {
		const int geometry_index = r_context.candidate_indices[node.first + i];
		if (r_work + 64 > p_work_cap) {
			r_cell.flags |= BakedVisibilityBakeCell::FLAG_WORK_CAP_FALLBACK;
			for (int remaining = i; remaining < node.count; remaining++) {
				r_cell.primary.geometry.push_back(r_context.candidate_indices[node.first + remaining]);
			}
			return;
		}
		r_work += 64;
		if (!_certifies_exclusion(r_context, p_cell_aabb, r_context.geometry[geometry_index].aabb.grow(CELL_EPSILON))) {
			r_cell.primary.geometry.push_back(geometry_index);
		}
	}
}

} // namespace

Error BakedVisibilityBaker::bake(const BakedVisibilityBakeInput &p_input, BakedVisibilityBakeOutput &r_output) const {
	r_output = BakedVisibilityBakeOutput();
	if (!p_input.anchor || !p_input.scene_root || p_input.requested_cell_size <= CMP_EPSILON || p_input.max_cells <= 0 || p_input.max_cells > 65536 || p_input.certificate_work_cap < 64 || p_input.transport_distance <= CMP_EPSILON) {
		r_output.error = "Baked visibility requires a scene root, positive cell and transport distances, at least one cell, and a certificate cap of 64 or more.";
		return ERR_INVALID_PARAMETER;
	}
	BakeContext context;
	context.path_root = p_input.scene_root;
	context.local_from_global = _node_transform(p_input.anchor, Transform3D()).affine_inverse();
	const uint64_t extraction_start_usec = Time::get_singleton()->get_ticks_usec();
	_collect_scene(p_input.scene_root, context, p_input.bake_mask);
	// Candidate metadata is enumerated only after the full traversal has been
	// canonically ordered. Array materialization remains deferred until selection.
	context.geometry.sort_custom<GeometryPathSort>();
	for (const GeometryEntry &geometry : context.geometry) {
		_collect_blocker_surface_candidates(geometry, context, r_output.rejected_blocker_count);
	}
	_select_and_materialize_blocker_surfaces(context, _resolve_max_blocker_triangles(p_input), r_output.rejected_blocker_count);
	context.preprocess.extraction_usec = Time::get_singleton()->get_ticks_usec() - extraction_start_usec;
	context.preprocess.progress |= BakedVisibilityBakeOutput::PreprocessStats::PROGRESS_TRIANGLES_EXTRACTED;
	print_verbose(vformat("Baked visibility: extracted %d source blocker triangles from %d materialized surfaces (%d rejected).", context.preprocess.triangle_count, context.preprocess.blocker_arrays_materialized, context.preprocess.blocker_surfaces_rejected));
	const uint64_t merge_start_usec = Time::get_singleton()->get_ticks_usec();
	for (BlockerEntry &blocker : context.blockers) {
		_merge_coplanar_pairs(blocker.patches, context.preprocess);
	}
	context.preprocess.merge_usec = Time::get_singleton()->get_ticks_usec() - merge_start_usec;
	context.preprocess.progress |= BakedVisibilityBakeOutput::PreprocessStats::PROGRESS_PATCHES_MERGED;
	print_verbose(vformat("Baked visibility: merged %d bounded convex triangle pairs.", context.preprocess.merged_patch_count));
	context.lights.sort_custom<LightPathSort>();
	context.blockers.sort_custom<BlockerPathSort>();
	if (context.geometry.is_empty()) {
		r_output.error = "Baked visibility found no static MeshInstance3D geometry.";
		return ERR_CANT_CREATE;
	}
	for (const GeometryEntry &entry : context.geometry) {
		r_output.geometry_paths.push_back(entry.path);
		r_output.geometry_identities.push_back(entry.identity);
		r_output.geometry_bounds.push_back(entry.aabb);
		bool certified_blocker = false;
		for (const BlockerEntry &blocker : context.blockers) {
			if (blocker.path == entry.path) {
				certified_blocker = true;
				break;
			}
		}
		r_output.geometry_certified_blockers.push_back(certified_blocker);
	}
	for (const LightEntry &entry : context.lights) {
		r_output.light_paths.push_back(entry.path);
		r_output.light_bounds.push_back(AABB(entry.position, Vector3()));
		r_output.light_directional.push_back(entry.directional);
	}
	r_output.static_geometry_count = context.geometry.size();
	r_output.eligible_blocker_count = context.blockers.size();
	context.blocker_indices.resize(context.blockers.size());
	context.blocker_seen_generation.resize(context.blockers.size());
	for (int i = 0; i < context.blocker_indices.size(); i++) {
		context.blocker_indices.write[i] = i;
		context.blocker_seen_generation.write[i] = 0;
	}
	context.candidate_indices.resize(context.geometry.size());
	for (int i = 0; i < context.candidate_indices.size(); i++) {
		context.candidate_indices.write[i] = i;
	}

	AABB bounds = p_input.bounds;
	if (!bounds.is_finite() || bounds.get_volume() <= 0.0f) {
		bounds = context.geometry[0].aabb;
		for (int i = 1; i < context.geometry.size(); i++) {
			bounds = bounds.merge(context.geometry[i].aabb);
		}
	}
	if (bounds.get_volume() <= CMP_EPSILON) {
		r_output.error = "Baked visibility bounds are empty.";
		return ERR_CANT_CREATE;
	}

	float cell_size = p_input.requested_cell_size;
	Vector3 size = bounds.size;
	int nx = MAX(1, Math::ceil(size.x / cell_size));
	int ny = MAX(1, Math::ceil(size.y / cell_size));
	int nz = MAX(1, Math::ceil(size.z / cell_size));
	auto cell_count = [&]() -> int64_t { return int64_t(nx) * int64_t(ny) * int64_t(nz); };
	while (cell_count() > p_input.max_cells) {
		const float scale = Math::pow(float(cell_count()) / float(p_input.max_cells), 1.0f / 3.0f);
		cell_size *= MAX(1.01f, scale);
		nx = MAX(1, Math::ceil(size.x / cell_size));
		ny = MAX(1, Math::ceil(size.y / cell_size));
		nz = MAX(1, Math::ceil(size.z / cell_size));
		if (!Math::is_finite(cell_size)) {
			r_output.error = "Baked visibility could not adapt the cell size to the cell limit.";
			return ERR_CANT_CREATE;
		}
	}
	const uint64_t bvh_start_usec = Time::get_singleton()->get_ticks_usec();
	if (!context.blockers.is_empty()) {
		_order_spatial_bvh_indices(context.blocker_indices, context.blockers, context.preprocess);
		_build_spatial_bvh(context.bvh, context.blocker_indices, context.blockers, 0, context.blockers.size());
	}
	_order_spatial_bvh_indices(context.candidate_indices, context.geometry, context.preprocess);
	_build_spatial_bvh(context.candidate_bvh, context.candidate_indices, context.geometry, 0, context.geometry.size());
	context.preprocess.bvh_usec = Time::get_singleton()->get_ticks_usec() - bvh_start_usec;
	context.preprocess.progress |= BakedVisibilityBakeOutput::PreprocessStats::PROGRESS_BVHS_BUILT;
	print_verbose(vformat("Baked visibility: built deterministic spatial BVHs for %d entries.", context.preprocess.bvh_order_entries));

	r_output.bounds = bounds;
	r_output.cell_size = cell_size;
	r_output.grid_size = Vector3i(nx, ny, nz);
	for (int z = 0; z < nz; z++) {
		for (int y = 0; y < ny; y++) {
			for (int x = 0; x < nx; x++) {
				BakedVisibilityBakeCell cell;
				cell.coordinate = Vector3i(x, y, z);
				const AABB cell_aabb = _cell_bounds(bounds, cell.coordinate, cell_size);
				int work = 0;
				_select_primary_candidates(context, 0, cell_aabb, p_input.certificate_work_cap, work, cell);
				r_output.conservative_fallback |= (cell.flags & BakedVisibilityBakeCell::FLAG_WORK_CAP_FALLBACK) != 0;
				for (int light_index = 0; light_index < context.lights.size(); light_index++) {
					if (_light_reaches_aabb(context.lights[light_index], cell_aabb)) {
						cell.primary.lights.push_back(light_index);
					}
				}

				Vector<AABB> primary_receiver_regions;
				Vector<AABB> transport_regions;
				for (int geometry_index : cell.primary.geometry) {
					primary_receiver_regions.push_back(context.geometry[geometry_index].aabb.grow(p_input.transport_distance * 1.1f));
					transport_regions.push_back(context.geometry[geometry_index].aabb.grow(p_input.transport_distance * 2.2f));
				}
				for (int geometry_index = 0; geometry_index < context.geometry.size(); geometry_index++) {
					for (const AABB &region : transport_regions) {
						if (region.intersects(context.geometry[geometry_index].aabb)) {
							cell.transport.geometry.push_back(geometry_index);
							break;
						}
					}
				}
				for (int light_index = 0; light_index < context.lights.size(); light_index++) {
					const LightEntry &light = context.lights[light_index];
					bool include = light.directional;
					if (!include) {
						for (const AABB &region : primary_receiver_regions) {
							if (_light_reaches_aabb(light, region)) {
								include = true;
								break;
							}
						}
					}
					if (include) {
						cell.transport.lights.push_back(light_index);
					}
				}
				cell.primary_set = _intern_set(r_output.interned_primary_sets, cell.primary);
				cell.transport_set = _intern_set(r_output.interned_transport_sets, cell.transport);
				// The interned pools are the bake output's sole set storage. Retaining
				// these per-cell staging arrays would duplicate large identical lists for
				// every cell in a static scene.
				cell.primary = BakedVisibilityBakeSet();
				cell.transport = BakedVisibilityBakeSet();
				r_output.cells.push_back(cell);
			}
		}
	}
	r_output.preprocess = context.preprocess;
	if (p_input.strict && r_output.conservative_fallback) {
		r_output.error = "Baked visibility strict mode exhausted the deterministic certificate work cap.";
		return ERR_CANT_CREATE;
	}
	return OK;
}

PackedByteArray BakedVisibilityBaker::make_instance_signature(const NodePath &p_path, uint8_t p_kind, const AABB &p_local_bounds, uint32_t p_flags, const String &p_identity) {
	const String text = String(p_path) + "|" + itos(p_kind) + "|" + itos(p_flags) + "|" +
			String::num_real(p_local_bounds.position.x) + "|" + String::num_real(p_local_bounds.position.y) + "|" + String::num_real(p_local_bounds.position.z) + "|" +
			String::num_real(p_local_bounds.size.x) + "|" + String::num_real(p_local_bounds.size.y) + "|" + String::num_real(p_local_bounds.size.z) + "|" + p_identity;
	return _sha256(text);
}

String BakedVisibilityBaker::make_geometry_identity(const MeshInstance3D *p_mesh) {
	if (!p_mesh || p_mesh->get_mesh().is_null()) {
		return "missing";
	}
	Ref<Mesh> mesh = p_mesh->get_mesh();
	String identity = vformat("surfaces=%d", mesh->get_surface_count());
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		Ref<Material> material = p_mesh->get_active_material(surface);
		const BaseMaterial3D *base = material.is_valid() ? Object::cast_to<BaseMaterial3D>(material.ptr()) : nullptr;
		identity += vformat("|%d:%d:%d:%d:%d", mesh->surface_get_primitive_type(surface), mesh->surface_get_array_len(surface), mesh->surface_get_array_index_len(surface), base ? int(base->get_transparency()) : -1, base ? int(base->get_cull_mode()) : -1);
	}
	return identity;
}

PackedByteArray BakedVisibilityBaker::make_source_digest(const String &p_source_path, const String &p_staged_source_path) {
	return _dependency_digest(p_source_path, p_staged_source_path);
}

Error BakedVisibilityBaker::build_data(const BakedVisibilityBakeInput &p_input, const BakedVisibilityBakeOutput &p_output, BakedVisibilityData3DData &r_data, String *r_error) const {
	r_data = BakedVisibilityData3DData();
	if (p_output.cells.is_empty() || p_output.geometry_paths.size() != p_output.geometry_bounds.size() || p_output.geometry_paths.size() != p_output.geometry_identities.size() || p_output.geometry_paths.size() != p_output.geometry_certified_blockers.size() || p_output.light_paths.size() != p_output.light_bounds.size() || p_output.light_paths.size() != p_output.light_directional.size()) {
		if (r_error) {
			*r_error = "Baked visibility output is incomplete.";
		}
		return ERR_INVALID_DATA;
	}
	for (const BakedVisibilityBakeCell &bake_cell : p_output.cells) {
		if (bake_cell.primary_set >= p_output.interned_primary_sets.size() || bake_cell.transport_set >= p_output.interned_transport_sets.size()) {
			if (r_error) {
				*r_error = "Baked visibility output references an invalid interned set.";
			}
			return ERR_INVALID_DATA;
		}
	}
	r_data.source_path = p_input.source_path;
	if (r_data.source_path.is_empty() && p_input.scene_root) {
		r_data.source_path = p_input.scene_root->get_scene_file_path();
	}
	if (r_data.source_path.is_empty()) {
		if (r_error) {
			*r_error = "Baked visibility can only be written for a saved scene.";
		}
		return ERR_CANT_CREATE;
	}
	r_data.source_sha256 = make_source_digest(r_data.source_path);
	if (r_data.source_sha256.size() != 32) {
		if (r_error) {
			*r_error = vformat("Could not read baked visibility source scene '%s'.", r_data.source_path);
		}
		return ERR_FILE_CANT_READ;
	}
	r_data.source_uid = ResourceLoader::get_resource_uid(r_data.source_path);
	if (r_data.source_uid == ResourceUID::INVALID_ID) {
		if (r_error) {
			*r_error = vformat("Baked visibility source scene '%s' has no resource UID.", r_data.source_path);
		}
		return ERR_CANT_CREATE;
	}
	r_data.local_bounds = p_output.bounds;
	r_data.cell_size = Vector3(p_output.cell_size, p_output.cell_size, p_output.cell_size);
	r_data.grid_size = p_output.grid_size;
	r_data.transport_distance = p_input.transport_distance;
	r_data.bake_mask = p_input.bake_mask;
	r_data.lookup_margin = p_input.lookup_margin;
	r_data.coverage_tolerance = PATCH_EPSILON;
	r_data.transport_tolerance = 0.1f;
	for (int i = 0; i < p_output.geometry_paths.size(); i++) {
		BakedVisibilityData3DData::Instance instance;
		instance.path = NodePath(p_output.geometry_paths[i]);
		instance.kind = BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY;
		instance.flags = p_output.geometry_certified_blockers[i] ? BakedVisibilityData3DData::INSTANCE_FLAG_CERTIFIED_BLOCKER : 0;
		instance.local_bounds = p_output.geometry_bounds[i];
		instance.signature_sha256 = make_instance_signature(instance.path, instance.kind, instance.local_bounds, instance.flags, String::hex_encode_buffer(r_data.source_sha256.ptr(), r_data.source_sha256.size()) + ":" + p_output.geometry_identities[i]);
		r_data.instances.push_back(instance);
	}
	for (int i = 0; i < p_output.light_paths.size(); i++) {
		BakedVisibilityData3DData::Instance instance;
		instance.path = NodePath(p_output.light_paths[i]);
		instance.kind = p_output.light_directional[i] ? BakedVisibilityData3DData::INSTANCE_KIND_DIRECTIONAL_LIGHT : BakedVisibilityData3DData::INSTANCE_KIND_POSITIONAL_LIGHT;
		instance.local_bounds = p_output.light_bounds[i];
		instance.signature_sha256 = make_instance_signature(instance.path, instance.kind, instance.local_bounds, instance.flags, String::hex_encode_buffer(r_data.source_sha256.ptr(), r_data.source_sha256.size()));
		r_data.instances.push_back(instance);
	}
	r_data.sets.push_back(Vector<uint32_t>());
	const uint32_t light_offset = p_output.geometry_paths.size();
	Vector<uint32_t> primary_set_map;
	primary_set_map.resize(p_output.interned_primary_sets.size());
	for (int i = 0; i < p_output.interned_primary_sets.size(); i++) {
		primary_set_map.write[i] = _intern_combined_set(r_data.sets, _combined_set(p_output.interned_primary_sets[i], light_offset));
	}
	Vector<uint32_t> transport_set_map;
	transport_set_map.resize(p_output.interned_transport_sets.size());
	for (int i = 0; i < p_output.interned_transport_sets.size(); i++) {
		transport_set_map.write[i] = _intern_combined_set(r_data.sets, _combined_set(p_output.interned_transport_sets[i], light_offset));
	}
	for (const BakedVisibilityBakeCell &bake_cell : p_output.cells) {
		BakedVisibilityData3DData::Cell cell;
		cell.flags = bake_cell.flags ? BakedVisibilityData3DData::CELL_FLAG_DEGRADED : 0;
		cell.primary_set = primary_set_map[bake_cell.primary_set];
		cell.transport_set = transport_set_map[bake_cell.transport_set];
		r_data.cells.push_back(cell);
	}
	r_data.report = vformat("static_geometry=%d blockers=%d rejected_blockers=%d selected_surfaces=%d selected_triangles=%d rejected_surfaces=%d rejected_triangles=%d arrays_materialized=%d cells=%d degraded=%s", p_output.static_geometry_count, p_output.eligible_blocker_count, p_output.rejected_blocker_count, p_output.preprocess.blocker_surfaces_selected, p_output.preprocess.blocker_triangles_selected, p_output.preprocess.blocker_surfaces_rejected, p_output.preprocess.blocker_triangles_rejected, p_output.preprocess.blocker_arrays_materialized, p_output.cells.size(), p_output.conservative_fallback ? "true" : "false");
	// The codec owns the canonical instance/set ordering and set-index remap.
	// Build output groups geometry and lights for collection efficiency, so it
	// is not necessarily lexical across the unified instance table. Round-trip
	// through the canonical codec before returning data that callers validate.
	PackedByteArray canonical_bytes;
	String canonical_error;
	if (BakedVisibilityCodec::encode(r_data, canonical_bytes, &canonical_error) != OK) {
		if (r_error) {
			*r_error = canonical_error;
		}
		return ERR_INVALID_DATA;
	}
	BakedVisibilityData3DData canonical_data;
	if (BakedVisibilityCodec::decode(canonical_bytes, canonical_data, &canonical_error) != OK) {
		if (r_error) {
			*r_error = canonical_error;
		}
		return ERR_INVALID_DATA;
	}
	r_data = canonical_data;
	return OK;
}
