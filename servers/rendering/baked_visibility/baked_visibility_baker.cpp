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
#include "core/os/mutex.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/object/worker_thread_pool.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"

#include <memory>
#include <limits>

namespace {

// Worker tasks update this reporter only after publishing their deterministic
// tile slot. Reporting is intentionally coarse and serialized around the
// engine print call so progress cannot perturb the bake or interleave lines.
struct BakeProgressReporter {
	String phase;
	uint32_t total = 0;
	uint32_t interval = 1;
	std::atomic<uint32_t> completed = 0;
	std::atomic<uint32_t> next_report = 1;
	uint64_t started_usec = 0;
	BinaryMutex output_mutex;

	BakeProgressReporter(const char *p_phase, uint32_t p_total) {
		phase = p_phase;
		total = p_total;
		interval = MAX(1u, (p_total + 19u) / 20u);
		next_report.store(MIN(interval, MAX(1u, p_total)), std::memory_order_relaxed);
		started_usec = Time::get_singleton()->get_ticks_usec();
		print_line(vformat("Baked visibility: %s 0/%d.", phase, total));
	}

	void report(uint32_t p_completed) {
		if (total == 0) {
			return;
		}
		uint32_t threshold = next_report.load(std::memory_order_relaxed);
		while (p_completed >= threshold && threshold <= total) {
			const uint32_t next = MIN(total + 1u, threshold + interval);
			if (next_report.compare_exchange_weak(threshold, next, std::memory_order_relaxed)) {
				MutexLock lock(output_mutex);
				const double elapsed = double(Time::get_singleton()->get_ticks_usec() - started_usec) / 1000000.0;
				const double eta = p_completed > 0 ? elapsed * (double(total) / double(p_completed) - 1.0) : 0.0;
				print_line(vformat("Baked visibility: %s %d/%d (elapsed %.1fs, ETA %.1fs).", phase, p_completed, total, elapsed, MAX(0.0, eta)));
				break;
			}
		}
	}

	void finish(uint32_t p_completed) {
		if (p_completed == 0 && total == 0) {
			return;
		}
		MutexLock lock(output_mutex);
		const double elapsed = double(Time::get_singleton()->get_ticks_usec() - started_usec) / 1000000.0;
		print_line(vformat("Baked visibility: %s %d/%d complete (elapsed %.1fs).", phase, p_completed, total, elapsed));
	}
};

constexpr float CELL_EPSILON = 0.001f;
constexpr float PATCH_EPSILON = 0.0001f;
constexpr int BVH_LEAF_SIZE = 8;
constexpr int MAX_PATCH_NOMINEES = 8;
constexpr int MAX_BLOCKER_TRIANGLES = 1048576;

// This tracks allocations owned by the offline baker only. It deliberately
// excludes renderer and process allocations, but every logical snapshot,
// materialization, worker scratch, result, checkpoint, and batch allocation
// is admitted before the corresponding Vector/mesh operation is attempted.
struct OwnedMemoryBudget {
	uint64_t used = 0;
	uint64_t budget = 0;
	String failure;

	explicit OwnedMemoryBudget(uint64_t p_budget) : budget(p_budget) {}

	bool reserve(uint64_t p_count, uint64_t p_size, const char *p_stage) {
		uint64_t requested = 0;
		if (p_count != 0 && p_size > std::numeric_limits<uint64_t>::max() / p_count) {
			failure = "Baked visibility owned memory cap exceeded at " + String(p_stage) + " (requested=overflow bytes, used=" + String::num_uint64(used) + " bytes, budget=" + String::num_uint64(budget) + " bytes).";
			return false;
		}
		requested = p_count * p_size;
		if (budget == 0 || requested > budget - MIN(used, budget)) {
			failure = "Baked visibility owned memory cap exceeded at " + String(p_stage) + " (requested=" + String::num_uint64(requested) + " bytes, used=" + String::num_uint64(used) + " bytes, budget=" + String::num_uint64(budget) + " bytes).";
			return false;
		}
		used += requested;
		return true;
	}

