/**************************************************************************/
/*  streamed_cluster_runtime.h                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"

namespace RendererPathTracing {

enum class StreamedClusterPageState : uint8_t {
	UNLOADED,
	REQUESTED,
	LOADING,
	RESIDENT,
	ACTIVE,
	RETIRING,
};

struct StreamedClusterPageDescriptor {
	uint64_t stable_id = 0;
	uint64_t revision = 0;
	uint64_t parent_id = 0;
	Vector<uint64_t> child_ids;
	AABB bounds;
	float geometric_error = 0.0f;
	uint64_t blob_bytes = 0;
	uint32_t vertex_bytes = 0;
	uint32_t index_bytes = 0;
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;
	uint32_t material_index = 0;
	uint32_t lod_level = 0;
	bool persistent = false;
};

struct StreamedClusterSelection {
	Vector<AABB> eye_regions;
	float maximum_geometric_error = 0.0f;
	uint64_t frame = 0;
};

struct StreamedClusterBudgets {
	uint64_t maximum_resident_bytes = 512ull * 1024ull * 1024ull;
	uint64_t maximum_load_bytes_per_frame = 32ull * 1024ull * 1024ull;
	uint32_t maximum_load_tasks_per_frame = 16;
	uint32_t maximum_retire_tasks_per_frame = 16;
	uint32_t minimum_resident_frames = 3;
};

struct StreamedClusterLoadTask {
	uint64_t task_token = 0;
	uint64_t stable_id = 0;
	uint64_t revision = 0;
	uint64_t blob_bytes = 0;
	uint32_t vertex_bytes = 0;
	uint32_t index_bytes = 0;
};

struct StreamedClusterRetireTask {
	uint64_t stable_id = 0;
	RID vertex_buffer;
	RID index_buffer;
};

struct StreamedClusterSurface {
	uint64_t stable_id = 0;
	uint64_t topology_revision = 0;
	RID vertex_buffer;
	RID index_buffer;
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;
	uint32_t vertex_stride = 12;
	uint32_t index_stride = 4;
	uint32_t material_index = 0;
	AABB bounds;
};

struct StreamedClusterFrameStats {
	uint32_t requested = 0;
	uint32_t loading = 0;
	uint32_t resident = 0;
	uint32_t active = 0;
	uint32_t retiring = 0;
	uint32_t load_tasks = 0;
	uint32_t retire_tasks = 0;
	uint32_t loaded = 0;
	uint32_t unloaded = 0;
	uint32_t failed_loads = 0;
	uint32_t budget_misses = 0;
	uint32_t coarse_fallbacks = 0;
	uint64_t resident_bytes = 0;
	uint64_t reserved_bytes = 0;
	uint64_t transfer_bytes = 0;
	uint64_t selection_time_usec = 0;
};

class StreamedClusterResidencyManager {
	struct Entry {
		StreamedClusterPageDescriptor descriptor;
		StreamedClusterPageState state = StreamedClusterPageState::UNLOADED;
		bool desired = false;
		bool retirement_issued = false;
		uint64_t load_task_token = 0;
		uint64_t upload_completion_token = 0;
		uint64_t retirement_completion_token = 0;
		uint64_t activated_frame = 0;
		RID vertex_buffer;
		RID index_buffer;
	};

	HashMap<uint64_t, Entry> entries;
	Vector<uint64_t> stable_order;
	StreamedClusterBudgets budgets;
	StreamedClusterFrameStats frame_stats;
	uint64_t current_frame = 0;
	uint64_t next_task_token = 1;
	uint64_t resident_bytes = 0;
	uint64_t reserved_bytes = 0;

	bool _intersects_any_eye(const AABB &p_bounds, const Vector<AABB> &p_eye_regions) const;
	bool _group_wants_refinement(const Entry &p_root, const StreamedClusterSelection &p_selection) const;
	void _refresh_counts();
	void _append_surface(const Entry &p_entry, Vector<StreamedClusterSurface> &r_surfaces) const;

public:
	Error set_manifest(const Vector<StreamedClusterPageDescriptor> &p_pages);
	void clear();
	void set_budgets(const StreamedClusterBudgets &p_budgets) { budgets = p_budgets; }
	const StreamedClusterBudgets &get_budgets() const { return budgets; }

	void update_selection(const StreamedClusterSelection &p_selection);
	Vector<StreamedClusterLoadTask> take_load_tasks();
	Error complete_load(uint64_t p_task_token, uint64_t p_upload_completion_token, RID p_vertex_buffer, RID p_index_buffer, bool p_success);
	Vector<StreamedClusterRetireTask> take_retire_tasks();
	Error submit_retirement(uint64_t p_stable_id, uint64_t p_retirement_completion_token);
	void update_completion_tokens(uint64_t p_completed_upload_token, uint64_t p_completed_retirement_token);

	StreamedClusterPageState get_page_state(uint64_t p_stable_id) const;
	Vector<StreamedClusterSurface> get_active_surfaces();
	const StreamedClusterFrameStats &get_frame_stats() const { return frame_stats; }
};

} // namespace RendererPathTracing
