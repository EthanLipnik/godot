/**************************************************************************/
/*  baked_visibility_baker.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/string/node_path.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"
#include "servers/rendering/baked_visibility/backend/baked_visibility_backend.h"
#include "servers/rendering/baked_visibility/baked_visibility_codec.h"

#include <atomic>

class Node;
class Node3D;
class MeshInstance3D;

// The baker deliberately uses instance paths rather than RIDs. A baked asset must
// remain valid across editor and game sessions, while RIDs are session-local.
struct BakedVisibilityBakeSet {
	PackedInt32Array geometry;
	PackedInt32Array lights;
};

struct BakedVisibilityBakeCell {
	enum Flags : uint32_t {
		FLAG_NONE = 0,
		FLAG_WORK_CAP_FALLBACK = 1 << 0,
	};

	Vector3i coordinate;
	BakedVisibilityBakeSet primary;
	BakedVisibilityBakeSet transport;
	uint32_t flags = FLAG_NONE;
	uint32_t primary_set = 0;
	uint32_t transport_set = 0;
	// Blocker indices selected by actual per-cell CPU exclusion certificates.
	// They are reduced to stable leaf dependencies after worker completion.
	Vector<int> certificate_blockers;
};

struct BakedVisibilityBakeCheckpoint;

struct BakedVisibilityBakeInput {
	Node3D *anchor = nullptr;
	Node *scene_root = nullptr;
	String source_path;
	AABB bounds;
	float requested_cell_size = 4.0f;
	int max_cells = 4096;
	int certificate_work_cap = 8192;
	int max_blocker_triangles = 131072;
	float transport_distance = 64.0f;
	uint32_t bake_mask = 0x000fffff; // Camera3D's default 20-layer cull mask.
	float lookup_margin = 0.1f;
	bool strict = false;
	// Optional controls make the bake independently resumable without exposing
	// scene objects to worker threads. A cancelled bake returns ERR_BUSY and
	// checkpoints only complete tiles.
	std::atomic_bool *cancel_flag = nullptr;
	const BakedVisibilityBakeCheckpoint *resume_checkpoint = nullptr;
	uint64_t max_memory_bytes = 512ull * 1024ull * 1024ull;
	// AUTO uses a live product-neutral backend only for broad candidate and
	// blocker hints. CPU certificates are always still performed per cell.
	BakedVisibilityBackendKind acceleration_backend = BakedVisibilityBackendKind::AUTO;
	// Full CPU/GPU parity is intentionally opt-in. Normal editor bakes execute
	// the selected adapter once and retain deterministic CPU fallback semantics.
	bool deterministic_validation = false;
#ifdef DEBUG_ENABLED
	// Test-only oracle switch; production always uses the tile worker pool.
	bool test_serial_reference = false;
#endif
};

struct BakedVisibilityBakeCheckpoint {
	static constexpr uint32_t CHECKPOINT_SCHEMA_VERSION = 2;
	uint32_t schema_version = CHECKPOINT_SCHEMA_VERSION;
	uint32_t format_version = BakedVisibilityData3DData::FORMAT_VERSION;
	Vector3i grid_size;
	Vector3i tile_grid_size;
	uint32_t hierarchy_depth = 1;
	Vector<BakedVisibilityData3DData::Tile> tiles;
	Vector<uint32_t> tile_cell_indices;
	Vector<BakedVisibilityBakeCell> cells;
	// Canonical paths are the stable checkpoint membership keys. Cells only keep
	// compact indices while baking; reuse remaps those indices through these keys
	// before accepting a completed leaf from a changed scene snapshot.
	Vector<String> geometry_paths;
	Vector<String> light_paths;
	Vector<uint8_t> completed_tiles;
	Vector<uint8_t> completed_cell_bitmap;
	PackedByteArray source_sha256;
	// Canonical full-scene fingerprint. Older checkpoints without this contract
	// are never admitted before certification.
	PackedByteArray scene_fingerprint;
	// Settings that affect every tile. Scene dependencies remain on the tile
	// signatures, which permits safe reuse only when their own certificate still
	// matches the immutable bake snapshot.
	PackedByteArray settings_sha256;
	uint32_t completed_cells = 0;
	uint32_t completed_tile_count = 0;
};

struct BakedVisibilityBakeOutput {
	struct AccelerationStats {
		BakedVisibilityBackendKind requested = BakedVisibilityBackendKind::AUTO;
		BakedVisibilityBackendKind selected = BakedVisibilityBackendKind::CPU_REFERENCE;
		uint32_t candidate_count = 0;
		uint32_t blocker_count = 0;
		uint32_t discovery_hints = 0;
		uint32_t hardware_blocker_hints = 0;
		uint32_t dispatch_count = 0;
		uint32_t ray_query_count = 0;
		uint64_t candidate_pairs_processed = 0;
		uint64_t cpu_candidate_ordered = 0;
		uint64_t cpu_candidate_pruned = 0;
		uint64_t gpu_certified_exclusions = 0;
		uint64_t cpu_fallbacks = 0;
		uint64_t checkpoint_tiles_reused = 0;
		uint64_t checkpoint_cells_reused = 0;
		uint64_t hierarchical_subtree_exclusions = 0;
		uint64_t cached_patch_hits = 0;
		uint64_t cached_patch_misses = 0;
		uint64_t cached_patch_invalidations = 0;
		uint64_t validation_candidate_pairs = 0;
		uint64_t certificate_packet_count = 0;
		uint64_t certificate_dispatch_count = 0;
		uint64_t certificate_validation_mismatches = 0;
		uint64_t certificate_usec = 0;
		bool gpu_executed = false;
		bool hardware_ray_queries_executed = false;
		bool validation_executed = false;
		bool validation_equivalent = true;
		String diagnostic;
	};
	// Offline observability only. These values are deliberately not copied into
	// BakedVisibilityData3DData, so timing and progress never affect canonical
	// baked bytes.
	struct PreprocessStats {
		enum Progress : uint32_t {
			PROGRESS_NONE = 0,
			PROGRESS_TRIANGLES_EXTRACTED = 1 << 0,
			PROGRESS_PATCHES_MERGED = 1 << 1,
			PROGRESS_BVHS_BUILT = 1 << 2,
		};

		uint64_t triangle_count = 0;
		uint64_t blocker_surface_candidates = 0;
		uint64_t blocker_surfaces_selected = 0;
		uint64_t blocker_surfaces_rejected = 0;
		uint64_t blocker_triangles_selected = 0;
		uint64_t blocker_triangles_rejected = 0;
		uint64_t blocker_arrays_materialized = 0;
		uint64_t shared_edge_candidates = 0;
		uint64_t ambiguous_edges = 0;
		uint64_t merge_attempts = 0;
		uint64_t merged_patch_count = 0;
		uint64_t merge_vertex_visits = 0;
		uint64_t blocker_nominations = 0;
		uint64_t bvh_order_entries = 0;
		uint64_t backend_candidate_hints = 0;
		uint64_t backend_hardware_blocker_hints = 0;
		uint64_t hierarchical_subtree_exclusions = 0;
		uint64_t gpu_certified_exclusions = 0;
		uint64_t cpu_certificate_fallbacks = 0;
		uint64_t certificate_packet_count = 0;
		uint64_t certificate_dispatch_count = 0;
		uint64_t certificate_validation_mismatches = 0;
		uint64_t cached_patch_hits = 0;
		uint64_t cached_patch_misses = 0;
		uint64_t cached_patch_invalidations = 0;
		uint64_t extraction_usec = 0;
		uint64_t merge_usec = 0;
		uint64_t bvh_usec = 0;
		uint64_t candidate_discovery_usec = 0;
		uint64_t certification_usec = 0;
		uint64_t total_usec = 0;
		uint32_t progress = PROGRESS_NONE;
	};

	Vector<String> geometry_paths;
	Vector<String> geometry_identities;
	Vector<AABB> geometry_bounds;
	Vector<bool> geometry_certified_blockers;
	Vector<String> light_paths;
	Vector<AABB> light_bounds;
	Vector<Vector3> light_directions;
	Vector<float> light_ranges;
	Vector<bool> light_directional;
	Vector<BakedVisibilityBakeSet> interned_primary_sets;
	Vector<BakedVisibilityBakeSet> interned_transport_sets;
	Vector<BakedVisibilityBakeCell> cells;
	AABB bounds;
	float cell_size = 0.0f;
	Vector3i grid_size;
	Vector3i tile_grid_size;
	uint32_t hierarchy_depth = 1;
	int static_geometry_count = 0;
	int eligible_blocker_count = 0;
	int rejected_blocker_count = 0;
	bool conservative_fallback = false;
	bool cancelled = false;
	uint32_t completed_cells = 0;
	uint32_t completed_tile_count = 0;
	uint32_t reused_cells = 0;
	uint32_t tile_count = 0;
	Vector<BakedVisibilityData3DData::Tile> tiles;
	Vector<uint32_t> tile_cell_indices;
	Vector<uint8_t> completed_tiles;
	Vector<uint8_t> completed_cell_bitmap;
	PackedByteArray scene_fingerprint;
	BakedVisibilityBakeCheckpoint checkpoint;
	String error;
	PreprocessStats preprocess;
	AccelerationStats acceleration;
};

// Offline, deterministic, correctness-first visibility bake. It only removes a
// candidate after a complete corner-to-corner certificate against a single opaque
// convex patch. Every unsupported blocker stays non-occluding; it never removes
// content based on sampled visibility alone.
class BakedVisibilityBaker {
public:
	// p_staged_source_path, when supplied, contributes the staged source text
	// under p_source_path's stable dependency identity. This lets the CLI sign
	// the canonical scene it is about to atomically commit.
	static PackedByteArray make_source_digest(const String &p_source_path, const String &p_staged_source_path = String());
	static PackedByteArray make_settings_digest(const BakedVisibilityBakeInput &p_input, const AABB &p_bounds, float p_cell_size, const Vector3i &p_grid_size);
	static String make_geometry_identity(const MeshInstance3D *p_mesh);
	static PackedByteArray make_instance_signature(const NodePath &p_path, uint8_t p_kind, const AABB &p_local_bounds, uint32_t p_flags, const String &p_identity);
	Error bake(const BakedVisibilityBakeInput &p_input, BakedVisibilityBakeOutput &r_output) const;
	Error build_data(const BakedVisibilityBakeInput &p_input, const BakedVisibilityBakeOutput &p_output, BakedVisibilityData3DData &r_data, String *r_error = nullptr) const;
};