	bool reserve_product(uint64_t p_first, uint64_t p_second, uint64_t p_size, const char *p_stage) {
		if (p_first != 0 && p_second > std::numeric_limits<uint64_t>::max() / p_first) {
			failure = "Baked visibility owned memory cap exceeded at " + String(p_stage) + " (requested=overflow bytes, used=" + String::num_uint64(used) + " bytes, budget=" + String::num_uint64(budget) + " bytes).";
			return false;
		}
		return reserve(p_first * p_second, p_size, p_stage);
	}
};

struct GeometryEntry {
	String path;
	String identity;
	AABB aabb;
	MeshInstance3D *instance = nullptr;
};

struct LightEntry {
	String path;
	Vector3 position;
	Vector3 direction;
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
	int vertex_count = 0;
	int element_count = 0;
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
	OwnedMemoryBudget *memory = nullptr;
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

static bool _collect_blocker_surface_candidates(const GeometryEntry &p_geometry, BakeContext &r_context, int &r_rejected_blockers) {
	Ref<Mesh> mesh = p_geometry.instance->get_mesh();
	if (mesh.is_null()) {
		return true;
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
		if (!r_context.memory->reserve(1, sizeof(BlockerSurfaceCandidate) + p_geometry.path.length(), "blocker surface metadata")) {
			return false;
		}
		candidate.path = p_geometry.path;
		candidate.instance = p_geometry.instance;
		candidate.aabb = p_geometry.aabb;
		candidate.surface = surface;
		candidate.triangle_count = triangle_count;
		candidate.vertex_count = mesh->surface_get_array_len(surface);
		candidate.element_count = (mesh->surface_get_format(surface) & Mesh::ARRAY_FORMAT_INDEX) ? mesh->surface_get_array_index_len(surface) : candidate.vertex_count;
		candidate.usefulness_score = quantized_area / uint64_t(triangle_count);
		r_context.blocker_surface_candidates.push_back(candidate);
		r_context.preprocess.blocker_surface_candidates++;
	}
	return true;
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

static bool _select_and_materialize_blocker_surfaces(BakeContext &r_context, int p_max_blocker_triangles, int &r_rejected_blockers) {
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
		if (!r_context.memory->reserve(1, sizeof(BlockerSurfaceCandidate), "blocker selection")) {
			return false;
		}
		selected.push_back(candidate);
		remaining_triangles -= candidate.triangle_count;
	}
	selected.sort_custom<BlockerSurfacePathSort>();
	uint64_t materialization_remaining_triangles = p_max_blocker_triangles;
	for (const BlockerSurfaceCandidate &candidate : selected) {
		if (r_context.blockers.is_empty() || r_context.blockers[r_context.blockers.size() - 1].path != candidate.path) {
			if (!r_context.memory->reserve(1, sizeof(BlockerEntry) + candidate.path.length(), "blocker metadata")) {
				return false;
			}
			BlockerEntry blocker;
			blocker.path = candidate.path;
			blocker.aabb = AABB();
			r_context.blockers.push_back(blocker);
		}
		BlockerEntry &blocker = r_context.blockers.write[r_context.blockers.size() - 1];
		const uint64_t arrays_and_patches = uint64_t(candidate.vertex_count) * sizeof(Vector3) + uint64_t(candidate.element_count) * sizeof(int32_t) + uint64_t(candidate.triangle_count) * sizeof(TrianglePatch);
		if (!r_context.memory->reserve(1, arrays_and_patches, "blocker mesh materialization")) {
			return false;
		}
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
	return true;
}

static bool _collect_scene(Node *p_node, BakeContext &r_context, uint32_t p_bake_mask) {
	if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node)) {
		if ((mesh->get_layer_mask() & p_bake_mask) != 0 && _is_static_mesh(mesh)) {
			if (!r_context.memory->reserve(1, sizeof(GeometryEntry) + 512, "scene geometry extraction")) {
				return false;
			}
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
			if (!r_context.memory->reserve(1, sizeof(LightEntry) + 256, "scene light extraction")) {
				return false;
			}
			LightEntry entry;
			entry.path = String(r_context.path_root->get_path_to(light));
			const Transform3D light_transform = _node_transform(light, r_context.local_from_global);
			entry.position = light_transform.origin;
			entry.direction = -light_transform.basis.get_column(2).normalized();
			entry.directional = light->get_light_type() == RSE::LIGHT_DIRECTIONAL;
			entry.range = entry.directional ? 0.0f : MAX(0.0f, light->get_param(Light3D::PARAM_RANGE));
			r_context.lights.push_back(entry);
		}
	}
	for (int child = 0; child < p_node->get_child_count(); child++) {
		if (!_collect_scene(p_node->get_child(child), r_context, p_bake_mask)) {
			return false;
		}
	}
	return true;
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

static bool _certifies_exclusion(BakeContext &r_context, const AABB &p_cell, const AABB &p_candidate, int *r_certificate_blocker = nullptr) {
	if (r_certificate_blocker) {
		*r_certificate_blocker = -1;
	}
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
	struct PatchNominee {
		const TrianglePatch *patch = nullptr;
		int blocker = -1;
	};
	Vector<PatchNominee> nominees;
	for (int blocker_index : nearby_blockers) {
		const BlockerEntry &blocker = r_context.blockers[blocker_index];
		for (const TrianglePatch &patch : blocker.patches) {
			for (int target_corner = 0; target_corner < 8; target_corner++) {
				if (_intersects_patch(patch, p_cell.get_center(), p_candidate.get_endpoint(target_corner), false)) {
					nominees.push_back({ &patch, blocker_index });
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
	for (const PatchNominee &nominee : nominees) {
		const TrianglePatch *patch = nominee.patch;
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
			if (r_certificate_blocker) {
				*r_certificate_blocker = nominee.blocker;
			}
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
	int certificate_blocker = -1;
	if (_certifies_exclusion(r_context, p_cell_aabb, candidate_bounds, &certificate_blocker)) {
		if (certificate_blocker >= 0) {
			r_cell.certificate_blockers.push_back(certificate_blocker);
		}
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
		certificate_blocker = -1;
		if (!_certifies_exclusion(r_context, p_cell_aabb, r_context.geometry[geometry_index].aabb.grow(CELL_EPSILON), &certificate_blocker)) {
			r_cell.primary.geometry.push_back(geometry_index);
		} else if (certificate_blocker >= 0) {
			r_cell.certificate_blockers.push_back(certificate_blocker);
		}
	}
}

struct TileDependencySet {
	Vector<uint32_t> candidates;
	Vector<uint32_t> lights;
	Vector<uint32_t> certificates;

	void finalize() {
		for (Vector<uint32_t> *values : { &candidates, &lights, &certificates }) {
			values->sort();
			for (int index = values->size() - 1; index > 0; index--) {
				if ((*values)[index] == (*values)[index - 1]) {
					values->remove_at(index);
				}
			}
		}
	}
};

static void _append_certificate_geometry(const BakeContext &p_context, int p_blocker_index, TileDependencySet &r_dependencies) {
	if (p_blocker_index < 0 || p_blocker_index >= p_context.blockers.size()) {
		return;
	}
	const String &blocker_path = p_context.blockers[p_blocker_index].path;
	for (int geometry_index = 0; geometry_index < p_context.geometry.size(); geometry_index++) {
		if (p_context.geometry[geometry_index].path == blocker_path) {
			r_dependencies.certificates.push_back(geometry_index);
			return;
		}
	}
}

static void _append_cell_dependencies(const BakeContext &p_context, const BakedVisibilityBakeCell &p_cell, TileDependencySet &r_dependencies) {
	for (int geometry_index : p_cell.primary.geometry) r_dependencies.candidates.push_back(geometry_index);
	for (int geometry_index : p_cell.transport.geometry) r_dependencies.candidates.push_back(geometry_index);
	for (int light_index : p_cell.primary.lights) r_dependencies.lights.push_back(light_index);
	for (int light_index : p_cell.transport.lights) r_dependencies.lights.push_back(light_index);
	for (int blocker_index : p_cell.certificate_blockers) _append_certificate_geometry(p_context, blocker_index, r_dependencies);
}

struct CellBuildTask {
	BakeContext *base = nullptr;
	const BakedVisibilityBakeInput *input = nullptr;
	AABB bounds;
	float cell_size = 0.0f;
	int nx = 0;
	int ny = 0;
	Vector<BakedVisibilityBakeCell> *cells = nullptr;
	Vector<BakedVisibilityBakeOutput::PreprocessStats> *cell_stats = nullptr;
	const Vector<BakedVisibilityData3DData::Tile> *tiles = nullptr;
	const Vector<uint32_t> *tile_cell_indices = nullptr;
	Vector<TileDependencySet> *tile_dependencies = nullptr;
	std::atomic<uint8_t> *completed_tiles = nullptr;
	Vector<uint8_t> *completed_cell_bitmap = nullptr;
	std::atomic_bool *cancel_flag = nullptr;
	std::atomic<uint32_t> *completed_tiles_count = nullptr;
	BakeProgressReporter *progress = nullptr;
	// Immutable broad discovery is observed before the complete CPU certificate.
	// It may inform diagnostics, but never authorizes candidate removal.
	const BakedVisibilityBackendBatchOutput *backend_batch = nullptr;
	uint32_t global_index = UINT32_MAX;
	uint32_t write_index = UINT32_MAX;
};

static void _build_cell(void *p_userdata, uint32_t p_index) {
	CellBuildTask *task = static_cast<CellBuildTask *>(p_userdata);
	if (task->cancel_flag && task->cancel_flag->load(std::memory_order_relaxed)) {
		return;
	}
	const uint32_t cell_index = task->global_index == UINT32_MAX ? p_index : task->global_index;
	const uint32_t output_index = task->write_index == UINT32_MAX ? p_index : task->write_index;
	const int z = int(cell_index) / (task->nx * task->ny);
	const int y = (int(cell_index) / task->nx) % task->ny;
	const int x = int(cell_index) % task->nx;
	BakeContext local = *task->base;
	const uint64_t base_merge_vertex_visits = local.preprocess.merge_vertex_visits;
	// The BVH and scene snapshot are immutable.  Only the seen-generation array
	// and per-cell counters are worker-local, so no lock or nondeterministic merge
	// is needed.
	// resize() alone may retain the copied CowData when the size is unchanged;
	// clear first so every worker owns an independent seen-generation array.
	local.blocker_seen_generation = Vector<uint32_t>();
	local.blocker_seen_generation.resize(local.blockers.size());
	for (int i = 0; i < local.blocker_seen_generation.size(); i++) local.blocker_seen_generation.write[i] = 0;
	local.blocker_seen_epoch = 0;
	BakedVisibilityBakeCell cell;
	cell.coordinate = Vector3i(x, y, z);
	const AABB cell_aabb = _cell_bounds(task->bounds, cell.coordinate, task->cell_size);
	if (task->backend_batch) {
		for (uint8_t discovered : task->backend_batch->candidate_mask) {
			local.preprocess.backend_candidate_hints += discovered ? 1 : 0;
		}
		for (uint8_t hit : task->backend_batch->hardware_blocker_hit_hints) {
			local.preprocess.backend_hardware_blocker_hints += hit ? 1 : 0;
		}
		// The next traversal remains the fail-open CPU authority.
	}
	int work = 0;
	_select_primary_candidates(local, 0, cell_aabb, task->input->certificate_work_cap, work, cell);
	for (int light_index = 0; light_index < local.lights.size(); light_index++) {
		if (_light_reaches_aabb(local.lights[light_index], cell_aabb)) cell.primary.lights.push_back(light_index);
	}
	Vector<AABB> primary_receiver_regions;
	Vector<AABB> transport_regions;
	for (int geometry_index : cell.primary.geometry) {
		primary_receiver_regions.push_back(local.geometry[geometry_index].aabb.grow(task->input->transport_distance * 1.1f));
		transport_regions.push_back(local.geometry[geometry_index].aabb.grow(task->input->transport_distance * 2.2f));
	}
	for (int geometry_index = 0; geometry_index < local.geometry.size(); geometry_index++) {
		for (const AABB &region : transport_regions) {
			if (region.intersects(local.geometry[geometry_index].aabb)) {
				cell.transport.geometry.push_back(geometry_index);
				break;
			}
		}
	}
	for (int light_index = 0; light_index < local.lights.size(); light_index++) {
		const LightEntry &light = local.lights[light_index];
		bool include = light.directional;
		if (!include) {
			for (const AABB &region : primary_receiver_regions) {
				if (_light_reaches_aabb(light, region)) {
					include = true;
					break;
				}
			}
		}
		if (include) cell.transport.lights.push_back(light_index);
	}
	task->cells->write[output_index] = cell;
	if (task->cell_stats) {
		local.preprocess.merge_vertex_visits -= MIN(local.preprocess.merge_vertex_visits, base_merge_vertex_visits);
		task->cell_stats->write[output_index] = local.preprocess;
	}
}

static void _build_tile(void *p_userdata, uint32_t p_tile_index) {
	CellBuildTask *task = static_cast<CellBuildTask *>(p_userdata);
	if (!task->completed_tiles || task->completed_tiles[p_tile_index].load(std::memory_order_acquire) || (task->cancel_flag && task->cancel_flag->load(std::memory_order_relaxed))) return;
	BakeContext local = *task->base;
	local.blocker_seen_generation = Vector<uint32_t>();
	local.blocker_seen_generation.resize(local.blockers.size());
	for (int i = 0; i < local.blocker_seen_generation.size(); i++) local.blocker_seen_generation.write[i] = 0;
	local.blocker_seen_epoch = 0;
	const BakedVisibilityData3DData::Tile &tile = (*task->tiles)[p_tile_index];
	for (uint32_t offset = 0; offset < tile.cell_count; offset++) {
		if (task->cancel_flag && task->cancel_flag->load(std::memory_order_relaxed)) return;
		const uint32_t cell_index = (*task->tile_cell_indices)[tile.first_cell + offset];
		CellBuildTask one = *task;
		one.base = &local;
		one.cells = task->cells;
		one.cell_stats = task->cell_stats;
		one.global_index = cell_index;
		one.write_index = cell_index;
		_build_cell(&one, cell_index);
		if (task->progress) task->progress->report(task->progress->completed.fetch_add(1, std::memory_order_relaxed) + 1);
	}
	if (task->cancel_flag && task->cancel_flag->load(std::memory_order_relaxed)) return;
	if (task->tile_dependencies) {
		TileDependencySet dependencies;
		for (uint32_t offset = 0; offset < tile.cell_count; offset++) {
			const BakedVisibilityBakeCell &cell = (*task->cells)[(*task->tile_cell_indices)[tile.first_cell + offset]];
			_append_cell_dependencies(*task->base, cell, dependencies);
		}
		dependencies.finalize();
		(*task->tile_dependencies).write[p_tile_index] = dependencies;
	}
	if (task->completed_cell_bitmap) {
		for (uint32_t offset = 0; offset < tile.cell_count; offset++) {
			const uint32_t cell_index = (*task->tile_cell_indices)[tile.first_cell + offset];
			task->completed_cell_bitmap->write[cell_index] = 1;
		}
	}
	task->completed_tiles[p_tile_index].store(1, std::memory_order_release);
	if (task->completed_tiles_count) task->completed_tiles_count->fetch_add(1, std::memory_order_relaxed);
}

static PackedByteArray _tile_dependency_signature(const BakeContext &p_context, const Vector3i &p_grid_size, const BakedVisibilityData3DData::Tile &p_tile, const TileDependencySet &p_dependencies) {
	String dependency = vformat("bvis-tile|%d|%d|%d|%d|%d|%d|", p_tile.coordinate.x, p_tile.coordinate.y, p_tile.coordinate.z, p_grid_size.x, p_grid_size.y, p_grid_size.z);
	auto aabb_text = [](const AABB &aabb) { return String::num_real(aabb.position.x) + "," + String::num_real(aabb.position.y) + "," + String::num_real(aabb.position.z) + "," + String::num_real(aabb.size.x) + "," + String::num_real(aabb.size.y) + "," + String::num_real(aabb.size.z); };
	for (uint32_t geometry_index : p_dependencies.candidates) {
		if (geometry_index >= uint32_t(p_context.geometry.size())) {
			continue;
		}
		const GeometryEntry &geometry = p_context.geometry[geometry_index];
		dependency += "|g:" + geometry.path + ":" + geometry.identity + ":" + aabb_text(geometry.aabb);
	}
	for (uint32_t geometry_index : p_dependencies.certificates) {
		if (geometry_index >= uint32_t(p_context.geometry.size())) {
			continue;
		}
		const GeometryEntry &geometry = p_context.geometry[geometry_index];
		dependency += "|b:" + geometry.path + ":" + geometry.identity + ":" + aabb_text(geometry.aabb);
	}
	for (uint32_t light_index : p_dependencies.lights) {
		if (light_index >= uint32_t(p_context.lights.size())) {
			continue;
		}
		const LightEntry &light = p_context.lights[light_index];
		dependency += "|l:" + light.path + ":" + aabb_text(AABB(light.position, Vector3())) + ":" + aabb_text(AABB(light.direction, Vector3())) + ":" + String::num_real(light.range) + ":" + (light.directional ? "1" : "0");
	}
	return _sha256(dependency);
}

static bool _remap_checkpoint_members(PackedInt32Array &r_members, const Vector<int> &p_remap) {
	for (int member = 0; member < r_members.size(); member++) {
		const int old_index = r_members[member];
		if (old_index < 0 || old_index >= p_remap.size() || p_remap[old_index] < 0) {
			return false;
		}
		r_members.set(member, p_remap[old_index]);
	}
	r_members.sort();
	return true;
}

static bool _remap_checkpoint_cell(const BakedVisibilityBakeCell &p_source, const Vector<int> &p_geometry_remap, const Vector<int> &p_light_remap, BakedVisibilityBakeCell &r_cell) {
	r_cell = p_source;
	return _remap_checkpoint_members(r_cell.primary.geometry, p_geometry_remap) &&
			_remap_checkpoint_members(r_cell.transport.geometry, p_geometry_remap) &&
			_remap_checkpoint_members(r_cell.primary.lights, p_light_remap) &&
			_remap_checkpoint_members(r_cell.transport.lights, p_light_remap);
}

} // namespace

Error BakedVisibilityBaker::bake(const BakedVisibilityBakeInput &p_input, BakedVisibilityBakeOutput &r_output) const {
	r_output = BakedVisibilityBakeOutput();
	if (!p_input.anchor || !p_input.scene_root || p_input.requested_cell_size <= CMP_EPSILON || p_input.max_cells <= 0 || p_input.max_cells > 65536 || p_input.certificate_work_cap < 64 || p_input.transport_distance <= CMP_EPSILON) {
		r_output.error = "Baked visibility requires a scene root, positive cell and transport distances, at least one cell, and a certificate cap of 64 or more.";
		return ERR_INVALID_PARAMETER;
	}
	OwnedMemoryBudget memory(p_input.max_memory_bytes);
	BakeContext context;
	context.memory = &memory;
	context.path_root = p_input.scene_root;
	context.local_from_global = _node_transform(p_input.anchor, Transform3D()).affine_inverse();
	const uint64_t extraction_start_usec = Time::get_singleton()->get_ticks_usec();
	if (!_collect_scene(p_input.scene_root, context, p_input.bake_mask)) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
	// Candidate metadata is enumerated only after the full traversal has been
	// canonically ordered. Array materialization remains deferred until selection.
	context.geometry.sort_custom<GeometryPathSort>();
	for (const GeometryEntry &geometry : context.geometry) {
		if (!_collect_blocker_surface_candidates(geometry, context, r_output.rejected_blocker_count)) {
			r_output.error = memory.failure;
			return ERR_OUT_OF_MEMORY;
		}
	}
	const int max_blocker_triangles = _resolve_max_blocker_triangles(p_input);
	if (!_select_and_materialize_blocker_surfaces(context, max_blocker_triangles, r_output.rejected_blocker_count)) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
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
	if (!memory.reserve(context.geometry.size(), sizeof(String) * 2 + sizeof(AABB) + sizeof(bool), "canonical geometry snapshot") || !memory.reserve(context.lights.size(), sizeof(String) + sizeof(AABB) + sizeof(Vector3) * 2 + sizeof(float) + sizeof(bool), "canonical light snapshot")) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
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
		r_output.light_directions.push_back(entry.direction);
		r_output.light_ranges.push_back(entry.range);
		r_output.light_directional.push_back(entry.directional);
	}
	r_output.static_geometry_count = context.geometry.size();
	r_output.eligible_blocker_count = context.blockers.size();
	if (!memory.reserve(context.blockers.size(), sizeof(int) + sizeof(uint32_t), "blocker traversal indices") || !memory.reserve(context.geometry.size(), sizeof(int), "candidate traversal indices")) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
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
	const uint64_t bvh_node_count = uint64_t(context.blockers.size()) * 2u + uint64_t(context.geometry.size()) * 2u;
	if (!memory.reserve(bvh_node_count, sizeof(BvhNode), "BVH nodes")) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
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
	const int total_cells = nx * ny * nz;
	const int tile_nx = (nx + 7) / 8;
	const int tile_ny = (ny + 7) / 8;
	const int tile_nz = (nz + 7) / 8;
	r_output.tile_count = tile_nx * tile_ny * tile_nz; // Leaf task count.
	const uint64_t maximum_tile_count = uint64_t(r_output.tile_count) * 2u;
	if (!memory.reserve(maximum_tile_count, sizeof(BakedVisibilityData3DData::Tile), "tile hierarchy") || !memory.reserve(total_cells, sizeof(uint32_t), "tile cell index pool")) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
	String hierarchy_error;
	if (BakedVisibilityCodec::build_tile_hierarchy(Vector3i(nx, ny, nz), r_output.tile_grid_size, r_output.hierarchy_depth, r_output.tiles, r_output.tile_cell_indices, &hierarchy_error) != OK) {
		r_output.error = hierarchy_error;
		return ERR_CANT_CREATE;
	}
	// Reserve worker, result/checkpoint, and accelerator ownership before any
	// variable result vector or backend buffer is materialized. The tracker is
	// intentionally about baker-owned logical allocations, not process memory.
	const uint64_t worker_count = WorkerThreadPool::get_singleton() ? uint64_t(MAX(1, WorkerThreadPool::get_singleton()->get_thread_count())) : 1u;
	if (!memory.reserve_product(worker_count, context.blockers.size(), sizeof(uint32_t), "per-worker blocker scratch") ||
			!memory.reserve_product(worker_count, context.geometry.size(), sizeof(int) * 2u, "per-worker geometry scratch") ||
			!memory.reserve_product(worker_count, context.lights.size(), sizeof(int) * 2u, "per-worker light scratch") ||
			!memory.reserve(total_cells, sizeof(BakedVisibilityBakeCell) * 2u + sizeof(BakedVisibilityBakeOutput::PreprocessStats) + sizeof(BakedVisibilityData3DData::Cell), "cell results and checkpoint") ||
			!memory.reserve_product(total_cells, context.geometry.size(), sizeof(int32_t) * 4u, "variable geometry candidate vectors") ||
			!memory.reserve_product(total_cells, context.lights.size(), sizeof(int32_t) * 4u, "variable light candidate vectors") ||
			!memory.reserve_product(r_output.tile_count, context.geometry.size(), sizeof(uint32_t), "tile geometry dependency unions") ||
			!memory.reserve_product(r_output.tile_count, context.lights.size(), sizeof(uint32_t), "tile light dependency unions") ||
			!memory.reserve_product(r_output.tile_count, context.blockers.size(), sizeof(uint32_t), "tile blocker dependency unions") ||
			!memory.reserve(context.geometry.size(), sizeof(BakedVisibilityBackendCandidate) + sizeof(uint32_t) * 4u + sizeof(uint8_t) * 3u, "accelerator candidate batch") ||
			!memory.reserve(context.blockers.size(), sizeof(BakedVisibilityBackendBlocker), "accelerator blocker batch") ||
			!memory.reserve(r_output.tile_count, sizeof(uint8_t) + sizeof(std::atomic<uint8_t>), "tile completion bitmap")) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
	Vector<BakedVisibilityBakeCell> staged_cells;
	staged_cells.resize(total_cells);
	// Acceleration receives only immutable snapshot bounds. The batch result is
	// observed before worker certification and cannot remove a candidate.
	BakedVisibilityBackendBatchInput backend_input;
	backend_input.query_bounds = bounds;
	backend_input.candidates.resize(context.geometry.size());
	for (uint32_t geometry_index = 0; geometry_index < context.geometry.size(); geometry_index++) {
		BakedVisibilityBackendCandidate &candidate = backend_input.candidates.write[geometry_index];
		candidate.bounds = context.geometry[geometry_index].aabb;
		candidate.canonical_index = geometry_index;
	}
	backend_input.blockers.resize(context.blockers.size());
	for (uint32_t blocker_index = 0; blocker_index < context.blockers.size(); blocker_index++) {
		backend_input.blockers.write[blocker_index].bounds = context.blockers[blocker_index].aabb;
	}
	const BakedVisibilityBackendCapabilities backend_capabilities = BakedVisibilityBackend::probe(p_input.acceleration_backend);
	const BakedVisibilityBackendKind selected_backend = backend_capabilities.kind == BakedVisibilityBackendKind::AUTO ? BakedVisibilityBackendKind::CPU_REFERENCE : backend_capabilities.kind;
	BakedVisibilityBackendBatchOutput backend_batch;
	String backend_error;
	const Error backend_result = BakedVisibilityBackend::execute(selected_backend, backend_input, backend_batch, &backend_error);
	if (backend_result != OK) {
		r_output.error = "Baked visibility backend batch failed: " + backend_error;
		return backend_result;
	}
	r_output.acceleration.requested = p_input.acceleration_backend;
	r_output.acceleration.selected = selected_backend;
	r_output.acceleration.candidate_count = backend_input.candidates.size();
	r_output.acceleration.blocker_count = backend_input.blockers.size();
	for (uint8_t discovered : backend_batch.candidate_mask) r_output.acceleration.discovery_hints += discovered ? 1 : 0;
	for (uint8_t hit : backend_batch.hardware_blocker_hit_hints) r_output.acceleration.hardware_blocker_hints += hit ? 1 : 0;
	r_output.acceleration.dispatch_count = backend_batch.dispatch_count;
	r_output.acceleration.ray_query_count = backend_batch.ray_query_count;
	r_output.acceleration.gpu_executed = backend_batch.gpu_executed;
	r_output.acceleration.hardware_ray_queries_executed = backend_batch.hardware_ray_queries_executed;
	// The accelerator's canonical compaction changes the order in which the CPU
	// walks candidate leaves. Every candidate missing from that broad result is
	// appended and still certified by CPU, so an accelerated miss cannot remove
	// potentially visible work. Rebuild only the candidate BVH; blocker BVH
	// stays in its spatial order for the conservative certificate.
	if (!memory.reserve(context.geometry.size(), sizeof(int), "accelerator ordered candidates") || !memory.reserve(context.geometry.size(), sizeof(uint8_t), "accelerator candidate membership") || !memory.reserve(context.geometry.size(), sizeof(BvhNode) * 2u, "accelerator ordered candidate BVH")) {
		r_output.error = memory.failure;
		return ERR_OUT_OF_MEMORY;
	}
	Vector<int> accelerated_candidate_order;
	Vector<uint8_t> candidate_added;
	candidate_added.resize(context.geometry.size());
	for (uint32_t source_index : backend_batch.compacted_candidate_indices) {
		if (source_index < uint32_t(context.geometry.size()) && source_index < uint32_t(backend_batch.candidate_mask.size()) && backend_batch.candidate_mask[source_index] && !candidate_added[source_index]) {
			accelerated_candidate_order.push_back(source_index);
			candidate_added.write[source_index] = 1;
		}
	}
	const uint32_t accelerated_ordered_count = accelerated_candidate_order.size();
	for (int source_index : context.candidate_indices) {
		if (!candidate_added[source_index]) {
			accelerated_candidate_order.push_back(source_index);
		}
	}
	if (accelerated_candidate_order.size() == context.candidate_indices.size()) {
		context.candidate_indices = accelerated_candidate_order;
		context.candidate_bvh.clear();
		_build_spatial_bvh(context.candidate_bvh, context.candidate_indices, context.geometry, 0, context.geometry.size());
	}
	r_output.acceleration.cpu_candidate_ordered = accelerated_ordered_count;
	r_output.acceleration.cpu_candidate_pruned = 0; // Never prune on a discovery hint.
	r_output.acceleration.candidate_pairs_processed = uint64_t(accelerated_ordered_count) * uint64_t(total_cells);
	r_output.acceleration.diagnostic = backend_batch.diagnostic + "; CPU certificate ordered " + String::num_uint64(accelerated_ordered_count) + " broad candidates and conservatively retained " + String::num_uint64(context.geometry.size() - accelerated_ordered_count) + " unaccelerated candidates";
	// Each cell is certified exactly once by the tile worker. The worker carries
	// the resulting cell into the tile dependency union, so checkpoint admission
	// never performs a second certificate traversal.
	Vector<TileDependencySet> tile_dependencies;
	tile_dependencies.resize(r_output.tile_count);
	CellBuildTask cell_task;
	cell_task.base = &context;
	cell_task.input = &p_input;
	cell_task.bounds = bounds;
	cell_task.cell_size = cell_size;
	cell_task.nx = nx;
	cell_task.ny = ny;
	cell_task.cells = &staged_cells;
	Vector<BakedVisibilityBakeOutput::PreprocessStats> cell_stats;
	cell_stats.resize(total_cells);
	cell_task.cell_stats = &cell_stats;
	cell_task.tiles = &r_output.tiles;
	cell_task.tile_cell_indices = &r_output.tile_cell_indices;
	cell_task.tile_dependencies = &tile_dependencies;
	cell_task.cancel_flag = p_input.cancel_flag;
	cell_task.backend_batch = &backend_batch;
	PackedByteArray current_source_digest = p_input.source_path.is_empty() ? PackedByteArray() : make_source_digest(p_input.source_path);
	r_output.completed_tiles.resize(r_output.tile_count);
	r_output.completed_cell_bitmap.resize(total_cells);
	std::unique_ptr<std::atomic<uint8_t>[]> completed_tiles_atomic = std::make_unique<std::atomic<uint8_t>[]>(r_output.tile_count);
	for (int tile_index = 0; tile_index < r_output.tile_count; tile_index++) completed_tiles_atomic[tile_index].store(0, std::memory_order_relaxed);
	cell_task.completed_tiles = completed_tiles_atomic.get();
	cell_task.completed_cell_bitmap = &r_output.completed_cell_bitmap;
	std::atomic<uint32_t> completed_tile_count = 0;
	cell_task.completed_tiles_count = &completed_tile_count;
	const PackedByteArray current_settings_digest = make_settings_digest(p_input, bounds, cell_size, Vector3i(nx, ny, nz));
	Vector<int> geometry_remap;
	Vector<int> light_remap;
	if (p_input.resume_checkpoint) {
		geometry_remap.resize(p_input.resume_checkpoint->geometry_paths.size());
		for (int old_index = 0; old_index < geometry_remap.size(); old_index++) {
			geometry_remap.write[old_index] = -1;
			for (int new_index = 0; new_index < r_output.geometry_paths.size(); new_index++) {
				if (p_input.resume_checkpoint->geometry_paths[old_index] == r_output.geometry_paths[new_index]) {
					geometry_remap.write[old_index] = new_index;
					break;
				}
			}
		}
		light_remap.resize(p_input.resume_checkpoint->light_paths.size());
		for (int old_index = 0; old_index < light_remap.size(); old_index++) {
			light_remap.write[old_index] = -1;
			for (int new_index = 0; new_index < r_output.light_paths.size(); new_index++) {
				if (p_input.resume_checkpoint->light_paths[old_index] == r_output.light_paths[new_index]) {
					light_remap.write[old_index] = new_index;
					break;
				}
			}
		}
	}
	BakeProgressReporter cell_progress("visibility cells", total_cells);
	cell_task.progress = &cell_progress;
#ifdef DEBUG_ENABLED
	if (p_input.test_serial_reference) {
		for (uint32_t tile_index = 0; tile_index < uint32_t(r_output.tile_count); tile_index++) _build_tile(&cell_task, tile_index);
	} else {
#endif
		if (WorkerThreadPool::get_singleton()) {
			const int worker_tasks = MIN(r_output.tile_count, MAX(1, WorkerThreadPool::get_singleton()->get_thread_count()));
			WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_native_group_task(&_build_tile, &cell_task, r_output.tile_count, worker_tasks, false, "BakedVisibilityTiles");
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		} else {
			for (uint32_t tile_index = 0; tile_index < uint32_t(r_output.tile_count); tile_index++) _build_tile(&cell_task, tile_index);
		}
#ifdef DEBUG_ENABLED
	}
#endif
	// Dependencies and signatures are assembled from the already-published cell
	// certificates. This is the single deterministic merge point; no second
	// certificate traversal is needed for checkpoint admission.
	for (int tile_index = 0; tile_index < r_output.tile_count; tile_index++) {
		BakedVisibilityData3DData::Tile &tile = r_output.tiles.write[tile_index];
		tile.candidate_dependencies = tile_dependencies[tile_index].candidates;
		tile.light_dependencies = tile_dependencies[tile_index].lights;
		tile.certificate_dependencies = tile_dependencies[tile_index].certificates;
		tile.dependency_signature = _tile_dependency_signature(context, Vector3i(nx, ny, nz), tile, tile_dependencies[tile_index]);
	}
	if (p_input.resume_checkpoint && p_input.resume_checkpoint->format_version == BakedVisibilityData3DData::FORMAT_VERSION && p_input.resume_checkpoint->grid_size == Vector3i(nx, ny, nz) && p_input.resume_checkpoint->tile_grid_size == r_output.tile_grid_size && p_input.resume_checkpoint->hierarchy_depth == r_output.hierarchy_depth && p_input.resume_checkpoint->settings_sha256 == current_settings_digest && p_input.resume_checkpoint->tiles.size() == r_output.tiles.size() && p_input.resume_checkpoint->tile_cell_indices.size() == r_output.tile_cell_indices.size() && p_input.resume_checkpoint->completed_tiles.size() == r_output.tile_count) {
		for (int tile_index = 0; tile_index < r_output.tile_count; tile_index++) {
			const BakedVisibilityData3DData::Tile &old_tile = p_input.resume_checkpoint->tiles[tile_index];
			const BakedVisibilityData3DData::Tile &new_tile = r_output.tiles[tile_index];
			if (!p_input.resume_checkpoint->completed_tiles[tile_index] || old_tile.dependency_signature != new_tile.dependency_signature || old_tile.cell_count != new_tile.cell_count) continue;
			bool valid_range = old_tile.first_cell + old_tile.cell_count <= uint32_t(p_input.resume_checkpoint->tile_cell_indices.size());
			for (uint32_t offset = 0; valid_range && offset < old_tile.cell_count; offset++) valid_range = p_input.resume_checkpoint->tile_cell_indices[old_tile.first_cell + offset] == r_output.tile_cell_indices[new_tile.first_cell + offset];
			if (!valid_range) continue;
			Vector<BakedVisibilityBakeCell> remapped_cells;
			remapped_cells.resize(new_tile.cell_count);
			for (uint32_t offset = 0; valid_range && offset < new_tile.cell_count; offset++) {
				const uint32_t cell_index = r_output.tile_cell_indices[new_tile.first_cell + offset];
				if (cell_index >= uint32_t(p_input.resume_checkpoint->cells.size()) || !_remap_checkpoint_cell(p_input.resume_checkpoint->cells[cell_index], geometry_remap, light_remap, remapped_cells.write[offset])) { valid_range = false; break; }
			}
			if (valid_range) {
				for (uint32_t offset = 0; offset < new_tile.cell_count; offset++) {
					const uint32_t cell_index = r_output.tile_cell_indices[new_tile.first_cell + offset];
					staged_cells.write[cell_index] = remapped_cells[offset];
				}
				r_output.reused_cells += new_tile.cell_count;
			}
		}
	}
	cell_progress.finish(cell_progress.completed.load(std::memory_order_relaxed));
	for (int tile_index = 0; tile_index < r_output.tile_count; tile_index++) r_output.completed_tiles.write[tile_index] = completed_tiles_atomic[tile_index].load(std::memory_order_acquire);
	for (int tile_index = 0; tile_index < r_output.tile_count; tile_index++) {
		BakedVisibilityData3DData::Tile &tile = r_output.tiles.write[tile_index];
		tile.candidate_dependencies = tile_dependencies[tile_index].candidates;
		tile.light_dependencies = tile_dependencies[tile_index].lights;
		tile.certificate_dependencies = tile_dependencies[tile_index].certificates;
	}
	r_output.completed_tile_count = 0;
	for (uint8_t complete : r_output.completed_tiles) r_output.completed_tile_count += complete ? 1 : 0;
	for (int tile_index = 0; tile_index < r_output.tile_count; tile_index++) if (r_output.completed_tiles[tile_index]) r_output.completed_cells += r_output.tiles[tile_index].cell_count;
	r_output.cancelled = p_input.cancel_flag && p_input.cancel_flag->load(std::memory_order_relaxed);
	if (r_output.cancelled || r_output.completed_cells != uint32_t(total_cells)) {
		r_output.checkpoint.format_version = BakedVisibilityData3DData::FORMAT_VERSION;
		r_output.checkpoint.grid_size = Vector3i(nx, ny, nz);
		r_output.checkpoint.tile_grid_size = r_output.tile_grid_size;
		r_output.checkpoint.hierarchy_depth = r_output.hierarchy_depth;
		 r_output.checkpoint.completed_cells = r_output.completed_cells;
		r_output.checkpoint.completed_cell_bitmap = r_output.completed_cell_bitmap;
		r_output.checkpoint.cells = staged_cells;
		r_output.checkpoint.geometry_paths = r_output.geometry_paths;
		r_output.checkpoint.light_paths = r_output.light_paths;
		r_output.checkpoint.source_sha256 = current_source_digest;
		r_output.checkpoint.settings_sha256 = current_settings_digest;
		r_output.checkpoint.tiles = r_output.tiles;
		r_output.checkpoint.tile_cell_indices = r_output.tile_cell_indices;
		r_output.checkpoint.completed_tiles = r_output.completed_tiles;
		r_output.checkpoint.completed_tile_count = r_output.completed_tile_count;
		r_output.error = "Baked visibility bake cancelled; completed tiles are resumable.";
		return ERR_BUSY;
	}
	for (const BakedVisibilityBakeCell &staged : staged_cells) {
		r_output.conservative_fallback |= (staged.flags & BakedVisibilityBakeCell::FLAG_WORK_CAP_FALLBACK) != 0;
		BakedVisibilityBakeCell cell = staged;
		cell.primary_set = _intern_set(r_output.interned_primary_sets, cell.primary);
		cell.transport_set = _intern_set(r_output.interned_transport_sets, cell.transport);
		cell.certificate_blockers.clear(); // Leaf union is retained on tile metadata.
		cell.primary = BakedVisibilityBakeSet();
		cell.transport = BakedVisibilityBakeSet();
		r_output.cells.push_back(cell);
	}
	r_output.checkpoint.format_version = BakedVisibilityData3DData::FORMAT_VERSION;
	r_output.checkpoint.grid_size = Vector3i(nx, ny, nz);
	r_output.checkpoint.tile_grid_size = r_output.tile_grid_size;
	r_output.checkpoint.hierarchy_depth = r_output.hierarchy_depth;
	r_output.checkpoint.tiles = r_output.tiles;
	r_output.checkpoint.tile_cell_indices = r_output.tile_cell_indices;
	r_output.checkpoint.completed_tiles = r_output.completed_tiles;
	r_output.checkpoint.completed_tile_count = r_output.completed_tile_count;
	r_output.checkpoint.completed_cells = r_output.completed_cells;
	r_output.checkpoint.completed_cell_bitmap = r_output.completed_cell_bitmap;
	r_output.checkpoint.cells = staged_cells;
	r_output.checkpoint.geometry_paths = r_output.geometry_paths;
	r_output.checkpoint.light_paths = r_output.light_paths;
	r_output.checkpoint.source_sha256 = current_source_digest;
	r_output.checkpoint.settings_sha256 = current_settings_digest;
	r_output.preprocess = context.preprocess;
	for (const BakedVisibilityBakeOutput::PreprocessStats &stats : cell_stats) {
		r_output.preprocess.blocker_nominations += stats.blocker_nominations;
		r_output.preprocess.merge_vertex_visits += stats.merge_vertex_visits;
		r_output.preprocess.backend_candidate_hints += stats.backend_candidate_hints;
		r_output.preprocess.backend_hardware_blocker_hints += stats.backend_hardware_blocker_hints;
	}
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

PackedByteArray BakedVisibilityBaker::make_settings_digest(const BakedVisibilityBakeInput &p_input, const AABB &p_bounds, float p_cell_size, const Vector3i &p_grid_size) {
	const String settings = vformat("bvis-settings|%d|%d|%d|%d|%d|%d|%d|%d", p_grid_size.x, p_grid_size.y, p_grid_size.z, p_input.bake_mask, p_input.max_cells, p_input.certificate_work_cap, p_input.max_blocker_triangles, p_input.strict ? 1 : 0) +
			"|" + String::num_int64(int64_t(p_input.max_memory_bytes)) +
			"|" + String::num_real(p_bounds.position.x) + "|" + String::num_real(p_bounds.position.y) + "|" + String::num_real(p_bounds.position.z) +
			"|" + String::num_real(p_bounds.size.x) + "|" + String::num_real(p_bounds.size.y) + "|" + String::num_real(p_bounds.size.z) +
			"|" + String::num_real(p_cell_size) + "|" + String::num_real(p_input.transport_distance) + "|" + String::num_real(p_input.lookup_margin) + "|" + String::num_real(p_input.requested_cell_size);
	return _sha256(settings);
}

Error BakedVisibilityBaker::build_data(const BakedVisibilityBakeInput &p_input, const BakedVisibilityBakeOutput &p_output, BakedVisibilityData3DData &r_data, String *r_error) const {
	r_data = BakedVisibilityData3DData();
	const bool has_light_metadata = p_output.light_directions.is_empty() && p_output.light_ranges.is_empty();
	if (p_output.cells.is_empty() || p_output.geometry_paths.size() != p_output.geometry_bounds.size() || p_output.geometry_paths.size() != p_output.geometry_identities.size() || p_output.geometry_paths.size() != p_output.geometry_certified_blockers.size() || p_output.light_paths.size() != p_output.light_bounds.size() || p_output.light_paths.size() != p_output.light_directional.size() || (!has_light_metadata && (p_output.light_paths.size() != p_output.light_directions.size() || p_output.light_paths.size() != p_output.light_ranges.size()))) {
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
	r_data.tile_grid_size = p_output.tile_grid_size;
	r_data.tile_size = Vector3i(BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE, BakedVisibilityData3DData::TILE_SIZE);
	r_data.hierarchy_depth = p_output.hierarchy_depth;
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
		const Vector3 light_direction = has_light_metadata ? Vector3() : p_output.light_directions[i];
		const float light_range = has_light_metadata ? 0.0f : p_output.light_ranges[i];
		const String light_identity = String::hex_encode_buffer(r_data.source_sha256.ptr(), r_data.source_sha256.size()) + ":" + String::num_real(light_direction.x) + ":" + String::num_real(light_direction.y) + ":" + String::num_real(light_direction.z) + ":" + String::num_real(light_range);
		instance.signature_sha256 = make_instance_signature(instance.path, instance.kind, instance.local_bounds, instance.flags, light_identity);
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
	r_data.tiles = p_output.tiles;
	r_data.tile_cell_indices = p_output.tile_cell_indices;
	if (r_data.tiles.is_empty()) {
		if (BakedVisibilityCodec::build_tile_hierarchy(r_data.grid_size, r_data.tile_grid_size, r_data.hierarchy_depth, r_data.tiles, r_data.tile_cell_indices, r_error) != OK) {
			return ERR_INVALID_DATA;
		}
	}
	r_data.report = vformat("static_geometry=%d blockers=%d rejected_blockers=%d selected_surfaces=%d selected_triangles=%d rejected_surfaces=%d rejected_triangles=%d arrays_materialized=%d cells=%d degraded=%s", p_output.static_geometry_count, p_output.eligible_blocker_count, p_output.rejected_blocker_count, p_output.preprocess.blocker_surfaces_selected, p_output.preprocess.blocker_triangles_selected, p_output.preprocess.blocker_surfaces_rejected, p_output.preprocess.blocker_triangles_rejected, p_output.preprocess.blocker_arrays_materialized, p_output.cells.size(), p_output.conservative_fallback ? "true" : "false");
	// Encoding validates canonical ordering without forcing the runtime decoder
	// to materialize the complete city grid.
	PackedByteArray canonical_bytes;
	String canonical_error;
	if (BakedVisibilityCodec::encode(r_data, canonical_bytes, &canonical_error) != OK) {
		if (r_error) {
			*r_error = canonical_error;
		}
		return ERR_INVALID_DATA;
	}
	BakedVisibilityData3DData canonical_data;
	Vector<BakedVisibilityData3DData::Tile> canonical_hierarchy;
	Vector<uint32_t> canonical_indices;
	if (BakedVisibilityCodec::decode(canonical_bytes, canonical_data, &canonical_error) != OK || BakedVisibilityCodec::build_tile_hierarchy(canonical_data.grid_size, canonical_data.tile_grid_size, canonical_data.hierarchy_depth, canonical_hierarchy, canonical_indices, &canonical_error) != OK) {
		if (r_error) {
			*r_error = canonical_error;
		}
		return ERR_INVALID_DATA;
	}
	canonical_data.tile_cell_indices = canonical_indices;
	canonical_data.cells.resize(r_data.cells.size());
	canonical_data.sets.push_back(Vector<uint32_t>());
	for (uint32_t tile_index = 0; tile_index < uint32_t(canonical_data.tiles.size()); tile_index++) {
		if (!(canonical_data.tiles[tile_index].flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) {
			continue;
		}
		Vector<uint32_t> cell_indices;
		Vector<BakedVisibilityData3DData::Cell> cells;
		Vector<Vector<uint32_t>> local_sets;
		if (BakedVisibilityCodec::decode_leaf_payload(canonical_data, tile_index, cell_indices, cells, local_sets, &canonical_error) != OK || cell_indices.size() != cells.size()) {
			if (r_error) {
				*r_error = canonical_error;
			}
			return ERR_INVALID_DATA;
		}
		Vector<uint32_t> local_to_global;
		local_to_global.resize(local_sets.size());
		for (int set = 0; set < local_sets.size(); set++) {
			local_to_global.write[set] = _intern_combined_set(canonical_data.sets, local_sets[set]);
		}
		for (int cell = 0; cell < cells.size(); cell++) {
			BakedVisibilityData3DData::Cell decoded_cell = cells[cell];
			decoded_cell.primary_set = local_to_global[decoded_cell.primary_set];
			decoded_cell.transport_set = local_to_global[decoded_cell.transport_set];
			canonical_data.cells.write[cell_indices[cell]] = decoded_cell;
		}
	}
	r_data = canonical_data;
	return OK;
}
