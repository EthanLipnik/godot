/**************************************************************************/
/*  rendering_server_types.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/io/image.h"
#include "core/math/aabb.h"
#include "core/math/rect2.h"
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cstdint>

template <typename T>
class Vector;

namespace RenderingServerTypes {

/* TEXTURE API */

typedef void (*TextureDetectCallback)(void *);
typedef void (*TextureDetectRoughnessCallback)(void *, const String &, RSE::TextureDetectRoughnessChannel);

struct TextureInfo {
	RID texture;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	Image::Format format;
	int64_t bytes;
	String path;
	RSE::TextureType type;
};

/* SHADER API */

struct ShaderNativeSourceCode {
	struct Version {
		struct Stage {
			String name;
			String code;
		};
		Vector<Stage> stages;
	};
	Vector<Version> versions;
};

/* MESH API */

struct SurfaceData {
	RSE::PrimitiveType primitive = RSE::PRIMITIVE_MAX;

	uint64_t format = RSE::ARRAY_FLAG_FORMAT_CURRENT_VERSION;
	Vector<uint8_t> vertex_data; // Vertex, Normal, Tangent (change with skinning, blendshape).
	Vector<uint8_t> attribute_data; // Color, UV, UV2, Custom0-3.
	Vector<uint8_t> skin_data; // Bone index, Bone weight.
	uint32_t vertex_count = 0;
	Vector<uint8_t> index_data;
	uint32_t index_count = 0;

	AABB aabb;
	struct LOD {
		float edge_length = 0.0f;
		Vector<uint8_t> index_data;
	};
	Vector<LOD> lods;
	Vector<AABB> bone_aabbs;

	// Transforms used in runtime bone AABBs compute.
	// Since bone AABBs is saved in Mesh space, but bones is in Skeleton space.
	Transform3D mesh_to_skeleton_xform;

	Vector<uint8_t> blend_shape_data;

	Vector4 uv_scale;

	RID material;
};

struct MeshInfo {
	RID mesh;
	String path;
	uint32_t vertex_buffer_size = 0;
	uint32_t attribute_buffer_size = 0;
	uint32_t skin_buffer_size = 0;
	uint32_t index_buffer_size = 0;
	uint32_t blend_shape_buffer_size = 0;
	uint32_t lod_index_buffers_size = 0;
	uint64_t vertex_count = 0;
};

/* STATUS INFORMATION */

struct FrameProfileArea {
	String name;
	double gpu_msec;
	double cpu_msec;
};

/* COMPOSITOR */

struct BlitToScreen {
	RID render_target;
	Rect2 src_rect = Rect2(0.0, 0.0, 1.0, 1.0);
	Rect2i dst_rect;

	struct {
		bool use_layer = false;
		uint32_t layer = 0;
	} multi_view;

	struct {
		//lens distorted parameters for VR
		bool apply = false;
		Vector2 eye_center;
		float k1 = 0.0;
		float k2 = 0.0;

		float upscale = 1.0;
		float aspect_ratio = 1.0;
	} lens_distortion;
};

/* BACKGROUND */

// Helper for RSE::SplashStretchMode, put here for convenience.
inline Rect2 get_splash_stretched_screen_rect(const Size2 &p_image_size, const Size2 &p_window_size, RSE::SplashStretchMode p_stretch_mode) {
	Size2 imgsize = p_image_size;
	Rect2 screenrect;
	switch (p_stretch_mode) {
		case RSE::SPLASH_STRETCH_MODE_DISABLED: {
			screenrect.size = imgsize;
			screenrect.position = ((p_window_size - screenrect.size) / 2.0).floor();
		} break;
		case RSE::SPLASH_STRETCH_MODE_KEEP: {
			if (p_window_size.width > p_window_size.height) {
				// Scale horizontally.
				screenrect.size.y = p_window_size.height;
				screenrect.size.x = imgsize.width * p_window_size.height / imgsize.height;
				screenrect.position.x = (p_window_size.width - screenrect.size.x) / 2;
			} else {
				// Scale vertically.
				screenrect.size.x = p_window_size.width;
				screenrect.size.y = imgsize.height * p_window_size.width / imgsize.width;
				screenrect.position.y = (p_window_size.height - screenrect.size.y) / 2;
			}
		} break;
		case RSE::SPLASH_STRETCH_MODE_KEEP_WIDTH: {
			// Scale vertically.
			screenrect.size.x = p_window_size.width;
			screenrect.size.y = imgsize.height * p_window_size.width / imgsize.width;
			screenrect.position.y = (p_window_size.height - screenrect.size.y) / 2;
		} break;
		case RSE::SPLASH_STRETCH_MODE_KEEP_HEIGHT: {
			// Scale horizontally.
			screenrect.size.y = p_window_size.height;
			screenrect.size.x = imgsize.width * p_window_size.height / imgsize.height;
			screenrect.position.x = (p_window_size.width - screenrect.size.x) / 2;
		} break;
		case RSE::SPLASH_STRETCH_MODE_COVER: {
			double window_aspect = (double)p_window_size.width / p_window_size.height;
			double img_aspect = imgsize.width / imgsize.height;

			if (window_aspect > img_aspect) {
				// Scale vertically.
				screenrect.size.x = p_window_size.width;
				screenrect.size.y = imgsize.height * p_window_size.width / imgsize.width;
				screenrect.position.y = (p_window_size.height - screenrect.size.y) / 2;
			} else {
				// Scale horizontally.
				screenrect.size.y = p_window_size.height;
				screenrect.size.x = imgsize.width * p_window_size.height / imgsize.height;
				screenrect.position.x = (p_window_size.width - screenrect.size.x) / 2;
			}
		} break;
		case RSE::SPLASH_STRETCH_MODE_IGNORE: {
			screenrect.size.x = p_window_size.width;
			screenrect.size.y = p_window_size.height;
		} break;
	}
	return screenrect;
}

/* RENDERING METHOD */

