/**************************************************************************/
/*  metal_flux_effect.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#ifdef METAL_ENABLED

#include "core/math/color.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "servers/rendering/path_tracing/environment_importance.h"
#include "servers/rendering/path_tracing/hybrid_residency.h"
#include "servers/rendering/path_tracing/hybrid_runtime.h"
#include "servers/rendering/path_tracing/indoor_lighting.h"
#include "servers/rendering/path_tracing/sampling_sequence.h"
#include "servers/rendering/path_tracing/streamed_cluster_runtime.h"
#include "servers/rendering/path_tracing/transport_culling.h"
#include "servers/rendering/sky_lighting.h"

namespace RendererRD {

struct MetalFluxEffectCache;

class MetalFluxEffect {
public:
	struct Surface {
		uint64_t stable_id = 0;
		uint64_t topology_revision = 0;
		uint64_t deformation_revision = 0;
		RID vertex_buffer;
		RID index_buffer;
		RID attribute_buffer;
		uint64_t vertex_buffer_offset = 0;
		uint64_t index_buffer_offset = 0;
		uint64_t attribute_buffer_offset = 0;
		uint32_t vertex_count = 0;
		uint32_t index_count = 0;
		uint32_t vertex_stride = 0;
		// Zero preserves the ordinary MeshStorage convention (16-bit when the
		// local vertex count permits it). Streamed pages set this explicitly.
		uint32_t index_stride = 0;
		uint32_t normal_offset = 0;
		uint32_t normal_stride = 0;
		uint32_t attribute_stride = 0;
		uint32_t uv_offset = 0;
		AABB compressed_aabb;
		bool compressed = false;
		bool has_normals = false;
		bool has_tangents = false;
		bool has_uv = false;
		bool dynamic = false;
		bool admission_emissive = false;
		float admission_camera_distance = 1.0e30f;
		// Stable portable ray-group semantics. Zero denotes conventional geometry.
		uint64_t ray_group_id = 0;
		uint64_t transport_region_id = 0;
		uint64_t ray_group_revision = 0;
		uint32_t ray_tier = 0; // RendererVirtualGeometry::RayTransportTier.
		bool persistent_coarse = false;
	};

	struct Instance {
		enum AlphaMode : uint32_t {
			ALPHA_OPAQUE = 0,
			ALPHA_MASK = 1,
		};

		uint64_t stable_id = 0;
		uint64_t surface_id = 0;
		uint64_t material_stable_id = 0;
		uint64_t material_generation = 1;
		Transform3D transform;
		bool dynamic = false;
		uint32_t visibility_mask = 0xffffffffu;
		Color albedo = Color(1.0, 1.0, 1.0, 1.0);
		Color emission = Color(0.0, 0.0, 0.0, 1.0);
		// Renderer-owned RD texture views. Color textures retain their sRGB view;
		// data textures retain their linear view and BC storage when available.
		RID albedo_texture;
		RID normal_texture;
		RID orm_texture;
		RID metallic_texture;
		RID roughness_texture;
		RID ambient_occlusion_texture;
		RID emission_texture;
		RID opacity_texture;
		RID alpha_occupancy_texture;
		Vector2 uv_scale = Vector2(1.0, 1.0);
		Vector2 uv_offset;
		Color metallic_texture_channel = Color(1.0, 0.0, 0.0, 0.0);
		Color roughness_texture_channel = Color(1.0, 0.0, 0.0, 0.0);
		Color ambient_occlusion_texture_channel = Color(1.0, 0.0, 0.0, 0.0);
		float metallic = 0.0f;
		float roughness = 1.0f;
		float specular = 0.5f;
		float thin_transmission = 0.0f;
		float thin_ior = 1.5f;
		uint32_t thin_transmission_unsupported_features = 0;
		float normal_scale = 1.0f;
		float ambient_occlusion_strength = 1.0f;
		float alpha_cutoff = 0.5f;
		float emission_texture_scale = 1.0f;
		AlphaMode alpha_mode = ALPHA_OPAQUE;
		bool orm_packed = false;
		bool emission_multiply = true;
		bool canonical_material = true;
		uint32_t face_flags = 0; // bit 0 two-sided, bits 1..2 CullMode.
		uint64_t ray_group_id = 0;
		uint64_t transport_region_id = 0;
		uint32_t ray_tier = 0;
		bool off_screen_transport = false;
	};

	// A small renderer-owned analytic-light contract shared by primary direct
	// shading and secondary transport. Direct radiance is kept separate from the
	// authored indirect-energy multiplier so the primary path is not dimmed by a
	// GI-only control.
	struct PunctualLight {
		enum Type : uint32_t {
			TYPE_OMNI = 0,
			TYPE_SPOT = 1,
			TYPE_AREA = 2,
			TYPE_DIRECTIONAL = 3,
		};
		Vector3 position;
		// The light's local -Z axis transformed into world space. It is the
		// emission axis for Spot and the one-sided normal for finite Area.
		Vector3 direction = Vector3(0.0f, 0.0f, -1.0f);
		Vector3 area_u = Vector3(1.0f, 0.0f, 0.0f);
		Vector3 area_v = Vector3(0.0f, 1.0f, 0.0f);
		Color radiance = Color(0.0, 0.0, 0.0, 1.0);
		float range = 0.0f;
		float attenuation = 1.0f;
		float spot_cos_outer = -1.0f;
		float spot_attenuation = 1.0f;
		uint32_t cull_mask = 0xffffffffu;
		uint32_t shadow_caster_mask = 0xffffffffu;
		uint64_t stable_id = 0;
		float shadow_opacity = 1.0f;
		float specular_amount = 1.0f;
		float indirect_energy = 1.0f;
		bool shadow_enabled = false;
		bool negative = false;
		Type type = TYPE_OMNI;
	};

	struct View {
		// Render-buffer identity supplied by the scene renderer. Submitted-view order is
		// not stable across viewports, mirrors, or stereo reconfiguration.
		uint64_t history_owner_id = 0;
		uint32_t eye_index = 0;
		RID color;
		RID depth;
		RID normal_roughness;
		RID primary_material;
		RID primary_identity;
		RID primary_geometry;
		RID primary_flags;
		RID effect_output;
		RID filtered_output;
		RID velocity;
		RID history_input;
		RID history_output;
		RID depth_history_input;
		RID depth_history_output;
		RID normal_history_input;
		RID normal_history_output;
		RID guide_normal;
		RID guide_diffuse;
		RID guide_specular;
		RID guide_roughness;
		RID guide_denoise_strength;
		RID guide_reactive;
		RID guide_specular_distance;
		RID guide_transparency;
		Projection clip_from_view;
		Transform3D world_from_view;
		Projection prev_clip_from_world;
	};

	struct FrameRequest {
		Vector<Surface> surfaces;
		Vector<Instance> instances;
		Vector<PunctualLight> punctual_lights;
		Vector<View> views;
		uint64_t ray_geometry_base_triangles = 0;
		uint64_t ray_geometry_selected_triangles = 0;
		uint32_t ray_lod_instance_surfaces = 0;
		uint32_t ray_lod_base_dynamic_surfaces = 0;
		uint32_t ray_lod_base_alpha_mask_surfaces = 0;
		uint32_t ray_lod_base_near_field_surfaces = 0;
		uint32_t maximum_blas_builds_per_frame = 32;
		uint64_t maximum_blas_build_triangles_per_frame = 500000;
		bool preview_admission_active = false;
		uint32_t punctual_light_overflow = 0;
		uint32_t unsupported_punctual_lights = 0;
		uint32_t unsupported_materials = 0;
		// Bounded observability for authored StandardMaterial features that are
		// intentionally outside the current opaque Flux closure. These counters
		// never reject an otherwise supported opaque material.
		uint32_t unsupported_clearcoat_materials = 0;
		uint32_t unsupported_anisotropy_materials = 0;
		uint32_t unsupported_transmission_materials = 0;
		uint32_t supported_thin_transmission_materials = 0;
		uint32_t unsupported_transmission_texture_materials = 0;
		uint32_t unsupported_transmission_volume_materials = 0;
		uint32_t unsupported_refraction_materials = 0;
		Vector3 directional_light_direction;
		Color directional_light_radiance;
		uint32_t directional_light_cull_mask = 0xffffffffu;
		uint32_t directional_shadow_caster_mask = 0xffffffffu;
		bool directional_light_active = false;
		bool directional_shadow_enabled = false;
		bool directional_negative = false;
		float directional_shadow_opacity = 1.0f;
		float directional_specular_amount = 1.0f;
		float reflection_strength = 1.0f;
		float reflection_roughness_cutoff = 0.45f;
		float ambient_occlusion_strength = 0.25f;
		float ambient_occlusion_distance = 2.0f;
		float contact_visibility_strength = 1.0f;
		float contact_visibility_distance = 1.2f;
		uint32_t contact_visibility_samples = 4;
		float global_illumination_strength = 1.0f;
		uint32_t global_illumination_samples = 1;
		// Bounded indoor transport controls. The scene renderer forwards project settings;
		// the Metal adapter clamps them again before dispatch.
		uint32_t transport_adaptive_min_samples = 1;
		uint32_t transport_adaptive_max_samples = 4;
		float transport_adaptive_variance_reference = 0.05f;
		float diffuse_cache_cell_size = 1.5f;
		// These estimators are prototypes until their deterministic quality gates
		// pass. Their default must preserve the established direct/GI path.
		bool restir_di_reuse = false;
		bool regir_direct_reuse = false;
		bool reusable_path_reuse = false;
		bool unified_finite_light_reservoir = false;
		bool disable_reconstruction = false;
		// Validation oracle: submit only fresh primary/direct/GI/reflection
		// transport. Every history, reuse, cache, and image reconstruction path
		// must fail closed when this is set.
		bool fresh_ray_oracle = false;
		// One-bounce finite-emitter -> planar-delta-mirror -> diffuse-receiver
		// estimator. The mirror table is rebuilt from current admitted geometry;
		// it never owns temporal visibility or contributions.
		bool bidirectional_caustics = false;
		float bidirectional_caustic_delta_roughness_threshold = 0.02f;
		uint32_t bidirectional_caustic_max_mirror_triangles = 4096;
		uint32_t bidirectional_caustic_max_candidates = 1;
		uint32_t frame_index = 0;
		// Sampling-only selection. A failed backend tile upload always resolves to
		// the progressive baseline for this submission.
		RendererPathTracing::SampleSequenceMode sampling_sequence_mode = RendererPathTracing::SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY;
		uint64_t diagnostics_owner_id = 0;
		uint64_t diagnostics_frame = 0;
		float transport_max_distance = 0.0f;
		uint32_t transport_primary_geometry_count = 0;
		uint32_t transport_selected_geometry_count = 0;
		uint32_t transport_eligible_geometry_count = 0;
		uint32_t transport_selected_light_count = 0;
		uint32_t transport_eligible_light_count = 0;
		uint32_t ray_proxy_source_count = 0;
		uint32_t ray_proxy_substituted_count = 0;
		uint32_t ray_proxy_fail_open_count = 0;
		uint32_t ray_proxy_duplicate_count = 0;
		uint32_t ray_proxy_rejection_counts[RendererPathTracing::RAY_PROXY_RELATION_MAX] = {};
		RendererPathTracing::TransportCullingState transport_state = RendererPathTracing::TRANSPORT_CULLING_DISABLED;
		RendererPathTracing::TransportCullingReason transport_reason = RendererPathTracing::TRANSPORT_CULLING_REASON_DISABLED;
		bool bounded_transport = false;
		float directional_light_angular_radius = 0.0f;
		uint32_t shadow_sample_count = 1;
		bool shadow_only = false;
		bool ray_traced_shadows = true;
		bool reflections = true;
		bool ambient_occlusion = true;
		bool contact_visibility = true;
		bool global_illumination = true;
		bool collect_gpu_timings = false;
		bool history_valid = false;
		// Selected direct visibility may be reused only for alpha surfaces whose
		// geometry/material/residency revisions remain unchanged across frames.
		bool alpha_visibility_reuse_allowed = false;
		bool use_metalfx_denoiser = false;
		// Validation-only GPU telemetry carried by reconstruction flag bit 1.
		bool collect_metalfx_reactive_telemetry = false;
		// Validation-only fixed-point stage ledger. It is opt-in and has no
		// bearing on transport, reconstruction, or temporal state.
		bool collect_stage_probe = false;
		// Diagnostic-only submission sequence supplied by the scene renderer. It never
		// participates in temporal state or random sampling.
		uint64_t metalfx_diagnostic_submission_index = 0;
		bool report_metalfx_reactive_coverage = false;
		struct SolarLobe {
			Vector3 current_direction;
			Vector3 previous_direction;
			Color perpendicular_irradiance;
			float angular_radius = 0.0f;
			float cloud_transmittance = 0.0f;
			uint64_t source_id = 0;
			uint64_t sample_id = 0;
			uint64_t profile_version = 0;
			uint64_t partition_version = 0;
			uint64_t state_generation = 0;
			uint64_t history_epoch = 0;
			bool active = false;
		};
		struct Environment {
			// Sharp radiance is the source for ordinary environment transport and
			// importance sampling: residual for a valid partition, full otherwise.
			RID sharp_radiance;
			RID full_sharp_radiance;
			RID residual_radiance;
			RendererPathTracing::EnvironmentImportanceMetadata metadata;
			// This tracks the sharp radiance content actually evaluated by transport.
			// It is intentionally distinct from metadata.generation, which throttles
			// only the importance-distribution rebuild cadence.
			uint64_t radiance_content_generation = 0;
			// Authored rectangular openings form an unbiased mixture with the
			// environment distribution. The Metal adapter owns only its packed
			// representation; scene extraction remains renderer-neutral.
			Vector<RendererPathTracing::EnvironmentPortal> portals;
			uint64_t portal_generation = 0;
			SolarLobe solar_lobe;
			bool active = false;
			// False only when the public toggle requested environment transport but
			// the ownership gate rejected it. Disabled keeps legacy miss lighting.
			bool legacy_miss_fallback = true;
			// Flux raster retains deterministic primary ambient/reflections. The
			// same Sky remains available to ray secondary transport and solar.
			bool primary_replacement = false;
		} environment;
	};
	struct StageTiming {
		StringName stage;
		double milliseconds = 0.0;
		uint64_t diagnostics_owner_id = 0;
		uint64_t diagnostics_frame = 0;
	};
	struct WorkAttribution {
		uint64_t diagnostics_owner_id = 0;
		uint64_t diagnostics_frame = 0;
		uint64_t light_revision_completed = 0;
		bool trace_compaction_active = false;
		bool trace_compaction_fallback = false;
		uint32_t trace_compaction_active_pixels = 0;
		uint32_t trace_compaction_inactive_pixels = 0;
		uint32_t trace_compaction_need[5] = {};
		uint32_t trace_compaction_enqueued[5] = {};
		uint32_t trace_compaction_dispatched[5] = {};
		uint32_t alpha_candidates = 0;
		uint32_t alpha_rejections = 0;
		uint32_t alpha_candidate_exhaustions = 0;
		uint32_t alpha_mixed_intersections = 0;
		uint32_t alpha_split_opaque_queries[4] = {};
		uint32_t alpha_split_opaque_hits[4] = {};
		uint32_t alpha_split_alpha_queries[4] = {};
		uint32_t alpha_split_alpha_hits[4] = {};
		uint32_t alpha_split_alpha_rejections[4] = {};
		uint32_t alpha_split_mixed_fallbacks[4] = {};
		uint32_t alpha_rear_opaque_hits = 0;
		uint32_t alpha_primary_candidates = 0;
		uint32_t alpha_primary_rejections = 0;
		uint32_t alpha_visibility_candidates = 0;
		uint32_t alpha_visibility_rejections = 0;
		uint32_t alpha_reflection_candidates = 0;
		uint32_t alpha_reflection_rejections = 0;
		uint32_t alpha_indirect_candidates = 0;
		uint32_t alpha_indirect_rejections = 0;
		uint32_t alpha_max_candidates_per_ray = 0;
		uint32_t alpha_occupancy_empty_rejections = 0;
		uint32_t alpha_occupancy_opaque_accepts = 0;
		uint32_t alpha_occupancy_mixed_samples = 0;
		uint32_t invalid_pdf_samples = 0;
		uint32_t nonfinite_lobe_samples = 0;
		uint32_t rejected_energy_samples = 0;
		uint32_t primary_valid_pixels = 0;
		uint32_t primary_invalid_pixels = 0;
		uint32_t primary_lit_pixels = 0;
		uint32_t primary_analytic_selected = 0;
		uint32_t primary_analytic_contributed = 0;
		uint32_t primary_analytic_visibility_tests = 0;
		uint32_t reusable_path_staged = 0;
		uint32_t reusable_path_updates = 0;
		uint32_t reusable_path_queries = 0;
		uint32_t reusable_path_valid_candidates = 0;
		uint32_t reusable_path_reused_candidates = 0;
		uint32_t reusable_path_rejections = 0;
		uint32_t reusable_path_occupied = 0;
		uint32_t reusable_path_invalid_record = 0;
		uint32_t reusable_path_endpoint_blocked = 0;
		uint32_t reusable_path_shading_invalid = 0;
		uint32_t reusable_path_zero_target = 0;
		uint32_t reusable_path_invalid_weight = 0;
		uint32_t reusable_path_considered = 0;
		uint32_t reusable_path_accepted = 0;
		uint32_t reusable_path_selected = 0;
		uint32_t reusable_path_reevaluations = 0;
		uint32_t reusable_path_reconnection_visibility = 0;
		uint32_t restir_gi_current_candidates = 0;
		uint32_t restir_gi_reused_candidates = 0;
		uint32_t restir_gi_selected_reuse = 0;
		uint32_t reusable_path_lighting_reevaluations = 0;
		uint32_t reusable_path_environment_reevaluations = 0;
		uint32_t bidirectional_caustic_candidates = 0;
		uint32_t bidirectional_caustic_valid = 0;
		uint32_t bidirectional_caustic_contributed = 0;
		uint32_t bidirectional_caustic_visibility_rays = 0;
		uint32_t bidirectional_caustic_rejections = 0;
		uint32_t bidirectional_caustic_nonfinite_or_pdf_failures = 0;
		uint32_t direct_candidate_evaluations = 0;
		uint32_t direct_selected_visibility = 0;
		uint32_t direct_temporal_reuse = 0;
		uint32_t direct_spatial_reuse = 0;
		uint32_t gi_fresh_rays = 0;
		uint32_t reflection_rays = 0;
		uint32_t gi_converged_skips = 0;
		uint32_t reflection_converged_skips = 0;
		uint32_t reflection_environment_misses = 0;
		uint32_t reflection_environment_contributions = 0;
		uint32_t reflection_validation_reprojection_rejections = 0;
		uint32_t reflection_validation_surface_rejections = 0;
		uint32_t reflection_validation_motion_rejections = 0;
		uint32_t reflection_validation_refreshes = 0;
	};

	struct FrameResult {
		RendererPathTracing::HybridSceneFrameStats scene;
		bool gpu_timing_capture_submitted = false;
		// Image-space reconstruction is mutually exclusive with MetalFX temporal
		// denoising. Reservoir/cache reuse is transport work and remains active.
		bool metalfx_denoiser = false;
		bool flux_image_reconstruction = false;
		uint32_t rendered_views = 0;
		uint64_t ray_geometry_base_triangles = 0;
		uint64_t ray_geometry_selected_triangles = 0;
		uint32_t ray_lod_instance_surfaces = 0;
		uint32_t ray_lod_base_dynamic_surfaces = 0;
		uint32_t ray_lod_base_alpha_mask_surfaces = 0;
		uint32_t ray_lod_base_near_field_surfaces = 0;
		uint32_t blas_builds_deferred = 0;
		uint64_t blas_build_triangles_deferred = 0;
		uint32_t blas_compaction_queries = 0;
		uint32_t blas_compactions = 0;
		uint32_t blas_compaction_swaps = 0;
		uint32_t blas_compaction_retirements = 0;
		uint32_t ray_group_pending = 0;
		uint32_t ray_group_near = 0;
		uint32_t ray_group_middle = 0;
		uint32_t ray_group_far = 0;
		uint32_t ray_group_coarse_fallbacks = 0;
		uint32_t ray_group_off_screen_retained = 0;
		uint64_t ray_group_uncompacted_bytes = 0;
		uint64_t ray_group_compacted_bytes = 0;
		StringName ray_structure_path = "metal_primitive_as_masked_tlas";
		uint32_t tlas_rebuilds = 0;
		uint32_t tlas_refits = 0;
		uint32_t tlas_reuses = 0;
		uint32_t textured_materials = 0;
		uint32_t texture_fallbacks = 0;
		uint32_t unsupported_materials = 0;
		uint32_t unsupported_clearcoat_materials = 0;
		uint32_t unsupported_anisotropy_materials = 0;
		uint32_t unsupported_transmission_materials = 0;
		uint32_t supported_thin_transmission_materials = 0;
		uint32_t unsupported_transmission_texture_materials = 0;
		uint32_t unsupported_transmission_volume_materials = 0;
		uint32_t unsupported_refraction_materials = 0;
		bool material_texture_tier2 = false;
		bool material_diagnostics_observed = false;
		bool full_material_diagnostics_observed = false;
		uint32_t material_texture_capacity = 0;
		uint32_t material_texture_requested[static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::MAX)] = {};
		uint32_t material_texture_resident[static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::MAX)] = {};
		uint32_t material_texture_misses[static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::MAX)] = {};
		uint32_t alpha_mask_instances = 0;
		uint32_t alpha_candidates = 0;
		uint32_t alpha_rejections = 0;
		uint32_t alpha_candidate_exhaustions = 0;
		uint32_t alpha_mixed_intersections = 0;
		uint32_t alpha_rear_opaque_hits = 0;
		uint32_t alpha_primary_candidates = 0;
		uint32_t alpha_primary_rejections = 0;
		uint32_t alpha_visibility_candidates = 0;
		uint32_t alpha_visibility_rejections = 0;
		uint32_t alpha_reflection_candidates = 0;
		uint32_t alpha_reflection_rejections = 0;
		uint32_t alpha_indirect_candidates = 0;
		uint32_t alpha_indirect_rejections = 0;
		uint32_t alpha_max_candidates_per_ray = 0;
		uint32_t alpha_occupancy_empty_rejections = 0;
		uint32_t alpha_occupancy_opaque_accepts = 0;
		uint32_t alpha_occupancy_mixed_samples = 0;
		uint32_t metalfx_reactive_opaque_pixels = 0;
		uint32_t metalfx_reactive_rejected_pixels = 0;
		uint32_t invalid_pdf_samples = 0;
		uint32_t nonfinite_lobe_samples = 0;
		uint32_t rejected_energy_samples = 0;
		uint32_t primary_valid_pixels = 0;
		uint32_t primary_invalid_pixels = 0;
		uint32_t primary_lit_pixels = 0;
		uint32_t primary_analytic_selected = 0;
		uint32_t primary_analytic_contributed = 0;
		uint32_t primary_analytic_visibility_tests = 0;
		uint32_t alpha_traversal_fallbacks = 0;
		uint32_t material_generation_rejects = 0;
		double material_table_update_milliseconds = 0.0;
		uint32_t punctual_lights = 0;
		uint32_t punctual_light_overflow = 0;
		uint32_t unsupported_punctual_lights = 0;
		uint32_t world_space_diffuse_contact_visibility_views = 0;
		uint32_t raster_primary_surface_views = 0;
		uint32_t transport_history_valid_views = 0;
		uint32_t direct_reservoir_candidates = 0;
		uint32_t direct_reservoir_temporal_reuse = 0;
		uint32_t direct_reservoir_spatial_reuse = 0;
		uint32_t direct_reservoir_visibility_tests = 0;
		uint32_t emissive_triangle_count = 0;
		uint32_t emissive_triangle_capacity = 0;
		uint32_t emissive_triangle_overflow = 0;
		uint32_t diffuse_cache_hits = 0;
		uint32_t diffuse_cache_updates = 0;
		uint64_t diffuse_cache_bytes = 0;
		bool regir_enabled = false;
		bool regir_valid = false;
		bool regir_complete = false;
		uint32_t regir_cells = 0;
		uint64_t regir_bytes = 0;
		bool reusable_path_cache_enabled = false;
		bool reusable_path_cache_valid = false;
		bool reusable_path_cache_complete = false;
		uint32_t reusable_path_cache_cells = 0;
		uint32_t reusable_path_cache_occupied_cells = 0;
		uint64_t reusable_path_cache_bytes = 0;
		uint32_t reusable_path_staged = 0;
		uint32_t reusable_path_updates = 0;
		uint32_t reusable_path_queries = 0;
		uint32_t reusable_path_valid_candidates = 0;
		uint32_t reusable_path_reused_candidates = 0;
		uint32_t reusable_path_rejections = 0;
		uint32_t reusable_path_reevaluations = 0;
		uint32_t reusable_path_reconnection_visibility = 0;
		bool restir_gi_valid = false;
		bool restir_gi_complete = false;
		uint32_t restir_gi_current_candidates = 0;
		uint32_t restir_gi_reused_candidates = 0;
		uint32_t restir_gi_selected_reuse = 0;
		uint32_t reusable_path_lighting_reevaluations = 0;
		uint32_t reusable_path_environment_reevaluations = 0;
		bool bidirectional_caustic_enabled = false;
		bool bidirectional_caustic_active = false;
		bool bidirectional_caustic_complete = false;
		bool bidirectional_caustic_backend_prototype = false;
		uint32_t bidirectional_caustic_mirror_triangle_count = 0;
		uint32_t bidirectional_caustic_mirror_triangle_capacity = 0;
		uint32_t bidirectional_caustic_mirror_triangle_overflow = 0;
		uint32_t bidirectional_caustic_source_triangle_count = 0;
		uint32_t bidirectional_caustic_candidates = 0;
		uint32_t bidirectional_caustic_valid = 0;
		uint32_t bidirectional_caustic_contributed = 0;
		uint32_t bidirectional_caustic_visibility_rays = 0;
		uint32_t bidirectional_caustic_rejections = 0;
		uint32_t bidirectional_caustic_nonfinite_or_pdf_failures = 0;
		uint32_t direct_candidate_evaluations = 0;
		uint32_t direct_selected_visibility = 0;
		uint32_t gi_fresh_rays = 0;
		uint32_t reflection_rays = 0;
		uint32_t gi_converged_skips = 0;
		uint32_t reflection_converged_skips = 0;
		uint32_t reflection_environment_misses = 0;
		uint32_t reflection_environment_contributions = 0;
		uint32_t reflection_validation_reprojection_rejections = 0;
		uint32_t reflection_validation_surface_rejections = 0;
		uint32_t reflection_validation_motion_rejections = 0;
		uint32_t reflection_validation_refreshes = 0;
		bool trace_compaction_active = false;
		bool trace_compaction_fallback = false;
		uint64_t light_distribution_identity = 0;
		// Frame-local transport revision tuple. These values describe the actual
		// admitted ray scene and committed residency, never a camera cell.
		uint64_t admitted_geometry_generation = 0;
		uint64_t visibility_residency_generation = 0;
		// Completion is a readiness watermark, not part of the ray-scene
		// identity. It is intentionally reported separately so completed command
		// buffers do not reset otherwise valid transport history.
		uint64_t residency_completion_token = 0;
		bool residency_complete = false;
		uint64_t light_distribution_generation = 0;
		uint64_t environment_generation = 0;
		bool transport_revisions_valid = false;
		// Bit 0 camera/screen history, bit 1 residency readiness, bit 2 geometry,
		// bit 3 light distribution, bit 4 environment, bit 5 reconstruction reset.
		uint32_t transport_history_invalid_reasons = 0;
		double cpu_scene_preparation_milliseconds = 0.0;
		double cpu_residency_planning_milliseconds = 0.0;
		double cpu_residency_plan_cached_milliseconds = 0.0;
		double cpu_residency_plan_full_milliseconds = 0.0;
		uint64_t residency_plan_cache_hit_count = 0;
		uint64_t residency_plan_cache_miss_count = 0;
		uint64_t residency_plan_cache_rebuild_count = 0;
		double cpu_metal_preparation_milliseconds = 0.0;
		double cpu_submission_milliseconds = 0.0;
		RendererPathTracing::EnvironmentImportanceDiagnostics environment;
	};

private:
	MetalFluxEffectCache *cache = nullptr;

public:
	MetalFluxEffect();
	~MetalFluxEffect();

	// The compositor must choose Flux before it constructs an effect instance.
	// Metal exposes ray tracing capability through its native device rather than
	// the generic RenderingDevice feature flags.
	static bool is_native_ray_tracing_supported();
	bool is_supported() const;
	static Error append_streamed_cluster_surfaces(FrameRequest &r_request, const Vector<RendererPathTracing::StreamedClusterSurface> &p_surfaces, const Transform3D &p_world_transform, const Vector<Instance> &p_material_templates);
	Error render(const FrameRequest &p_request, FrameResult &r_result, String *r_error = nullptr);
	Error collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error = nullptr);
	Error collect_completed_work_attribution(Vector<WorkAttribution> &r_attribution, String *r_error = nullptr);
};

} // namespace RendererRD

#endif
