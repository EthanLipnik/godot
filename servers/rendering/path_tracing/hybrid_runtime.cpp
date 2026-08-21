/**************************************************************************/
/*  hybrid_runtime.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "hybrid_runtime.h"

#include "core/variant/variant.h"

namespace RendererPathTracing {

static String _candidate_failure(const ReconstructionRequest &p_request, const ReconstructionCapabilities &p_candidate) {
	if (!p_candidate.available) {
		return p_candidate.unavailable_reason.is_empty() ? "adapter is unavailable" : p_candidate.unavailable_reason;
	}
	if (p_request.view_count == 0 || p_request.view_count > p_candidate.max_views) {
		return "requested view count is unsupported";
	}
	if (p_request.view_count > 1 && !p_candidate.stereo) {
		return "independent stereo histories are unsupported";
	}
	if (p_request.require_temporal_denoising && !p_candidate.temporal_denoising) {
		return "temporal denoising is unsupported";
	}
	if (p_request.require_super_resolution && !p_candidate.super_resolution) {
		return "super resolution is unsupported";
	}
	const uint32_t missing_guides = p_candidate.required_guides & ~p_request.available_guides;
	if (missing_guides != 0) {
		return vformat("required guide mask 0x%03x is missing 0x%03x", p_candidate.required_guides, missing_guides);
	}
	return String();
}

ReconstructionSelection select_reconstruction_adapter(const ReconstructionRequest &p_request, const Vector<ReconstructionCapabilities> &p_candidates) {
	ReconstructionSelection selection;
	Vector<String> failures;
	for (int index = 0; index < p_candidates.size(); index++) {
		const ReconstructionCapabilities &candidate = p_candidates[index];
		const String failure = _candidate_failure(p_request, candidate);
		if (failure.is_empty()) {
			selection.adapter_index = index;
			selection.adapter_name = candidate.name;
			selection.kind = candidate.kind;
			return selection;
		}
		failures.push_back(vformat("%s: %s", String(candidate.name), failure));
	}
	selection.unavailable_reason = failures.is_empty() ? "No reconstruction adapters were registered." : String("No reconstruction adapter satisfies the runtime contract: ") + String("; ").join(failures);
	return selection;
}

HybridSceneTracker::HybridSceneTracker(uint32_t p_frames_in_flight) {
	frames_in_flight = MAX(1u, p_frames_in_flight);
}

void HybridSceneTracker::begin_frame(uint64_t p_frame) {
	frame = p_frame;
	frame_stats = HybridSceneFrameStats();
}

uint32_t HybridSceneTracker::touch(uint64_t p_stable_id, const HybridSceneRevision &p_revision, bool p_allow_blas_refit) {
	DEV_ASSERT(p_stable_id != 0);
	Entry *entry = entries.getptr(p_stable_id);
	uint32_t updates = HYBRID_SCENE_UPDATE_NONE;
	if (!entry) {
		Entry new_entry;
		new_entry.revision = p_revision;
		new_entry.last_seen_frame = frame;
		entries.insert(p_stable_id, new_entry);
		updates = HYBRID_SCENE_UPDATE_BUILD_BLAS | HYBRID_SCENE_UPDATE_TLAS | HYBRID_SCENE_UPDATE_MATERIAL;
	} else {
		entry->last_seen_frame = frame;
		entry->pending_retirement = false;
		entry->retire_after_frame = 0;
		if (entry->revision.topology != p_revision.topology) {
			updates |= HYBRID_SCENE_UPDATE_BUILD_BLAS | HYBRID_SCENE_UPDATE_TLAS;
		} else if (entry->revision.deformation != p_revision.deformation) {
			updates |= p_allow_blas_refit ? HYBRID_SCENE_UPDATE_REFIT_BLAS : HYBRID_SCENE_UPDATE_BUILD_BLAS;
		}
		if (entry->revision.transform != p_revision.transform) {
			updates |= HYBRID_SCENE_UPDATE_TLAS;
		}
		if (entry->revision.material != p_revision.material) {
			updates |= HYBRID_SCENE_UPDATE_MATERIAL;
		}
		entry->revision = p_revision;
	}

	if (updates & HYBRID_SCENE_UPDATE_BUILD_BLAS) {
		frame_stats.blas_built++;
	} else if (updates & HYBRID_SCENE_UPDATE_REFIT_BLAS) {
		frame_stats.blas_refit++;
	} else {
		frame_stats.blas_reused++;
	}
	if (updates & HYBRID_SCENE_UPDATE_TLAS) {
		frame_stats.tlas_updated++;
	}
	if (updates & HYBRID_SCENE_UPDATE_MATERIAL) {
		frame_stats.materials_updated++;
	}
	return updates;
}

void HybridSceneTracker::end_frame() {
	Vector<uint64_t> retired;
	for (KeyValue<uint64_t, Entry> &pair : entries) {
		Entry &entry = pair.value;
		if (entry.last_seen_frame == frame) {
			continue;
		}
		if (!entry.pending_retirement) {
			entry.pending_retirement = true;
			entry.retire_after_frame = frame + frames_in_flight;
		}
		if (frame >= entry.retire_after_frame) {
			retired.push_back(pair.key);
		} else {
			frame_stats.pending_retirements++;
		}
	}
	for (uint64_t stable_id : retired) {
		entries.erase(stable_id);
		frame_stats.retired++;
	}
}

} // namespace RendererPathTracing
