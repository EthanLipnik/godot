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
#include "servers/rendering/baked_visibility/baked_visibility_codec.h"

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
};

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
};

struct BakedVisibilityBakeOutput {
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
		uint64_t extraction_usec = 0;
		uint64_t merge_usec = 0;
		uint64_t bvh_usec = 0;
		uint32_t progress = PROGRESS_NONE;
	};

	Vector<String> geometry_paths;
	Vector<String> geometry_identities;
	Vector<AABB> geometry_bounds;
	Vector<bool> geometry_certified_blockers;
	Vector<String> light_paths;
	Vector<AABB> light_bounds;
	Vector<bool> light_directional;
	Vector<BakedVisibilityBakeSet> interned_primary_sets;
	Vector<BakedVisibilityBakeSet> interned_transport_sets;
	Vector<BakedVisibilityBakeCell> cells;
	AABB bounds;
	float cell_size = 0.0f;
	Vector3i grid_size;
	int static_geometry_count = 0;
	int eligible_blocker_count = 0;
	int rejected_blocker_count = 0;
	bool conservative_fallback = false;
	String error;
	PreprocessStats preprocess;
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
	static String make_geometry_identity(const MeshInstance3D *p_mesh);
	static PackedByteArray make_instance_signature(const NodePath &p_path, uint8_t p_kind, const AABB &p_local_bounds, uint32_t p_flags, const String &p_identity);
	Error bake(const BakedVisibilityBakeInput &p_input, BakedVisibilityBakeOutput &r_output) const;
	Error build_data(const BakedVisibilityBakeInput &p_input, const BakedVisibilityBakeOutput &p_output, BakedVisibilityData3DData &r_data, String *r_error = nullptr) const;
};