struct FluxTextureDiagnostics {
	uint32_t requested = 0;
	uint32_t resident = 0;
	uint32_t misses = 0;
};

struct FluxRayClassDiagnostics {
	uint32_t candidates = 0;
	uint32_t rejections = 0;
};

struct FluxStageTimings {
	double blas = 0.0;
	double tlas = 0.0;
	double ray_shadows = 0.0;
	double ray_effects = 0.0;
	double spatial = 0.0;
	double temporal = 0.0;
	double composition = 0.0;
};

// Backend-neutral, read-only diagnostics for one completed Flux viewport frame.
// `retained_non_primary_geometry_count` is selected transport geometry beyond
// raster-primary membership. It must not be interpreted as an exact off-frustum
// count because the conservative transport set may retain other non-primary work.
struct FluxDiagnostics {
	bool valid = false;
	// `frame` remains the legacy submitted-frame key. Keep the explicit name as
	// well so an asynchronously published capture is unambiguous to tools.
	uint64_t frame = 0;
	uint64_t submitted_frame = 0;
	uint64_t observed_frame = 0;
	uint64_t completion_frame_age = 0;
	uint32_t pending_capture_count = 0;
	bool raw_shadow_timing_available = false;
	bool shadow_residency_complete = false;
	int32_t effective_mode = 0;
	bool ray_effects_active = false;
	// `metalfx` means MetalFX is the sole image-space denoiser for this frame;
	// transport reservoirs/caches are deliberately not represented here.
	String denoiser = "none";
	bool flux_image_reconstruction = false;
	uint32_t viewport_internal_width = 0;
	uint32_t viewport_internal_height = 0;
	uint32_t viewport_target_width = 0;
	uint32_t viewport_target_height = 0;
	int32_t viewport_scaling_3d_mode = 0;
	float viewport_scaling_3d_scale = 1.0f;
	bool preview_admission_active = false;
	uint32_t preview_admission_blas_build_limit = 0;
	uint64_t preview_admission_blas_triangle_limit = 0;
	bool stbn_sampling_enabled = false;
	bool restir_di_enabled = false;
	bool regir_reuse_enabled = false;
	String regir_reuse_reason = "single_reservoir_cell_correlation_unvalidated";
	bool reusable_path_reuse_enabled = false;
	bool unified_finite_light_reuse_enabled = false;
	bool environment_active = false;
	String environment_status = "disabled";
	String environment_importance_cache = "no-distribution";
	uint32_t environment_radiance_width = 0;
	uint32_t environment_radiance_height = 0;
	uint32_t environment_proposal_width = 0;
	uint32_t environment_proposal_height = 0;
	uint32_t primary_surface_version = 0;
	bool ray_owned_shading = false;
	uint32_t primary_surface_view_count = 0;
	uint32_t primary_unsupported_surface_count = 0;
	bool transport_complete = false;
	String transport_incomplete_reason = "disabled";
	uint32_t invalid_pdf_sample_count = 0;
	uint32_t nonfinite_lobe_sample_count = 0;
	uint32_t rejected_energy_sample_count = 0;
	uint32_t primary_valid_pixel_count = 0;
	uint32_t primary_invalid_pixel_count = 0;
	uint32_t primary_lit_pixel_count = 0;

	uint32_t admitted_geometry_count = 0;
	uint32_t admitted_surface_count = 0;
	uint64_t admitted_base_triangle_count = 0;
	uint64_t admitted_selected_triangle_count = 0;
	uint32_t admitted_canonical_material_count = 0;

	String transport_state = "disabled";
	String transport_reason = "disabled";
	float transport_max_distance = 0.0f;
	uint32_t transport_primary_geometry_count = 0;
	uint32_t transport_selected_geometry_count = 0;
	uint32_t transport_eligible_geometry_count = 0;
	uint32_t transport_retained_non_primary_geometry_count = 0;
	uint32_t transport_selected_light_count = 0;
	uint32_t transport_eligible_light_count = 0;
	bool transport_retains_non_primary_geometry = false;
	uint32_t ray_proxy_source_count = 0;
	uint32_t ray_proxy_substituted_count = 0;
	uint32_t ray_proxy_fail_open_count = 0;
	uint32_t ray_proxy_duplicate_count = 0;
	uint32_t ray_proxy_rejection_counts[15] = {};

	bool material_tier2 = false;
	uint32_t material_capacity = 0;
	FluxTextureDiagnostics material_albedo;
	FluxTextureDiagnostics material_normal;
	FluxTextureDiagnostics material_orm;
	FluxTextureDiagnostics material_emissive;
	FluxTextureDiagnostics material_opacity;
	FluxTextureDiagnostics material_alpha_occupancy;
	uint32_t supported_thin_transmission_material_count = 0;
	uint32_t unsupported_transmission_texture_material_count = 0;
	uint32_t unsupported_transmission_volume_material_count = 0;

