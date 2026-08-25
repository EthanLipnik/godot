/**************************************************************************/
/*  virtual_geometry_format.h                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/io/compression.h"
#include "core/math/aabb.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

// This is deliberately a portable, CPU-only contract. Backend residency and
// device caches are VG2 work and must not leak into serialized VG1 data.
namespace RendererVirtualGeometry {

static constexpr uint32_t FORMAT_VERSION = 1;
static constexpr uint32_t PAGE_PAYLOAD_VERSION = 1;
static constexpr uint32_t COMPILER_SEMANTIC_GENERATION = 1;
static constexpr uint32_t COMPRESSION_GENERATION = 1;
static constexpr uint32_t MATERIAL_SCHEMA_VERSION = 1;
static constexpr uint32_t RAY_HINT_SCHEMA_VERSION = 1;
static constexpr uint64_t MAX_PORTABLE_PAGE_BYTES = uint64_t(INT64_MAX);

enum StreamFlags : uint32_t {
	STREAM_POSITION = 1 << 0,
	STREAM_NORMAL = 1 << 1,
	STREAM_TANGENT = 1 << 2,
	STREAM_UV0 = 1 << 3,
	STREAM_UV1 = 1 << 4,
	STREAM_COLOR = 1 << 5,
	STREAM_JOINTS = 1 << 6,
	STREAM_WEIGHTS = 1 << 7,
};

enum ClusterFlags : uint32_t {
	CLUSTER_OPAQUE = 1 << 0,
	CLUSTER_DOUBLE_SIDED = 1 << 1,
	CLUSTER_HIGH_PRECISION_POSITIONS = 1 << 2,
};

// Ray grouping is an independent, versioned portable hint. It contains only
// ordinary cluster identities and transport semantics; native acceleration
// structures remain backend-owned caches.
enum class RayTransportTier : uint8_t {
	TIER_FAR,
	TIER_MIDDLE,
	TIER_NEAR,
};

struct RayGroupDescriptor {
	uint64_t stable_id = 0;
	uint64_t transport_region_id = 0;
	uint64_t revision = 0;
	RayTransportTier tier = RayTransportTier::TIER_FAR;
	Vector<uint64_t> cluster_ids;
	AABB bounds;
	bool persistent_coarse = false;
	bool fixed_topology_refit = false;
};

struct StreamSchema {
	uint64_t stable_id = 0;
	uint32_t flags = STREAM_POSITION;
	uint32_t revision = 1;
};

struct MaterialDescriptor {
	uint64_t semantic_id = 0;
	uint32_t closure_class = 0;
	uint32_t revision = 1;
};

struct ClusterDescriptor {
	uint64_t stable_id = 0;
	uint64_t topology_revision = 0;
	uint64_t attribute_revision = 0;
	uint64_t material_semantic_revision = 0;
	uint64_t page_id = 0;
	uint64_t payload_offset = 0;
	uint64_t payload_size = 0;
	uint32_t vertex_count = 0;
	uint32_t triangle_count = 0;
	uint64_t stream_schema_id = 0;
	uint32_t material_slot = 0;
	AABB bounds;
	Vector3 sphere_center;
	real_t sphere_radius = 0.0;
	Vector3 position_decode_scale;
	Vector3 position_decode_bias;
	uint64_t feature_remap_offset = 0;
	uint32_t feature_remap_count = 0;
	uint32_t flags = CLUSTER_OPAQUE | CLUSTER_HIGH_PRECISION_POSITIONS;
};

struct RefinementGroupDescriptor {
	uint64_t stable_id = 0;
	uint64_t revision = 0;
	Vector<uint64_t> coarse_cluster_ids;
	Vector<uint64_t> fine_cluster_ids;
	Vector<uint64_t> parent_group_ids;
	Vector<uint64_t> child_group_ids;
	AABB bounds;
	real_t geometric_error = 0.0;
	real_t attribute_error = 0.0;
	Vector<uint64_t> atomic_dependencies;
	bool persistent_root = false;
};

struct PageDescriptor {
	uint64_t stable_id = 0;
	uint64_t content_hash = 0;
	uint64_t compressed_size = 0;
	uint64_t decoded_size = 0;
	uint64_t file_offset = 0;
	uint64_t file_size = 0;
	uint32_t compression_scheme = uint32_t(Compression::MODE_ZSTD);
	uint32_t compression_generation = COMPRESSION_GENERATION;
	Vector<uint64_t> cluster_ids;
	Vector<uint64_t> required_parent_or_root_pages;
	Vector<uint64_t> material_dependency_hashes;
	uint32_t priority_class = 0;
	bool persistent = false;
};

struct DependencyRecord {
	uint64_t source_identity = 0;
	uint64_t source_digest = 0;
	uint64_t compiler_settings_hash = 0;
	uint64_t dependency_hash = 0;
};

struct Manifest {
	uint32_t format_version = FORMAT_VERSION;
	uint32_t page_payload_version = PAGE_PAYLOAD_VERSION;
	uint32_t compiler_semantic_generation = COMPILER_SEMANTIC_GENERATION;
	uint32_t compression_generation = COMPRESSION_GENERATION;
	uint32_t material_schema_version = MATERIAL_SCHEMA_VERSION;
	uint32_t ray_hint_schema_version = RAY_HINT_SCHEMA_VERSION;
	uint64_t source_asset_identity = 0;
	uint64_t source_primitive_identity = 0;
	uint64_t source_digest = 0;
	AABB resource_bounds;
	Vector<StreamSchema> stream_schemas;
	Vector<MaterialDescriptor> materials;
	Vector<ClusterDescriptor> clusters;
	Vector<RefinementGroupDescriptor> groups;
	Vector<uint64_t> root_group_ids;
	Vector<RayGroupDescriptor> ray_groups;
	Vector<PageDescriptor> pages;
	Vector<DependencyRecord> dependencies;
	Vector<String> conventional_path_diagnostics;
	uint64_t build_settings_hash = 0;
};

struct Package {
	Manifest manifest;
	// In-memory build product. Serialized packages place these buffers at the
	// checked 64-bit PageDescriptor ranges.
	Vector<PackedByteArray> compressed_pages;
};

static inline uint64_t hash_bytes(const uint8_t *p_data, uint64_t p_size, uint64_t p_seed = 1469598103934665603ull) {
	uint64_t hash = p_seed;
	for (uint64_t i = 0; i < p_size; i++) {
		hash ^= p_data[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static inline bool checked_add_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_result) {
	if (p_b > UINT64_MAX - p_a) {
		return false;
	}
	r_result = p_a + p_b;
	return r_result <= MAX_PORTABLE_PAGE_BYTES;
}

static inline bool checked_range_u64(uint64_t p_offset, uint64_t p_size, uint64_t p_limit) {
	uint64_t end = 0;
	return checked_add_u64(p_offset, p_size, end) && end <= p_limit;
}

// This validates only portable contracts; it intentionally has no GPU object
// or backend dependency. p_check_payloads also checks compression round trips.
static inline Error validate_package(const Package &p_package, bool p_check_payloads = true) {
	const Manifest &manifest = p_package.manifest;
	ERR_FAIL_COND_V(manifest.format_version != FORMAT_VERSION || manifest.page_payload_version != PAGE_PAYLOAD_VERSION, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(manifest.compiler_semantic_generation == 0 || manifest.compression_generation == 0 || manifest.material_schema_version == 0 || manifest.ray_hint_schema_version == 0, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(!manifest.resource_bounds.is_finite(), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_check_payloads && p_package.compressed_pages.size() != manifest.pages.size(), ERR_INVALID_DATA);
	HashSet<uint64_t> cluster_ids;
	HashSet<uint64_t> page_ids;
	HashSet<uint64_t> group_ids;
	HashSet<uint64_t> ray_group_ids;
	HashMap<uint64_t, uint64_t> cluster_page;
	for (const ClusterDescriptor &cluster : manifest.clusters) {
		ERR_FAIL_COND_V(cluster.stable_id == 0 || cluster.page_id == 0 || cluster.vertex_count == 0 || cluster.triangle_count == 0, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(cluster_ids.has(cluster.stable_id) || !cluster.bounds.is_finite() || !Math::is_finite(cluster.sphere_radius) || cluster.sphere_radius < 0.0, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(!checked_range_u64(cluster.payload_offset, cluster.payload_size, MAX_PORTABLE_PAGE_BYTES), ERR_INVALID_DATA);
		cluster_ids.insert(cluster.stable_id);
		cluster_page.insert(cluster.stable_id, cluster.page_id);
	}
	for (int page_index = 0; page_index < manifest.pages.size(); page_index++) {
		const PageDescriptor &page = manifest.pages[page_index];
		ERR_FAIL_COND_V(page.stable_id == 0 || page_ids.has(page.stable_id) || page.decoded_size == 0 || page.compressed_size == 0, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(!checked_range_u64(page.file_offset, page.file_size, MAX_PORTABLE_PAGE_BYTES) || page.file_size != page.compressed_size, ERR_INVALID_DATA);
		page_ids.insert(page.stable_id);
		HashSet<uint64_t> page_clusters;
		for (uint64_t cluster_id : page.cluster_ids) {
			ERR_FAIL_COND_V(!cluster_ids.has(cluster_id) || page_clusters.has(cluster_id), ERR_INVALID_DATA);
			ERR_FAIL_COND_V(cluster_page[cluster_id] != page.stable_id, ERR_INVALID_DATA);
			page_clusters.insert(cluster_id);
		}
		if (p_check_payloads) {
			const PackedByteArray &compressed = p_package.compressed_pages[page_index];
			ERR_FAIL_COND_V(uint64_t(compressed.size()) != page.compressed_size || page.decoded_size > uint64_t(INT64_MAX), ERR_INVALID_DATA);
			PackedByteArray decoded;
			ERR_FAIL_COND_V(decoded.resize(int64_t(page.decoded_size)) != OK, ERR_OUT_OF_MEMORY);
			const int64_t decoded_size = Compression::decompress(decoded.ptrw(), decoded.size(), compressed.ptr(), compressed.size(), Compression::Mode(page.compression_scheme));
			ERR_FAIL_COND_V(decoded_size != decoded.size() || hash_bytes(decoded.ptr(), decoded.size()) != page.content_hash, ERR_FILE_CORRUPT);
		}
	}
	for (const ClusterDescriptor &cluster : manifest.clusters) {
		const PageDescriptor *page = nullptr;
		for (const PageDescriptor &candidate : manifest.pages) {
			if (candidate.stable_id == cluster.page_id) { page = &candidate; break; }
		}
		ERR_FAIL_NULL_V(page, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(!checked_range_u64(cluster.payload_offset, cluster.payload_size, page->decoded_size), ERR_INVALID_DATA);
	}
	for (const RefinementGroupDescriptor &group : manifest.groups) {
		ERR_FAIL_COND_V(group.stable_id == 0 || group_ids.has(group.stable_id) || group.coarse_cluster_ids.is_empty() || group.fine_cluster_ids.is_empty(), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(!group.bounds.is_finite() || !Math::is_finite(group.geometric_error) || group.geometric_error < 0.0, ERR_INVALID_DATA);
		group_ids.insert(group.stable_id);
		for (uint64_t id : group.coarse_cluster_ids) { ERR_FAIL_COND_V(!cluster_ids.has(id), ERR_INVALID_DATA); }
		for (uint64_t id : group.fine_cluster_ids) { ERR_FAIL_COND_V(!cluster_ids.has(id), ERR_INVALID_DATA); }
	}
	HashMap<uint64_t, uint32_t> ray_tiers_by_region;
	for (const RayGroupDescriptor &ray_group : manifest.ray_groups) {
		ERR_FAIL_COND_V(ray_group.stable_id == 0 || ray_group.transport_region_id == 0 || ray_group.revision == 0 || ray_group.cluster_ids.is_empty(), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(ray_group_ids.has(ray_group.stable_id) || !ray_group.bounds.is_finite(), ERR_INVALID_DATA);
		const uint32_t tier = uint32_t(ray_group.tier);
		ERR_FAIL_COND_V(tier > uint32_t(RayTransportTier::TIER_NEAR), ERR_INVALID_DATA);
		const uint32_t bit = 1u << tier;
		uint32_t &region_tiers = ray_tiers_by_region[ray_group.transport_region_id];
		ERR_FAIL_COND_V((region_tiers & bit) != 0, ERR_INVALID_DATA);
		region_tiers |= bit;
		ray_group_ids.insert(ray_group.stable_id);
		HashSet<uint64_t> ray_clusters;
		for (uint64_t cluster_id : ray_group.cluster_ids) {
			ERR_FAIL_COND_V(!cluster_ids.has(cluster_id) || ray_clusters.has(cluster_id), ERR_INVALID_DATA);
			ray_clusters.insert(cluster_id);
		}
		ERR_FAIL_COND_V(ray_group.persistent_coarse != (ray_group.tier == RayTransportTier::TIER_FAR), ERR_INVALID_DATA);
	}
	for (const KeyValue<uint64_t, uint32_t> &region : ray_tiers_by_region) {
		ERR_FAIL_COND_V(region.value != 0x7u, ERR_INVALID_DATA);
	}
	HashSet<uint64_t> roots;
	for (uint64_t root : manifest.root_group_ids) { ERR_FAIL_COND_V(!group_ids.has(root) || roots.has(root), ERR_INVALID_DATA); roots.insert(root); }
	ERR_FAIL_COND_V(roots.is_empty(), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(ray_tiers_by_region.size() != roots.size(), ERR_INVALID_DATA);
	for (const KeyValue<uint64_t, uint32_t> &region : ray_tiers_by_region) { ERR_FAIL_COND_V(!roots.has(region.key), ERR_INVALID_DATA); }
	for (const RefinementGroupDescriptor &group : manifest.groups) {
		for (uint64_t parent : group.parent_group_ids) { ERR_FAIL_COND_V(!group_ids.has(parent) || parent == group.stable_id, ERR_INVALID_DATA); }
		for (uint64_t child : group.child_group_ids) { ERR_FAIL_COND_V(!group_ids.has(child) || child == group.stable_id, ERR_INVALID_DATA); }
		ERR_FAIL_COND_V(group.persistent_root != roots.has(group.stable_id), ERR_INVALID_DATA);
	}
	// Parent links must form an acyclic graph and every group must reach a root.
	for (const RefinementGroupDescriptor &start : manifest.groups) {
		HashSet<uint64_t> visited;
		Vector<uint64_t> frontier;
		frontier.push_back(start.stable_id);
		bool reached_root = false;
		while (!frontier.is_empty()) {
			const uint64_t id = frontier[frontier.size() - 1];
			frontier.remove_at(frontier.size() - 1);
			ERR_FAIL_COND_V(visited.has(id), ERR_INVALID_DATA);
			visited.insert(id);
			const RefinementGroupDescriptor *group = nullptr;
			for (const RefinementGroupDescriptor &candidate : manifest.groups) if (candidate.stable_id == id) { group = &candidate; break; }
			ERR_FAIL_NULL_V(group, ERR_INVALID_DATA);
			if (roots.has(id)) { reached_root = true; continue; }
			ERR_FAIL_COND_V(group->parent_group_ids.is_empty(), ERR_INVALID_DATA);
			for (uint64_t parent : group->parent_group_ids) frontier.push_back(parent);
		}
		ERR_FAIL_COND_V(!reached_root, ERR_INVALID_DATA);
	}
	return OK;
}

} // namespace RendererVirtualGeometry
