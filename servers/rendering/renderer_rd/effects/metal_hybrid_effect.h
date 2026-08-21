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
#include "servers/rendering/path_tracing/hybrid_runtime.h"

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
		uint64_t stable_id = 0;
		uint64_t surface_id = 0;
		Transform3D transform;
		uint32_t visibility_mask = 0xff;
		Color albedo = Color(1.0, 1.0, 1.0, 1.0);
		Color emission = Color(0.0, 0.0, 0.0, 1.0);
		// The renderer-owned RD texture used by the narrow opaque albedo-texture
		// transport contract. It is deliberately not a general material binding.
		RID albedo_texture;
		float metallic = 0.0f;
		float roughness = 1.0f;
	};

	struct View {
		RID color;
		RID depth;
		RID normal_roughness;
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
		Vector<View> views;
		Vector3 directional_light_direction;
		float reflection_strength = 1.0f;
		float reflection_roughness_cutoff = 0.45f;
		float ambient_occlusion_strength = 0.25f;
		float ambient_occlusion_distance = 2.0f;
		float global_illumination_strength = 1.0f;
		uint32_t global_illumination_samples = 1;
		uint32_t frame_index = 0;
		float directional_light_angular_radius = 0.0f;
		uint32_t shadow_sample_count = 1;
		bool shadow_only = false;
		bool ray_traced_shadows = true;
		bool reflections = true;
		bool ambient_occlusion = true;
		bool global_illumination = true;
		bool collect_gpu_timings = false;
		bool history_valid = false;
		bool use_metalfx_denoiser = false;
	};
	struct StageTiming {
		StringName stage;
		double milliseconds = 0.0;
	};

	struct FrameResult {
		RendererPathTracing::HybridSceneFrameStats scene;
		uint32_t rendered_views = 0;
		uint32_t textured_materials = 0;
		uint32_t texture_fallbacks = 0;
	};

private:
	MetalHybridEffectCache *cache = nullptr;

public:
	MetalHybridEffect();
	~MetalHybridEffect();

	bool is_supported() const;
	Error render(const FrameRequest &p_request, FrameResult &r_result, String *r_error = nullptr);
	Error collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error = nullptr);
};

} // namespace RendererRD

#endif