	bool work_attribution_valid = false;
	bool trace_compaction_active = false;
	bool trace_compaction_fallback = false;
	uint32_t trace_compaction_active_pixel_count = 0;
	uint32_t trace_compaction_inactive_pixel_count = 0;
	uint32_t trace_compaction_need_counts[5] = {};
	uint32_t trace_compaction_enqueued_counts[5] = {};
	uint32_t trace_compaction_dispatched_counts[5] = {};
	uint32_t acceleration_structure_deferred_build_count = 0;
	uint64_t acceleration_structure_deferred_triangle_count = 0;
	uint32_t acceleration_structure_tlas_rebuild_count = 0;
	uint32_t acceleration_structure_tlas_refit_count = 0;
	uint32_t acceleration_structure_tlas_reuse_count = 0;
	uint32_t alpha_mask_instance_count = 0;
	uint32_t alpha_candidate_count = 0;
	uint32_t alpha_rejection_count = 0;
	uint32_t alpha_candidate_exhaustion_count = 0;
	uint32_t alpha_mixed_intersection_count = 0;
	uint32_t alpha_split_opaque_query_counts[4] = {};
	uint32_t alpha_split_opaque_hit_counts[4] = {};
	uint32_t alpha_split_alpha_query_counts[4] = {};
	uint32_t alpha_split_alpha_hit_counts[4] = {};
	uint32_t alpha_split_alpha_rejection_counts[4] = {};
	uint32_t alpha_split_mixed_fallback_counts[4] = {};
	uint32_t alpha_rear_opaque_hit_count = 0;
	uint32_t alpha_max_candidates_per_ray = 0;
	uint32_t alpha_traversal_fallback_count = 0;
	FluxRayClassDiagnostics alpha_primary;
	FluxRayClassDiagnostics alpha_visibility;
	FluxRayClassDiagnostics alpha_reflection;
	FluxRayClassDiagnostics alpha_indirect;
	uint32_t alpha_occupancy_empty_rejection_count = 0;
	uint32_t alpha_occupancy_opaque_accept_count = 0;
	uint32_t alpha_occupancy_mixed_sample_count = 0;
	uint32_t primary_analytic_selected_count = 0;
	uint32_t primary_analytic_contributed_count = 0;
	uint32_t primary_analytic_visibility_test_count = 0;
	uint32_t direct_reservoir_candidate_count = 0;
	uint32_t direct_reservoir_temporal_reuse_count = 0;
	uint32_t direct_reservoir_spatial_reuse_count = 0;
	uint32_t direct_reservoir_visibility_test_count = 0;
	uint32_t regir_classified_candidate_count = 0;
	uint32_t regir_threadgroup_selected_count = 0;
	uint32_t regir_reduce_input_candidate_count = 0;
	uint32_t regir_reduced_valid_cell_count = 0;
	uint32_t regir_query_attempt_count = 0;
	uint32_t regir_query_valid_cell_count = 0;
	uint32_t regir_query_pdf_rejection_count = 0;
	uint32_t regir_query_zero_target_count = 0;
	uint32_t regir_fresh_fallback_count = 0;
	uint32_t regir_query_key_rejection_count = 0;
	uint32_t regir_query_revision_rejection_count = 0;
	uint32_t regir_query_payload_rejection_count = 0;
	uint32_t regir_merge_accepted_count = 0;
	uint32_t regir_merge_selected_count = 0;
	bool direct_reservoir_valid = false;
	uint32_t gi_fresh_ray_count = 0;
	uint32_t reflection_ray_count = 0;
	uint32_t gi_converged_skip_count = 0;
	uint32_t reflection_converged_skip_count = 0;
	uint32_t emissive_triangle_count = 0;
	uint32_t emissive_triangle_capacity = 0;
	uint32_t emissive_triangle_overflow = 0;
	uint32_t diffuse_cache_hit_count = 0;
	uint32_t diffuse_cache_update_count = 0;
	uint64_t diffuse_cache_bytes = 0;
	bool diffuse_cache_counters_valid = false;
	// World-space ReGIR is separate from per-view screen-space history. Counts
	// are published only when its cache allocation and submitted stages exist.
	bool regir_enabled = false;
	bool regir_valid = false;
	bool regir_complete = false;
	uint32_t regir_cell_count = 0;
	uint64_t regir_bytes = 0;
	// This is a bounded, world-keyed secondary-hit proposal cache. It never
	// owns screen-space history or cached visibility/radiance contributions.
	bool reusable_path_cache_enabled = false;
	bool reusable_path_cache_valid = false;
	bool reusable_path_cache_complete = false;
	uint32_t reusable_path_cache_cell_count = 0;
	uint32_t reusable_path_cache_occupied_cell_count = 0;
	bool reusable_path_cache_occupancy_valid = false;
	uint64_t reusable_path_cache_bytes = 0;
	uint32_t reusable_path_cache_staged_count = 0;
	uint32_t reusable_path_cache_update_count = 0;
	uint32_t reusable_path_cache_query_count = 0;
	uint32_t reusable_path_cache_valid_candidate_count = 0;
	uint32_t reusable_path_cache_reused_candidate_count = 0;
	uint32_t reusable_path_cache_rejection_count = 0;
	uint32_t reusable_path_cache_invalid_record_count = 0;
	uint32_t reusable_path_cache_endpoint_blocked_count = 0;
	uint32_t reusable_path_cache_shading_invalid_count = 0;
	uint32_t reusable_path_cache_zero_target_count = 0;
	uint32_t reusable_path_cache_invalid_weight_count = 0;
	uint32_t reusable_path_cache_considered_count = 0;
	uint32_t reusable_path_cache_accepted_count = 0;
	uint32_t reusable_path_cache_selected_count = 0;
	uint32_t reusable_path_cache_reevaluation_count = 0;
	uint32_t reusable_path_cache_reconnection_visibility_count = 0;
	uint32_t reusable_path_cache_lighting_reevaluation_count = 0;
	uint32_t reusable_path_cache_environment_reevaluation_count = 0;
	// Metal currently implements the backend prototype only. It is world-cache
	// proposal reuse, not a declaration of cross-backend feature parity.
	bool restir_gi_valid = false;
	bool restir_gi_complete = false;
	bool restir_gi_backend_prototype = false;
	uint32_t restir_gi_current_candidate_count = 0;
	uint32_t restir_gi_reused_candidate_count = 0;
	uint32_t restir_gi_selected_reuse_count = 0;
	// This deliberately narrow backend prototype is a current-frame virtual
	// finite-area source through one strictly planar delta mirror. It is not a
	// general bidirectional transport completion claim.
	bool bidirectional_caustic_enabled = false;
	bool bidirectional_caustic_active = false;
	bool bidirectional_caustic_complete = false;
	bool bidirectional_caustic_backend_prototype = false;
	uint32_t bidirectional_caustic_mirror_triangle_count = 0;
	uint32_t bidirectional_caustic_mirror_triangle_capacity = 0;
	uint32_t bidirectional_caustic_mirror_triangle_overflow = 0;
	uint32_t bidirectional_caustic_source_triangle_count = 0;
	uint32_t bidirectional_caustic_candidate_count = 0;
	uint32_t bidirectional_caustic_valid_count = 0;
	uint32_t bidirectional_caustic_contributed_count = 0;
	uint32_t bidirectional_caustic_visibility_ray_count = 0;
	uint32_t bidirectional_caustic_rejection_count = 0;
	uint32_t bidirectional_caustic_nonfinite_or_pdf_failure_count = 0;
	uint32_t punctual_light_count = 0;
	uint32_t punctual_light_overflow_count = 0;
	uint32_t unsupported_punctual_light_count = 0;
	uint64_t light_distribution_identity = 0;
	uint64_t light_revision_requested = 0;
	uint64_t light_revision_submitted = 0;
	uint64_t light_revision_completed = 0;
	// Frame-local revision tuple for the exact ray scene that was submitted.
	// They are suitable for rejecting screen-space transport history, but carry
	// no camera position, rotation, or ReGIR cell state.
	uint64_t admitted_geometry_generation = 0;
	uint64_t visibility_residency_generation = 0;
	uint64_t residency_completion_token = 0;
	bool residency_complete = false;
	uint64_t light_distribution_generation = 0;
	uint64_t environment_generation = 0;
	bool transport_revisions_valid = false;
	uint32_t transport_history_invalid_reasons = 0;
	// CPU wall phases measure only Flux request/Metal preparation/submission.
	// They intentionally do not label the rest of the frame as CPU work.
	double cpu_scene_preparation_milliseconds = 0.0;
	double cpu_residency_planning_milliseconds = 0.0;
	double cpu_residency_plan_cached_milliseconds = 0.0;
	double cpu_residency_plan_full_milliseconds = 0.0;
	uint64_t residency_plan_cache_hit_count = 0;
	uint64_t residency_plan_cache_miss_count = 0;
	uint64_t residency_plan_cache_rebuild_count = 0;
	double cpu_metal_preparation_milliseconds = 0.0;
	double cpu_submission_milliseconds = 0.0;

