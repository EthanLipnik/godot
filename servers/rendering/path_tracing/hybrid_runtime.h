/**************************************************************************/
/*  hybrid_runtime.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "path_tracing_scene_packet.h"

#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

namespace RendererPathTracing {

enum HybridEffect : uint32_t {
	HYBRID_EFFECT_RASTER_PRIMARY = 1 << 0,
	HYBRID_EFFECT_RAY_SHADOWS = 1 << 1,
	HYBRID_EFFECT_RAY_REFLECTIONS = 1 << 2,
	HYBRID_EFFECT_RAY_DIFFUSE_INDIRECT = 1 << 3,
	HYBRID_EFFECT_TEMPORAL_RECONSTRUCTION = 1 << 4,
};

enum class ReconstructionKind : uint32_t {
	NONE,
	METALFX,
	DLSS_RAY_RECONSTRUCTION,
};

struct ReconstructionCapabilities {
	StringName name;
	ReconstructionKind kind = ReconstructionKind::NONE;
	bool available = false;
	bool temporal_denoising = false;
	bool super_resolution = false;
	bool stereo = false;
	uint32_t max_views = 0;
	uint32_t required_guides = 0;
	String unavailable_reason;
};

struct ReconstructionRequest {
	uint32_t view_count = 1;
	uint32_t available_guides = 0;
	bool require_temporal_denoising = true;
	bool require_super_resolution = true;
};

struct ReconstructionSelection {
	int adapter_index = -1;
	StringName adapter_name;
	ReconstructionKind kind = ReconstructionKind::NONE;
	String unavailable_reason;

	bool is_available() const { return adapter_index >= 0; }
};

ReconstructionSelection select_reconstruction_adapter(const ReconstructionRequest &p_request, const Vector<ReconstructionCapabilities> &p_candidates);

enum HybridSceneUpdate : uint32_t {
	HYBRID_SCENE_UPDATE_NONE = 0,
	HYBRID_SCENE_UPDATE_BUILD_BLAS = 1 << 0,
	HYBRID_SCENE_UPDATE_REFIT_BLAS = 1 << 1,
	HYBRID_SCENE_UPDATE_TLAS = 1 << 2,
	HYBRID_SCENE_UPDATE_MATERIAL = 1 << 3,
};

struct HybridSceneRevision {
	uint64_t topology = 0;
	uint64_t deformation = 0;
	uint64_t transform = 0;
	uint64_t material = 0;
};

struct HybridSceneFrameStats {
	uint32_t blas_built = 0;
	uint32_t blas_refit = 0;
	uint32_t blas_reused = 0;
	uint32_t tlas_updated = 0;
	uint32_t materials_updated = 0;
	uint32_t pending_retirements = 0;
	uint32_t retired = 0;
};

class HybridSceneTracker {
	struct Entry {
		HybridSceneRevision revision;
		uint64_t last_seen_frame = 0;
		uint64_t retire_after_frame = 0;
		bool pending_retirement = false;
	};

	HashMap<uint64_t, Entry> entries;
	uint64_t frame = 0;
	uint32_t frames_in_flight = 3;
	HybridSceneFrameStats frame_stats;

public:
	explicit HybridSceneTracker(uint32_t p_frames_in_flight = 3);

	void begin_frame(uint64_t p_frame);
	uint32_t touch(uint64_t p_stable_id, const HybridSceneRevision &p_revision, bool p_allow_blas_refit);
	void end_frame();

	const HybridSceneFrameStats &get_frame_stats() const { return frame_stats; }
	uint32_t get_tracked_count() const { return entries.size(); }
};

} // namespace RendererPathTracing
