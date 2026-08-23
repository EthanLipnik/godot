/**************************************************************************/
/*  metal_hybrid_effect.h                                                 */
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
#include "servers/rendering/path_tracing/streamed_cluster_runtime.h"
#include "servers/rendering/path_tracing/transport_culling.h"
#include "servers/rendering/sky_lighting.h"

namespace RendererRD {

struct MetalHybridEffectCache;

class MetalHybridEffect {
public:
	struct Surface {
		uint64_t stable_id = 0;
		uint64_t topology_revision = 0;
		uint64_t deformation_revision = 0;
		RID vertex_buffer;
		RID index_buffer;
		RID attribute_buffer;
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
		bool has_uv = false;
		bool dynamic = false;
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
		float normal_scale = 1.0f;
		float ambient_occlusion_strength = 1.0f;
		float alpha_cutoff = 0.5f;
		float emission_texture_scale = 1.0f;
		AlphaMode alpha_mode = ALPHA_OPAQUE;
		bool orm_packed = false;
		bool emission_multiply = true;
		bool canonical_material = true;
	};

	// A small renderer-owned punctual-light contract for secondary transport.
	// It deliberately contains only the fields Full Hybrid can evaluate without
	// borrowing Forward+'s clustered-light buffers or material closures.
	struct PunctualLight {
		enum Type : uint32_t {
			TYPE_OMNI = 0,
			TYPE_SPOT = 1,
			TYPE_AREA = 2,
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
		Type type = TYPE_OMNI;
	};

	struct View {
		// Render-buffer identity supplied by Forward+.  Submitted-view order is
		// not stable across viewports, mirrors, or stereo reconfiguration.
		uint64_t history_owner_id = 0;
		uint32_t eye_index = 0;
		RID color;
		RID depth;
		RID normal_roughness;
		RID primary_material;
		RID primary_identity;
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
		uint32_t maximum_blas_builds_per_frame = 32;
		uint64_t maximum_blas_build_triangles_per_frame = 500000;
		uint32_t punctual_light_overflow = 0;
		uint32_t unsupported_punctual_lights = 0;
		uint32_t unsupported_materials = 0;
		Vector3 directional_light_direction;
		float reflection_strength = 1.0f;
		float reflection_roughness_cutoff = 0.45f;
		float ambient_occlusion_strength = 0.25f;
		float ambient_occlusion_distance = 2.0f;
		float contact_visibility_strength = 1.0f;
		float contact_visibility_distance = 1.2f;
		uint32_t contact_visibility_samples = 4;
		float global_illumination_strength = 1.0f;
		uint32_t global_illumination_samples = 1;
		// Bounded indoor transport controls. Forward+ forwards project settings;
		// the Metal adapter clamps them again before dispatch.
		uint32_t transport_adaptive_min_samples = 1;
		uint32_t transport_adaptive_max_samples = 4;
		float transport_adaptive_variance_reference = 0.05f;
		float diffuse_cache_cell_size = 1.5f;
		uint32_t frame_index = 0;
		float transport_max_distance = 0.0f;
		uint32_t transport_primary_geometry_count = 0;
		uint32_t transport_selected_geometry_count = 0;
		uint32_t transport_eligible_geometry_count = 0;
		uint32_t transport_selected_light_count = 0;
		uint32_t transport_eligible_light_count = 0;
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
		bool use_metalfx_denoiser = false;
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
		} environment;
	};
	struct StageTiming {
		StringName stage;
		double milliseconds = 0.0;
	};

	struct FrameResult {
		RendererPathTracing::HybridSceneFrameStats scene;
		uint32_t rendered_views = 0;
		uint64_t ray_geometry_base_triangles = 0;
		uint64_t ray_geometry_selected_triangles = 0;
		uint32_t ray_lod_instance_surfaces = 0;
		uint32_t blas_builds_deferred = 0;
		uint64_t blas_build_triangles_deferred = 0;
		uint32_t textured_materials = 0;
		uint32_t texture_fallbacks = 0;
		uint32_t unsupported_materials = 0;
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
		uint32_t alpha_traversal_fallbacks = 0;
		uint32_t material_generation_rejects = 0;
		double material_table_update_milliseconds = 0.0;
		uint32_t punctual_lights = 0;
		uint32_t punctual_light_overflow = 0;
		uint32_t unsupported_punctual_lights = 0;
		uint32_t world_space_diffuse_contact_visibility_views = 0;
		uint32_t raster_primary_surface_views = 0;
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
		uint64_t light_distribution_identity = 0;
		RendererPathTracing::EnvironmentImportanceDiagnostics environment;
	};

private:
	MetalHybridEffectCache *cache = nullptr;

public:
	MetalHybridEffect();
	~MetalHybridEffect();

	bool is_supported() const;
	static Error append_streamed_cluster_surfaces(FrameRequest &r_request, const Vector<RendererPathTracing::StreamedClusterSurface> &p_surfaces, const Transform3D &p_world_transform, const Vector<Instance> &p_material_templates);
	Error render(const FrameRequest &p_request, FrameResult &r_result, String *r_error = nullptr);
	Error collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error = nullptr);
};

} // namespace RendererRD

#endif