	bool timings_valid = false;
	// The submitted diagnostics frame that produced `timings_ms`. This can be
	// older than `frame` when a newer work-only snapshot carries forward the
	// latest completed timing sample for the same viewport owner.
	uint64_t timings_frame = 0;
	FluxStageTimings timings_ms;

	void reset_for_frame(uint64_t p_frame, int32_t p_effective_mode) {
		*this = FluxDiagnostics();
		valid = true;
		frame = p_frame;
		submitted_frame = p_frame;
		effective_mode = p_effective_mode;
	}

	static uint64_t saturating_frame_age(uint64_t p_submitted_frame, uint64_t p_observed_frame) {
		return p_observed_frame > p_submitted_frame ? p_observed_frame - p_submitted_frame : 0;
	}

	void set_completion_observation(uint64_t p_observed_frame, uint32_t p_pending_capture_count) {
		observed_frame = p_observed_frame;
		completion_frame_age = saturating_frame_age(submitted_frame, p_observed_frame);
		pending_capture_count = p_pending_capture_count;
	}

	void refresh_shadow_residency_completeness() {
		shadow_residency_complete = transport_complete && acceleration_structure_deferred_build_count == 0 && acceleration_structure_deferred_triangle_count == 0;
	}

	void carry_forward_timings_from(const FluxDiagnostics &p_timing_sample) {
		if (!timings_valid && p_timing_sample.timings_valid) {
			timings_valid = true;
			timings_frame = p_timing_sample.timings_frame;
			timings_ms = p_timing_sample.timings_ms;
		}
	}

	void set_transport_counts(uint32_t p_primary_geometry_count, uint32_t p_selected_geometry_count, uint32_t p_eligible_geometry_count) {
		transport_primary_geometry_count = p_primary_geometry_count;
		transport_selected_geometry_count = p_selected_geometry_count;
		transport_eligible_geometry_count = p_eligible_geometry_count;
		transport_retained_non_primary_geometry_count = p_selected_geometry_count > p_primary_geometry_count ? p_selected_geometry_count - p_primary_geometry_count : 0;
		transport_retains_non_primary_geometry = transport_retained_non_primary_geometry_count > 0;
	}

	void apply_work_attribution_transport_validation() {
		if (transport_complete && (invalid_pdf_sample_count > 0 || nonfinite_lobe_sample_count > 0)) {
			transport_complete = false;
			transport_incomplete_reason = "invalid transport sample rejected";
		}
		refresh_shadow_residency_completeness();
	}

	static Dictionary _texture_dictionary(const FluxTextureDiagnostics &p_texture) {
		Dictionary texture;
		texture["requested"] = p_texture.requested;
		texture["resident"] = p_texture.resident;
		texture["misses"] = p_texture.misses;
		return texture;
	}

	static Dictionary _ray_class_dictionary(const FluxRayClassDiagnostics &p_ray_class) {
		Dictionary ray_class;
		ray_class["candidate_count"] = p_ray_class.candidates;
		ray_class["rejection_count"] = p_ray_class.rejections;
		return ray_class;
	}

	Dictionary to_dictionary() const {
		Dictionary admitted;
		admitted["geometry_count"] = admitted_geometry_count;
		admitted["surface_count"] = admitted_surface_count;
		admitted["base_triangle_count"] = int64_t(admitted_base_triangle_count);
		admitted["selected_triangle_count"] = int64_t(admitted_selected_triangle_count);
		admitted["canonical_material_count"] = admitted_canonical_material_count;

		Dictionary transport;
		transport["state"] = transport_state;
		transport["reason"] = transport_reason;
		transport["max_distance"] = transport_max_distance;
		transport["primary_geometry_count"] = transport_primary_geometry_count;
		transport["selected_geometry_count"] = transport_selected_geometry_count;
		transport["eligible_geometry_count"] = transport_eligible_geometry_count;
		transport["retained_non_primary_geometry_count"] = transport_retained_non_primary_geometry_count;
		transport["selected_light_count"] = transport_selected_light_count;
		transport["eligible_light_count"] = transport_eligible_light_count;
		transport["retains_non_primary_geometry"] = transport_retains_non_primary_geometry;
		transport["ray_proxy_source_count"] = ray_proxy_source_count;
		transport["ray_proxy_substituted_count"] = ray_proxy_substituted_count;
		transport["ray_proxy_fail_open_count"] = ray_proxy_fail_open_count;
		transport["ray_proxy_duplicate_count"] = ray_proxy_duplicate_count;
		// Keep relation failures machine-readable for scene-streaming validation.
		Dictionary ray_proxy_rejections;
		const char *ray_proxy_reason_names[] = { "none", "valid", "unproven", "dynamic", "visible_proxy", "same_geometry", "transform_mismatch", "material_mismatch", "topology_mismatch", "invalid_containment", "invalid_surface_mapping", "nonopaque", "near_field", "cyclic", "missing_renderer_geometry" };
		for (uint32_t reason = 2; reason < 15; reason++) {
			ray_proxy_rejections[ray_proxy_reason_names[reason]] = ray_proxy_rejection_counts[reason];
		}
		transport["ray_proxy_rejections"] = ray_proxy_rejections;

		Dictionary materials;
		materials["tier2"] = material_tier2;
		materials["capacity"] = material_capacity;
		materials["albedo"] = _texture_dictionary(material_albedo);
		materials["normal"] = _texture_dictionary(material_normal);
		materials["orm"] = _texture_dictionary(material_orm);
		materials["emissive"] = _texture_dictionary(material_emissive);
		materials["opacity"] = _texture_dictionary(material_opacity);
		materials["alpha_occupancy"] = _texture_dictionary(material_alpha_occupancy);
		materials["supported_thin_transmission_count"] = supported_thin_transmission_material_count;
		materials["unsupported_transmission_texture_count"] = unsupported_transmission_texture_material_count;
		materials["unsupported_transmission_volume_count"] = unsupported_transmission_volume_material_count;

		Dictionary acceleration_structure;
		acceleration_structure["deferred_build_count"] = acceleration_structure_deferred_build_count;
		acceleration_structure["deferred_triangle_count"] = int64_t(acceleration_structure_deferred_triangle_count);
		acceleration_structure["tlas_rebuild_count"] = acceleration_structure_tlas_rebuild_count;
		acceleration_structure["tlas_refit_count"] = acceleration_structure_tlas_refit_count;
		acceleration_structure["tlas_reuse_count"] = acceleration_structure_tlas_reuse_count;
		Dictionary alpha_occupancy;
		alpha_occupancy["empty_rejection_count"] = alpha_occupancy_empty_rejection_count;
		alpha_occupancy["opaque_accept_count"] = alpha_occupancy_opaque_accept_count;
		alpha_occupancy["mixed_sample_count"] = alpha_occupancy_mixed_sample_count;
		Dictionary alpha;
		alpha["mask_instance_count"] = alpha_mask_instance_count;
		alpha["candidate_count"] = alpha_candidate_count;
		alpha["rejection_count"] = alpha_rejection_count;
		alpha["candidate_exhaustion_count"] = alpha_candidate_exhaustion_count;
		alpha["mixed_intersection_count"] = alpha_mixed_intersection_count;
		const char *alpha_ray_classes[] = { "primary", "visibility", "reflection", "gi" };
		Dictionary alpha_split_domains;
		for (uint32_t ray_class = 0; ray_class < 4; ray_class++) {
			Dictionary domain;
			domain["opaque_query_count"] = alpha_split_opaque_query_counts[ray_class];
			domain["opaque_hit_count"] = alpha_split_opaque_hit_counts[ray_class];
			domain["alpha_query_count"] = alpha_split_alpha_query_counts[ray_class];
			domain["alpha_hit_count"] = alpha_split_alpha_hit_counts[ray_class];
			domain["alpha_rejection_count"] = alpha_split_alpha_rejection_counts[ray_class];
			domain["mixed_fallback_count"] = alpha_split_mixed_fallback_counts[ray_class];
			alpha_split_domains[alpha_ray_classes[ray_class]] = domain;
		}
		alpha["split_domains"] = alpha_split_domains;
		alpha["rear_opaque_hit_count"] = alpha_rear_opaque_hit_count;
		alpha["max_candidates_per_ray"] = alpha_max_candidates_per_ray;
		alpha["traversal_fallback_count"] = alpha_traversal_fallback_count;
		alpha["primary"] = _ray_class_dictionary(alpha_primary);
		alpha["visibility"] = _ray_class_dictionary(alpha_visibility);
		alpha["reflection"] = _ray_class_dictionary(alpha_reflection);
		alpha["indirect"] = _ray_class_dictionary(alpha_indirect);
		alpha["occupancy"] = alpha_occupancy;
		Dictionary primary_analytic;
		primary_analytic["selected_count"] = primary_analytic_selected_count;
		primary_analytic["contributed_count"] = primary_analytic_contributed_count;
		primary_analytic["visibility_test_count"] = primary_analytic_visibility_test_count;
		Dictionary direct_reservoir;
		direct_reservoir["candidate_count"] = direct_reservoir_candidate_count;
		direct_reservoir["temporal_reuse_count"] = direct_reservoir_temporal_reuse_count;
		direct_reservoir["spatial_reuse_count"] = direct_reservoir_spatial_reuse_count;
		direct_reservoir["visibility_test_count"] = direct_reservoir_visibility_test_count;
		direct_reservoir["regir_classified_candidate_count"] = regir_classified_candidate_count;
		direct_reservoir["regir_threadgroup_selected_count"] = regir_threadgroup_selected_count;
		direct_reservoir["regir_reduce_input_candidate_count"] = regir_reduce_input_candidate_count;
		direct_reservoir["regir_reduced_valid_cell_count"] = regir_reduced_valid_cell_count;
		direct_reservoir["regir_query_attempt_count"] = regir_query_attempt_count;
		direct_reservoir["regir_query_valid_cell_count"] = regir_query_valid_cell_count;
		direct_reservoir["regir_query_pdf_rejection_count"] = regir_query_pdf_rejection_count;
		direct_reservoir["regir_query_zero_target_count"] = regir_query_zero_target_count;
		direct_reservoir["regir_fresh_fallback_count"] = regir_fresh_fallback_count;
		direct_reservoir["regir_query_key_rejection_count"] = regir_query_key_rejection_count;
		direct_reservoir["regir_query_revision_rejection_count"] = regir_query_revision_rejection_count;
		direct_reservoir["regir_query_payload_rejection_count"] = regir_query_payload_rejection_count;
		direct_reservoir["regir_merge_accepted_count"] = regir_merge_accepted_count;
		direct_reservoir["regir_merge_selected_count"] = regir_merge_selected_count;
		direct_reservoir["valid"] = direct_reservoir_valid;
		Dictionary ray_work;
		ray_work["gi_fresh_ray_count"] = gi_fresh_ray_count;
		ray_work["gi_converged_skip_count"] = gi_converged_skip_count;
		ray_work["gi_cache_probe_count"] = reusable_path_cache_query_count;
		ray_work["gi_reconnection_visibility_count"] = reusable_path_cache_reconnection_visibility_count;
		ray_work["reflection_ray_count"] = reflection_ray_count;
		ray_work["reflection_converged_skip_count"] = reflection_converged_skip_count;
		ray_work["alpha_intersection_count"] = alpha_mixed_intersection_count;
		Dictionary emissive;
		emissive["triangle_count"] = emissive_triangle_count;
		emissive["triangle_capacity"] = emissive_triangle_capacity;
		emissive["triangle_overflow"] = emissive_triangle_overflow;
		Dictionary diffuse_cache;
		diffuse_cache["hit_count"] = diffuse_cache_hit_count;
		diffuse_cache["update_count"] = diffuse_cache_update_count;
		diffuse_cache["bytes"] = int64_t(diffuse_cache_bytes);
		diffuse_cache["counters_valid"] = diffuse_cache_counters_valid;
		Dictionary regir;
		regir["enabled"] = regir_enabled;
		regir["reason"] = regir_reuse_reason;
		regir["valid"] = regir_valid;
		regir["complete"] = regir_complete;
		regir["cell_count"] = regir_cell_count;
		regir["bytes"] = int64_t(regir_bytes);
		Dictionary reusable_path_cache;
		reusable_path_cache["enabled"] = reusable_path_cache_enabled;
		reusable_path_cache["valid"] = reusable_path_cache_valid;
		reusable_path_cache["complete"] = reusable_path_cache_complete;
		reusable_path_cache["cell_count"] = reusable_path_cache_cell_count;
		reusable_path_cache["occupied_cell_count"] = reusable_path_cache_occupied_cell_count;
		reusable_path_cache["occupancy_valid"] = reusable_path_cache_occupancy_valid;
		reusable_path_cache["bytes"] = int64_t(reusable_path_cache_bytes);
		reusable_path_cache["staged_count"] = reusable_path_cache_staged_count;
		reusable_path_cache["update_count"] = reusable_path_cache_update_count;
		reusable_path_cache["query_count"] = reusable_path_cache_query_count;
		reusable_path_cache["valid_candidate_count"] = reusable_path_cache_valid_candidate_count;
		reusable_path_cache["reused_candidate_count"] = reusable_path_cache_reused_candidate_count;
		reusable_path_cache["rejection_count"] = reusable_path_cache_rejection_count;
		Dictionary reusable_path_post_validation;
		reusable_path_post_validation["invalid_record_count"] = reusable_path_cache_invalid_record_count;
		reusable_path_post_validation["endpoint_blocked_count"] = reusable_path_cache_endpoint_blocked_count;
		reusable_path_post_validation["shading_invalid_count"] = reusable_path_cache_shading_invalid_count;
		reusable_path_post_validation["zero_target_count"] = reusable_path_cache_zero_target_count;
		reusable_path_post_validation["invalid_weight_count"] = reusable_path_cache_invalid_weight_count;
		reusable_path_post_validation["considered_count"] = reusable_path_cache_considered_count;
		reusable_path_post_validation["accepted_count"] = reusable_path_cache_accepted_count;
		reusable_path_post_validation["selected_count"] = reusable_path_cache_selected_count;
		reusable_path_cache["post_validation"] = reusable_path_post_validation;
		reusable_path_cache["reevaluation_count"] = reusable_path_cache_reevaluation_count;
		reusable_path_cache["reconnection_visibility_count"] = reusable_path_cache_reconnection_visibility_count;
		reusable_path_cache["lighting_reevaluation_count"] = reusable_path_cache_lighting_reevaluation_count;
		reusable_path_cache["environment_reevaluation_count"] = reusable_path_cache_environment_reevaluation_count;
		Dictionary restir_gi;
		restir_gi["valid"] = restir_gi_valid;
		restir_gi["complete"] = restir_gi_complete;
		restir_gi["backend_prototype"] = restir_gi_backend_prototype;
		restir_gi["current_candidate_count"] = restir_gi_current_candidate_count;
		restir_gi["reused_candidate_count"] = restir_gi_reused_candidate_count;
		restir_gi["selected_reuse_count"] = restir_gi_selected_reuse_count;
		Dictionary bidirectional_caustic;
		bidirectional_caustic["enabled"] = bidirectional_caustic_enabled;
		bidirectional_caustic["active"] = bidirectional_caustic_active;
		bidirectional_caustic["complete"] = bidirectional_caustic_complete;
		bidirectional_caustic["backend_prototype"] = bidirectional_caustic_backend_prototype;
		bidirectional_caustic["mirror_triangle_count"] = bidirectional_caustic_mirror_triangle_count;
		bidirectional_caustic["mirror_triangle_capacity"] = bidirectional_caustic_mirror_triangle_capacity;
		bidirectional_caustic["mirror_triangle_overflow"] = bidirectional_caustic_mirror_triangle_overflow;
		bidirectional_caustic["source_triangle_count"] = bidirectional_caustic_source_triangle_count;
		bidirectional_caustic["candidate_count"] = bidirectional_caustic_candidate_count;
		bidirectional_caustic["valid_count"] = bidirectional_caustic_valid_count;
		bidirectional_caustic["contributed_count"] = bidirectional_caustic_contributed_count;
		bidirectional_caustic["visibility_ray_count"] = bidirectional_caustic_visibility_ray_count;
		bidirectional_caustic["rejection_count"] = bidirectional_caustic_rejection_count;
		bidirectional_caustic["nonfinite_or_pdf_failure_count"] = bidirectional_caustic_nonfinite_or_pdf_failure_count;
		Dictionary punctual_lights;
		punctual_lights["count"] = punctual_light_count;
		punctual_lights["overflow_count"] = punctual_light_overflow_count;
		punctual_lights["unsupported_count"] = unsupported_punctual_light_count;
		Dictionary transport_revisions;
		transport_revisions["valid"] = transport_revisions_valid;
		transport_revisions["admitted_geometry_generation"] = int64_t(admitted_geometry_generation);
		transport_revisions["visibility_residency_generation"] = int64_t(visibility_residency_generation);
		transport_revisions["residency_completion_token"] = int64_t(residency_completion_token);
		transport_revisions["residency_complete"] = residency_complete;
		transport_revisions["light_distribution_generation"] = int64_t(light_distribution_generation);
		transport_revisions["environment_generation"] = int64_t(environment_generation);
		Dictionary history_validity;
		history_validity["camera_or_screen"] = (transport_history_invalid_reasons & 1u) == 0;
		history_validity["residency_ready"] = (transport_history_invalid_reasons & 2u) == 0;
		history_validity["geometry"] = (transport_history_invalid_reasons & 4u) == 0;
		history_validity["light_distribution"] = (transport_history_invalid_reasons & 8u) == 0;
		history_validity["environment"] = (transport_history_invalid_reasons & 16u) == 0;
		history_validity["reconstruction_reset"] = (transport_history_invalid_reasons & 32u) == 0;
		transport_revisions["history_validity"] = history_validity;
		Dictionary light_revisions;
		light_revisions["requested"] = int64_t(light_revision_requested);
		light_revisions["submitted"] = int64_t(light_revision_submitted);
		light_revisions["completed"] = int64_t(light_revision_completed);
		Dictionary trace_compaction;
		trace_compaction["active"] = trace_compaction_active;
		trace_compaction["fallback"] = trace_compaction_fallback;
		trace_compaction["inactive_pixels"] = trace_compaction_inactive_pixel_count;
		const char *trace_compaction_class_names[] = { "direct_only", "gi", "reflection", "exact_alpha", "complex_light" };
		uint32_t enqueued_total = 0;
		uint32_t dispatched_total = 0;
		for (uint32_t class_index = 0; class_index < 5; class_index++) {
			Dictionary class_counts;
			class_counts["need_count"] = trace_compaction_need_counts[class_index];
			class_counts["enqueued_count"] = trace_compaction_enqueued_counts[class_index];
			class_counts["dispatched_count"] = trace_compaction_dispatched_counts[class_index];
			trace_compaction[trace_compaction_class_names[class_index]] = class_counts;
			enqueued_total += trace_compaction_enqueued_counts[class_index];
			dispatched_total += trace_compaction_dispatched_counts[class_index];
		}
		trace_compaction["active_pixel_count"] = trace_compaction_active_pixel_count;
		Dictionary trace_compaction_invariants;
		trace_compaction_invariants["sum_enqueued_equals_active"] = !trace_compaction_active || enqueued_total == trace_compaction_active_pixel_count;
		trace_compaction_invariants["sum_dispatched_equals_active"] = !trace_compaction_active || dispatched_total == trace_compaction_active_pixel_count;
		trace_compaction_invariants["inactive_excluded_from_trace"] = !trace_compaction_active || dispatched_total <= trace_compaction_active_pixel_count;
		trace_compaction["invariants"] = trace_compaction_invariants;

		Dictionary timings;
		timings["blas"] = timings_ms.blas;
		timings["tlas"] = timings_ms.tlas;
		timings["ray_shadows"] = timings_ms.ray_shadows;
		timings["ray_effects"] = timings_ms.ray_effects;
		timings["spatial"] = timings_ms.spatial;
		timings["temporal"] = timings_ms.temporal;
		timings["composition"] = timings_ms.composition;
		Dictionary cpu_phases;
		cpu_phases["scene_preparation"] = cpu_scene_preparation_milliseconds;
		cpu_phases["residency_planning"] = cpu_residency_planning_milliseconds;
		cpu_phases["residency_plan_cached"] = cpu_residency_plan_cached_milliseconds;
		cpu_phases["residency_plan_full"] = cpu_residency_plan_full_milliseconds;
		cpu_phases["metal_preparation"] = cpu_metal_preparation_milliseconds;
		cpu_phases["submission"] = cpu_submission_milliseconds;
		Dictionary diagnostics;
		Dictionary viewport;
		viewport["internal_width"] = viewport_internal_width;
		viewport["internal_height"] = viewport_internal_height;
		viewport["target_width"] = viewport_target_width;
		viewport["target_height"] = viewport_target_height;
		viewport["scaling_3d_mode"] = viewport_scaling_3d_mode;
		viewport["scaling_3d_scale"] = viewport_scaling_3d_scale;
		viewport["preview_admission_active"] = preview_admission_active;
		viewport["preview_admission_blas_build_limit"] = preview_admission_blas_build_limit;
		viewport["preview_admission_blas_triangle_limit"] = int64_t(preview_admission_blas_triangle_limit);
		Dictionary reuse;
		reuse["stbn_sampling_enabled"] = stbn_sampling_enabled;
		reuse["restir_di_enabled"] = restir_di_enabled;
		reuse["regir_enabled"] = regir_reuse_enabled;
		reuse["regir_reason"] = regir_reuse_reason;
		reuse["reusable_path_enabled"] = reusable_path_reuse_enabled;
		reuse["unified_finite_light_enabled"] = unified_finite_light_reuse_enabled;
		Dictionary residency_plan;
		residency_plan["cache_hit_count"] = int64_t(residency_plan_cache_hit_count);
		residency_plan["cache_miss_count"] = int64_t(residency_plan_cache_miss_count);
		residency_plan["rebuild_count"] = int64_t(residency_plan_cache_rebuild_count);
		diagnostics["residency_plan"] = residency_plan;

		diagnostics["valid"] = valid;
		diagnostics["frame"] = int64_t(frame);
		// Completion age is diagnostic delivery age, not light/shadow latency.
		Dictionary submission_completion;
		submission_completion["submitted_frame"] = int64_t(submitted_frame);
		submission_completion["observed_frame"] = int64_t(observed_frame);
		submission_completion["completion_frame_age"] = int64_t(completion_frame_age);
		submission_completion["pending_capture_count"] = pending_capture_count;
		submission_completion["ray_shadow_timing_available"] = raw_shadow_timing_available;
		submission_completion["residency_complete"] = shadow_residency_complete;
		diagnostics["effective_mode"] = effective_mode;
		diagnostics["ray_effects_active"] = ray_effects_active;
		diagnostics["denoiser"] = denoiser;
		diagnostics["flux_image_reconstruction"] = flux_image_reconstruction;
		diagnostics["viewport"] = viewport;
		diagnostics["reuse"] = reuse;
		diagnostics["environment_active"] = environment_active;
		diagnostics["environment_status"] = environment_status;
		Dictionary environment_importance;
		environment_importance["cache"] = environment_importance_cache;
		environment_importance["radiance_width"] = environment_radiance_width;
		environment_importance["radiance_height"] = environment_radiance_height;
		environment_importance["proposal_width"] = environment_proposal_width;
		environment_importance["proposal_height"] = environment_proposal_height;
		diagnostics["environment_importance"] = environment_importance;
		diagnostics["primary_surface_version"] = primary_surface_version;
		diagnostics["ray_owned_shading"] = ray_owned_shading;
		diagnostics["primary_surface_view_count"] = primary_surface_view_count;
		diagnostics["primary_unsupported_surface_count"] = primary_unsupported_surface_count;
		diagnostics["transport_complete"] = transport_complete;
		diagnostics["transport_incomplete_reason"] = transport_incomplete_reason;
		diagnostics["residency_complete"] = residency_complete;
		diagnostics["invalid_pdf_sample_count"] = invalid_pdf_sample_count;
		diagnostics["nonfinite_lobe_sample_count"] = nonfinite_lobe_sample_count;
		diagnostics["rejected_energy_sample_count"] = rejected_energy_sample_count;
		diagnostics["primary_valid_pixel_count"] = primary_valid_pixel_count;
		diagnostics["primary_invalid_pixel_count"] = primary_invalid_pixel_count;
		diagnostics["primary_lit_pixel_count"] = primary_lit_pixel_count;
		diagnostics["admitted"] = admitted;
		diagnostics["transport"] = transport;
		diagnostics["materials"] = materials;
		diagnostics["work_attribution_valid"] = work_attribution_valid;
		diagnostics["trace_compaction"] = trace_compaction;
		diagnostics["acceleration_structure"] = acceleration_structure;
		diagnostics["alpha"] = alpha;
		diagnostics["primary_analytic"] = primary_analytic;
		diagnostics["direct_reservoir"] = direct_reservoir;
		diagnostics["ray_work"] = ray_work;
		diagnostics["emissive"] = emissive;
		diagnostics["diffuse_cache"] = diffuse_cache;
		diagnostics["reusable_path_cache"] = reusable_path_cache;
		diagnostics["restir_gi"] = restir_gi;
		diagnostics["bidirectional_caustic"] = bidirectional_caustic;
		diagnostics["regir"] = regir;
		diagnostics["punctual_lights"] = punctual_lights;
		diagnostics["light_distribution_identity"] = int64_t(light_distribution_identity);
		diagnostics["light_revisions"] = light_revisions;
		diagnostics["transport_revisions"] = transport_revisions;
		diagnostics["submission_completion"] = submission_completion;
		diagnostics["timings_valid"] = timings_valid;
		diagnostics["timings_frame"] = int64_t(timings_frame);
		diagnostics["timings_ms"] = timings;
		diagnostics["cpu_phases_ms"] = cpu_phases;
		return diagnostics;
	}
};

struct FluxDiagnosticsPendingState {
	FluxDiagnostics diagnostics;
	bool timing_expected = false;
	bool timing_ready = false;
	bool work_attribution_expected = false;
	bool work_attribution_ready = false;

	bool ready_to_publish() const {
		return (!timing_expected || timing_ready) && (!work_attribution_expected || work_attribution_ready);
	}
};

struct RenderInfo {
	int info[RSE::VIEWPORT_RENDER_INFO_TYPE_MAX][RSE::VIEWPORT_RENDER_INFO_MAX] = {};
	uint64_t flux_owner_id = 0;
	uint64_t flux_current_frame = 0;
	int32_t flux_last_effective_mode = -1;
	// A viewport-local, one-time admission boost used by the editor preview.
	// It is intentionally not a project setting and is cleared only after the
	// submitted Flux diagnostics prove residency complete.
	uint32_t flux_preview_blas_build_limit = 0;
	uint64_t flux_preview_blas_triangle_limit = 0;
	FluxDiagnostics flux;
};

} // namespace RenderingServerTypes
