/**************************************************************************/
/*  render_flux.cpp                                          */
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

#include "render_flux.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "servers/rendering/path_tracing/ray_geometry_admission_policy.h"
#include "servers/rendering/path_tracing/ray_geometry_lod_policy.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/storage/ltc_lut.gen.h"

using namespace RendererSceneRenderImplementation;

#define PRELOAD_PIPELINES_ON_SURFACE_CACHE_CONSTRUCTION 1

#define FADE_ALPHA_PASS_THRESHOLD 0.999

#ifdef METAL_MFXTEMPORAL_ENABLED
static RendererPathTracing::Matrix4 _mfx_matrix_from_projection(const Projection &p_projection) {
	RendererPathTracing::Matrix4 result = {};
	for (uint32_t column = 0; column < 4; column++) {
		result.columns[column] = { float(p_projection.columns[column].x), float(p_projection.columns[column].y), float(p_projection.columns[column].z), float(p_projection.columns[column].w) };
	}
	return result;
}

static RendererPathTracing::Matrix4 _mfx_matrix_from_transform(const Transform3D &p_transform) {
	RendererPathTracing::Matrix4 result = {};
	const Vector3 x = p_transform.basis.get_column(0);
	const Vector3 y = p_transform.basis.get_column(1);
	const Vector3 z = p_transform.basis.get_column(2);
	result.columns[0] = { x.x, x.y, x.z, 0.0f };
	result.columns[1] = { y.x, y.y, y.z, 0.0f };
	result.columns[2] = { z.x, z.y, z.z, 0.0f };
	result.columns[3] = { p_transform.origin.x, p_transform.origin.y, p_transform.origin.z, 1.0f };
	return result;
}
#endif

#ifdef METAL_ENABLED
// MeshStorage keeps UV attributes in a separate packed buffer. Keep this
// layout calculation in lockstep with MeshStorage::_mesh_surface_generate_vertex_format()
// so the Metal hybrid path can interpolate UV0 without assuming a scene-level
// mesh representation.
static void _metal_flux_get_uv0_layout(uint64_t p_format, uint32_t &r_attribute_stride, uint32_t &r_uv_offset, bool &r_has_uv) {
	r_attribute_stride = 0;
	r_uv_offset = 0;
	r_has_uv = false;
	const bool compressed = p_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	const uint32_t custom_sizes[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
	const uint32_t custom_shifts[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
	for (int attribute = RSE::ARRAY_COLOR; attribute < RSE::ARRAY_BONES; attribute++) {
		if (!(p_format & (1ULL << attribute))) {
			continue;
		}
		switch (attribute) {
			case RSE::ARRAY_COLOR:
				r_attribute_stride += sizeof(uint32_t);
				break;
			case RSE::ARRAY_TEX_UV:
				r_uv_offset = r_attribute_stride;
				r_has_uv = true;
				r_attribute_stride += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
				break;
			case RSE::ARRAY_TEX_UV2:
				r_attribute_stride += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
				break;
			case RSE::ARRAY_CUSTOM0:
			case RSE::ARRAY_CUSTOM1:
			case RSE::ARRAY_CUSTOM2:
			case RSE::ARRAY_CUSTOM3: {
				const int custom_index = attribute - RSE::ARRAY_CUSTOM0;
				const uint32_t custom_format = (p_format >> custom_shifts[custom_index]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
				r_attribute_stride += custom_sizes[custom_format];
			} break;
			default:
				break;
		}
	}
}

struct _MetalHybridSurfaceAdmissionLess {
	bool operator()(const RendererRD::MetalFluxEffect::Surface &p_a, const RendererRD::MetalFluxEffect::Surface &p_b) const {
		if (p_a.persistent_coarse != p_b.persistent_coarse) return p_a.persistent_coarse;
		if (p_a.ray_group_id != 0 && p_b.ray_group_id != 0 && p_a.ray_tier != p_b.ray_tier) return p_a.ray_tier < p_b.ray_tier;
		RendererPathTracing::RayGeometryAdmissionPriority a;
		a.stable_id = p_a.stable_id;
		a.camera_distance = p_a.admission_camera_distance;
		a.dynamic = p_a.dynamic;
		a.emissive = p_a.admission_emissive;
		RendererPathTracing::RayGeometryAdmissionPriority b;
		b.stable_id = p_b.stable_id;
		b.camera_distance = p_b.admission_camera_distance;
		b.dynamic = p_b.dynamic;
		b.emissive = p_b.admission_emissive;
		return RendererPathTracing::ray_geometry_admission_precedes(a, b);
	}
};

struct _MetalHybridInstanceStableIdLess {
	bool operator()(const RendererRD::MetalFluxEffect::Instance &p_a, const RendererRD::MetalFluxEffect::Instance &p_b) const {
		return p_a.stable_id < p_b.stable_id;
	}
};

struct _MetalHybridShaderClassification {
	bool canonical_standard_material = false;
	bool alpha_scissor_used = false;
	bool alpha_hash_used = false;
	bool alpha_antialiasing_used = false;
	bool emission_multiply = true;
	bool clearcoat_used = false;
	bool anisotropy_used = false;
	bool transmission_used = false;
	bool refraction_used = false;
	Color roughness_texture_channel = Color(1.0, 0.0, 0.0, 0.0);
};

static _FORCE_INLINE_ void _flux_bounded_feature_count(uint32_t &r_count) {
	// Diagnostics are intentionally bounded so a pathological imported scene
	// cannot turn unsupported-feature reporting into an unbounded counter.
	if (r_count < 1000000u) {
		r_count++;
	}
}
#endif

void RenderFlux::RenderBufferDataFlux::ensure_specular() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_SPECULAR)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_SPECULAR, get_specular_format(), get_specular_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_SPECULAR_MSAA, get_specular_format(), get_specular_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderFlux::RenderBufferDataFlux::ensure_normal_roughness_texture() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_NORMAL_ROUGHNESS)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_NORMAL_ROUGHNESS, get_normal_roughness_format(), get_normal_roughness_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_NORMAL_ROUGHNESS_MSAA, get_normal_roughness_format(), get_normal_roughness_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderFlux::RenderBufferDataFlux::ensure_primary_surface_v1() {
	ERR_FAIL_NULL(render_buffers);
	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_MATERIAL)) {
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_MATERIAL, get_primary_surface_material_format(), get_primary_surface_usage_bits(render_buffers->get_can_be_storage()), RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
	}
	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_IDENTITY)) {
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_IDENTITY, get_primary_surface_identity_format(), get_primary_surface_usage_bits(render_buffers->get_can_be_storage()), RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
	}
	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_GEOMETRY)) {
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_GEOMETRY, get_primary_surface_geometry_format(), get_primary_surface_usage_bits(render_buffers->get_can_be_storage()), RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
	}
	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_FLAGS)) {
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_FLAGS, get_primary_surface_flags_format(), get_primary_surface_usage_bits(render_buffers->get_can_be_storage()), RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
	}
}

void RenderFlux::RenderBufferDataFlux::ensure_voxelgi() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_VOXEL_GI)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_VOXEL_GI, get_voxelgi_format(), get_voxelgi_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_VOXEL_GI_MSAA, get_voxelgi_format(), get_voxelgi_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderFlux::RenderBufferDataFlux::ensure_hybrid_effect() {
	ERR_FAIL_NULL(render_buffers);
	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_EFFECT)) {
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_EFFECT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
	}
	if (!render_buffers->has_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_FILTERED)) {
		render_buffers->create_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_FILTERED, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
	}
	const StringName history_names[] = { RB_TEX_HYBRID_HISTORY_A, RB_TEX_HYBRID_HISTORY_B };
	for (const StringName &name : history_names) {
		if (!render_buffers->has_texture(RB_SCOPE_FLUX, name)) {
			render_buffers->create_texture(RB_SCOPE_FLUX, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
		}
	}
	const StringName depth_history_names[] = { RB_TEX_HYBRID_DEPTH_HISTORY_A, RB_TEX_HYBRID_DEPTH_HISTORY_B };
	for (const StringName &name : depth_history_names) {
		if (!render_buffers->has_texture(RB_SCOPE_FLUX, name)) {
			render_buffers->create_texture(RB_SCOPE_FLUX, name, RD::DATA_FORMAT_R32_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
		}
	}
	const StringName normal_history_names[] = { RB_TEX_HYBRID_NORMAL_HISTORY_A, RB_TEX_HYBRID_NORMAL_HISTORY_B };
	for (const StringName &name : normal_history_names) {
		if (!render_buffers->has_texture(RB_SCOPE_FLUX, name)) {
			render_buffers->create_texture(RB_SCOPE_FLUX, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
		}
	}
	struct HybridGuideTexture {
		StringName name;
		RD::DataFormat format;
	};
	const HybridGuideTexture guide_textures[] = {
		{ RB_TEX_HYBRID_GUIDE_NORMAL, RD::DATA_FORMAT_R16G16B16A16_SFLOAT },
		{ RB_TEX_HYBRID_GUIDE_DIFFUSE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT },
		{ RB_TEX_HYBRID_GUIDE_SPECULAR, RD::DATA_FORMAT_R16G16B16A16_SFLOAT },
		{ RB_TEX_HYBRID_GUIDE_ROUGHNESS, RD::DATA_FORMAT_R16_SFLOAT },
		{ RB_TEX_HYBRID_GUIDE_DENOISE_STRENGTH, RD::DATA_FORMAT_R8_UNORM },
		{ RB_TEX_HYBRID_GUIDE_REACTIVE, RD::DATA_FORMAT_R8_UNORM },
		{ RB_TEX_HYBRID_GUIDE_SPECULAR_DISTANCE, RD::DATA_FORMAT_R16_SFLOAT },
		{ RB_TEX_HYBRID_GUIDE_TRANSPARENCY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT },
	};
	for (const HybridGuideTexture &guide : guide_textures) {
		if (!render_buffers->has_texture(RB_SCOPE_FLUX, guide.name)) {
			const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
					(guide.name == RB_TEX_HYBRID_GUIDE_TRANSPARENCY ? RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
			render_buffers->create_texture(RB_SCOPE_FLUX, guide.name, guide.format,
					usage, RD::TEXTURE_SAMPLES_1, render_buffers->get_internal_size(), render_buffers->get_view_count());
		}
	}
}

RID RenderFlux::RenderBufferDataFlux::get_hybrid_transparency_fb() {
	ERR_FAIL_NULL_V(render_buffers, RID());
	const RID transparency = render_buffers->get_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_GUIDE_TRANSPARENCY);
	const RID depth = render_buffers->get_depth_texture();
	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), transparency, depth);
}

void RenderFlux::RenderBufferDataFlux::ensure_fsr2(RendererRD::FSR2Effect *p_effect) {
	if (fsr2_context == nullptr) {
		fsr2_context = p_effect->create_context(render_buffers->get_internal_size(), render_buffers->get_target_size());
	}
}

#ifdef METAL_MFXTEMPORAL_ENABLED
bool RenderFlux::RenderBufferDataFlux::ensure_mfx_temporal(RendererRD::MFXTemporalEffect *p_effect) {
	if (mfx_temporal_context == nullptr) {
		RendererRD::MFXTemporalEffect::CreateParams params;
		params.input_size = render_buffers->get_internal_size();
		params.output_size = render_buffers->get_target_size();
		params.input_format = render_buffers->get_base_data_format();
		params.depth_format = render_buffers->get_depth_format(false, false, render_buffers->get_can_be_storage());
		params.motion_format = render_buffers->get_velocity_format();
		params.reactive_format = render_buffers->get_base_data_format(); // Reactive is derived from input.
		params.output_format = render_buffers->get_base_data_format();
		params.motion_vector_scale = render_buffers->get_internal_size();
		mfx_temporal_context = p_effect->create_context(params);
		return true;
	}
	return false;
}

bool RenderFlux::RenderBufferDataFlux::ensure_mfx_denoised(RendererRD::MFXDenoisedEffect *p_effect, String *r_error) {
	if (!p_effect || !p_effect->is_supported()) {
		if (r_error) {
			*r_error = "The active Metal device does not support temporal denoised MetalFX scaling.";
		}
		return false;
	}
	// MetalFX contexts capture the guide dimensions and formats at creation.
	// Reusing one after a render-buffer resize or format change feeds stale
	// history into the new viewport. Keep the identity in the render-buffer
	// owner so the reset is coupled to the actual context replacement.
	auto mix_key = [](uint64_t p_hash, uint64_t p_value) {
		return p_hash ^ (p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2));
	};
	uint64_t context_key = 0x4d465844454e4f49ULL;
	context_key = mix_key(context_key, uint64_t(render_buffers->get_internal_size().x));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_internal_size().y));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_target_size().x));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_target_size().y));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_base_data_format()));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_depth_format(false, false, render_buffers->get_can_be_storage())));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_velocity_format()));
	context_key = mix_key(context_key, uint64_t(render_buffers->get_view_count()));
	if (!mfx_denoised_contexts.is_empty() && mfx_denoised_context_key == context_key) {
		return true;
	}
	if (!mfx_denoised_contexts.is_empty()) {
		for (RendererRD::MFXDenoisedContext *context : mfx_denoised_contexts) {
			memdelete(context);
		}
		mfx_denoised_contexts.clear();
		hybrid_mfx_denoised_reset = true;
	}
	RendererRD::MFXDenoisedEffect::CreateParams params;
	params.input_size = render_buffers->get_internal_size();
	params.output_size = render_buffers->get_target_size();
	params.color_format = render_buffers->get_base_data_format();
	params.depth_format = render_buffers->get_depth_format(false, false, render_buffers->get_can_be_storage());
	params.motion_format = render_buffers->get_velocity_format();
	params.normal_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	params.diffuse_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	params.specular_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	params.roughness_format = RD::DATA_FORMAT_R16_SFLOAT;
	params.denoise_strength_format = RD::DATA_FORMAT_R8_UNORM;
	params.reactive_format = RD::DATA_FORMAT_R8_UNORM;
	params.specular_distance_format = RD::DATA_FORMAT_R16_SFLOAT;
	params.transparency_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	params.output_format = render_buffers->get_base_data_format();
	for (uint32_t view = 0; view < render_buffers->get_view_count(); view++) {
		RendererRD::MFXDenoisedContext *context = p_effect->create_context(params, r_error);
		if (!context) {
			for (RendererRD::MFXDenoisedContext *created : mfx_denoised_contexts) {
				memdelete(created);
			}
			mfx_denoised_contexts.clear();
			mfx_denoised_context_key = 0;
			return false;
		}
		mfx_denoised_contexts.push_back(context);
	}
	mfx_denoised_context_key = context_key;
	if (r_error) {
		r_error->clear();
	}
	return true;
}
#endif

void RenderFlux::RenderBufferDataFlux::free_data() {
	hybrid_history_valid = false;
	hybrid_history_index = 0;
	hybrid_mfx_denoised_active = false;
	hybrid_mfx_denoised_reset = true;
	hybrid_environment_history_key = 0;
	hybrid_scene_history_key = 0;
	mfx_denoised_context_key = 0;
	hybrid_renderer_enabled = true;
	// JIC, should already have been cleared
	if (render_buffers) {
		render_buffers->clear_context(RB_SCOPE_FLUX);
		render_buffers->clear_context(RB_SCOPE_SSDS);
		render_buffers->clear_context(RB_SCOPE_SSIL);
		render_buffers->clear_context(RB_SCOPE_SSAO);
		render_buffers->clear_context(RB_SCOPE_SSR);
	}

	if (cluster_builder) {
		memdelete(cluster_builder);
		cluster_builder = nullptr;
	}

	if (fsr2_context) {
		memdelete(fsr2_context);
		fsr2_context = nullptr;
	}

#ifdef METAL_MFXTEMPORAL_ENABLED
	if (mfx_temporal_context) {
		memdelete(mfx_temporal_context);
		mfx_temporal_context = nullptr;
	}
	for (RendererRD::MFXDenoisedContext *context : mfx_denoised_contexts) {
		memdelete(context);
	}
	mfx_denoised_contexts.clear();
#endif

	if (!render_sdfgi_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_sdfgi_uniform_set)) {
		RD::get_singleton()->free_rid(render_sdfgi_uniform_set);
	}
}

void RenderFlux::RenderBufferDataFlux::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (render_buffers) {
		// JIC
		free_data();
	}

	render_buffers = p_render_buffers;
	ERR_FAIL_NULL(render_buffers);

	if (cluster_builder == nullptr) {
		cluster_builder = memnew(ClusterBuilderRD);
	}
	cluster_builder->set_shared(RenderFlux::get_singleton()->get_cluster_builder_shared());

	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	// Note: Color and depth buffer is only used for our debug mode, we don't support stereo rendering for this.
	cluster_builder->setup(p_render_buffers->get_internal_size(), p_render_buffers->get_max_cluster_elements(), p_render_buffers->get_depth_texture(0), sampler, p_render_buffers->get_internal_texture(0));
}

RID RenderFlux::RenderBufferDataFlux::get_color_only_fb() {
	ERR_FAIL_NULL_V(render_buffers, RID());

	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID color = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_MSAA) : render_buffers->get_internal_texture();
	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	if (render_buffers->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
		RID vrs_texture = render_buffers->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), color, depth, vrs_texture);
	} else {
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), color, depth);
	}
}

RID RenderFlux::RenderBufferDataFlux::get_color_pass_fb(uint32_t p_color_pass_flags) {
	ERR_FAIL_NULL_V(render_buffers, RID());
	// PrimarySurfaceV1 is a single-sample ABI consumed directly by Metal. Keep
	// its color/depth pair single-sample even when the raster preview requests
	// MSAA; stochastic reconstruction owns ray-on anti-aliasing.
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED && (p_color_pass_flags & COLOR_PASS_FLAG_PRIMARY_SURFACE) == 0;

	int v_count = (p_color_pass_flags & COLOR_PASS_FLAG_MULTIVIEW) ? render_buffers->get_view_count() : 1;
	RID color = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_MSAA) : render_buffers->get_internal_texture();

	RID specular;
	if (p_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) {
		ensure_specular();
		specular = render_buffers->get_texture(RB_SCOPE_FLUX, use_msaa ? RB_TEX_SPECULAR_MSAA : RB_TEX_SPECULAR);
	}

	RID velocity_buffer;
	if (p_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) {
		render_buffers->ensure_velocity();
		velocity_buffer = render_buffers->get_velocity_buffer(use_msaa);
	}

	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	if (render_buffers->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
		RID vrs_texture = render_buffers->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(v_count, color, specular, velocity_buffer, depth, vrs_texture);
	} else {
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(v_count, color, specular, velocity_buffer, depth);
	}
}

RID RenderFlux::RenderBufferDataFlux::get_depth_fb(DepthFrameBufferType p_type) {
	ERR_FAIL_NULL_V(render_buffers, RID());
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	switch (p_type) {
		case DEPTH_FB: {
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth);
		} break;
		case DEPTH_FB_ROUGHNESS: {
			ensure_normal_roughness_texture();

			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);

			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer);
		} break;
		case DEPTH_FB_ROUGHNESS_VOXELGI: {
			ensure_normal_roughness_texture();
			ensure_voxelgi();

			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);
			RID voxelgi_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, use_msaa ? RB_TEX_VOXEL_GI_MSAA : RB_TEX_VOXEL_GI);

			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer, voxelgi_buffer);
		} break;
		case DEPTH_FB_ROUGHNESS_HYBRID_MATERIAL: {
			ensure_normal_roughness_texture();
			ensure_primary_surface_v1();
			depth = render_buffers->get_depth_texture();
			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, RB_TEX_NORMAL_ROUGHNESS);
			RID primary_material_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_MATERIAL);
			RID primary_identity_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_IDENTITY);
			RID primary_geometry_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_GEOMETRY);
			RID primary_flags_buffer = render_buffers->get_texture(RB_SCOPE_FLUX, RB_TEX_PRIMARY_SURFACE_FLAGS);
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer, primary_material_buffer, primary_identity_buffer, primary_geometry_buffer, primary_flags_buffer);
		} break;
		default: {
			ERR_FAIL_V(RID());
		} break;
	}
}

RID RenderFlux::RenderBufferDataFlux::get_specular_only_fb() {
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID specular = render_buffers->get_texture(RB_SCOPE_FLUX, use_msaa ? RB_TEX_SPECULAR_MSAA : RB_TEX_SPECULAR);

	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), specular);
}

RID RenderFlux::RenderBufferDataFlux::get_velocity_only_fb() {
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID velocity = render_buffers->get_texture(RB_SCOPE_BUFFERS, use_msaa ? RB_TEX_VELOCITY_MSAA : RB_TEX_VELOCITY);

	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), velocity);
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_specular_format() {
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

uint32_t RenderFlux::RenderBufferDataFlux::get_specular_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_normal_roughness_format() {
	return RD::DATA_FORMAT_R8G8B8A8_UNORM;
}

uint32_t RenderFlux::RenderBufferDataFlux::get_normal_roughness_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_primary_surface_material_format() {
	return RD::DATA_FORMAT_R8G8B8A8_UNORM;
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_primary_surface_identity_format() {
	return RD::DATA_FORMAT_R16G16B16A16_UINT;
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_primary_surface_geometry_format() {
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_primary_surface_flags_format() {
	return RD::DATA_FORMAT_R32_UINT;
}

uint32_t RenderFlux::RenderBufferDataFlux::get_primary_surface_usage_bits(bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(false, false, p_storage);
}

RD::DataFormat RenderFlux::RenderBufferDataFlux::get_voxelgi_format() {
	return RD::DATA_FORMAT_R8G8_UINT;
}

uint32_t RenderFlux::RenderBufferDataFlux::get_voxelgi_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

void RenderFlux::setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataFlux> data;
	data.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_FLUX, data);

	Ref<RendererRD::GI::RenderBuffersGI> rbgi;
	rbgi.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_GI, rbgi);
}

bool RenderFlux::free(RID p_rid) {
	if (RendererSceneRenderRD::free(p_rid)) {
		return true;
	}
	return false;
}

void RenderFlux::update() {
	RendererSceneRenderRD::update();
	_update_global_pipeline_data_requirements_from_project();
	_update_global_pipeline_data_requirements_from_light_storage();
}

/// RENDERING ///

template <RenderFlux::PassMode p_pass_mode, uint32_t p_color_pass_flags>
void RenderFlux::_render_list_template(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	RD::DrawListID draw_list = p_draw_list;
	RD::FramebufferFormatID framebuffer_format = p_framebuffer_Format;

	//global scope bindings
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, render_base_uniform_set, SCENE_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, p_params->render_pass_uniform_set, RENDER_PASS_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);

	RID prev_material_uniform_set;

	RID prev_vertex_array_rd;
	RID prev_index_array_rd;
	RID prev_xforms_uniform_set;

	SceneShaderFlux::ShaderData *shader = nullptr;
	SceneShaderFlux::ShaderData *prev_shader = nullptr;
	SceneShaderFlux::ShaderData::PipelineKey pipeline_key;
	uint32_t pipeline_hash = 0;
	uint32_t prev_pipeline_hash = 0;

	bool shadow_pass = (p_pass_mode == PASS_MODE_SHADOW) || (p_pass_mode == PASS_MODE_SHADOW_DP);

	SceneState::PushConstant push_constant;

	if constexpr (p_pass_mode == PASS_MODE_DEPTH_MATERIAL) {
		push_constant.uv_offset = Math::make_half_float(p_params->uv_offset.y) << 16;
		push_constant.uv_offset |= Math::make_half_float(p_params->uv_offset.x);
	} else {
		push_constant.uv_offset = 0;
	}

	bool should_request_redraw = false;

	for (uint32_t i = p_from_element; i < p_to_element; i++) {
		const GeometryInstanceSurfaceDataCache *surf = p_params->elements[i];
		const RenderElementInfo &element_info = p_params->element_info[i];

		if (p_pass_mode == PASS_MODE_COLOR && surf->color_pass_inclusion_mask && (p_color_pass_flags & surf->color_pass_inclusion_mask) == 0) {
			// Some surfaces can be repeated in multiple render lists. We exclude them from being rendered on the color pass based on the
			// features supported by the pass compared to the exclusion mask.
			continue;
		}

		if (surf->owner->instance_count == 0) {
			continue;
		}

		push_constant.base_index = i + p_params->element_offset;

		RID material_uniform_set;
		void *mesh_surface;

		if (shadow_pass || p_pass_mode == PASS_MODE_DEPTH) { //regular depth pass can use these too
			material_uniform_set = surf->material_uniform_set_shadow;
			shader = surf->shader_shadow;
			mesh_surface = surf->surface_shadow;

		} else {
#ifdef DEBUG_ENABLED
			if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LIGHTING)) {
				material_uniform_set = scene_shader.default_material_uniform_set;
				shader = scene_shader.default_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW)) {
				material_uniform_set = scene_shader.overdraw_material_uniform_set;
				shader = scene_shader.overdraw_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS)) {
				material_uniform_set = scene_shader.debug_shadow_splits_material_uniform_set;
				shader = scene_shader.debug_shadow_splits_material_shader_ptr;
			} else {
#endif
				material_uniform_set = surf->material_uniform_set;
				shader = surf->shader;
				surf->material->set_as_used();
#ifdef DEBUG_ENABLED
			}
#endif
			mesh_surface = surf->surface;
		}

		if (!mesh_surface) {
			continue;
		}

		//request a redraw if one of the shaders uses TIME
		if (shader->uses_time) {
			should_request_redraw = true;
		}

		// Determine the cull variant.
		SceneShaderFlux::ShaderData::CullVariant cull_variant = SceneShaderFlux::ShaderData::CULL_VARIANT_MAX;
		if constexpr (p_pass_mode == PASS_MODE_DEPTH_MATERIAL || p_pass_mode == PASS_MODE_SDF) {
			cull_variant = SceneShaderFlux::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
		} else {
			if constexpr (p_pass_mode == PASS_MODE_SHADOW || p_pass_mode == PASS_MODE_SHADOW_DP) {
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) {
					cull_variant = SceneShaderFlux::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
				}
			}

			if (cull_variant == SceneShaderFlux::ShaderData::CULL_VARIANT_MAX) {
				bool mirror = surf->owner->mirror;
				if (p_params->reverse_cull) {
					mirror = !mirror;
				}

				cull_variant = mirror ? SceneShaderFlux::ShaderData::CULL_VARIANT_REVERSED : SceneShaderFlux::ShaderData::CULL_VARIANT_NORMAL;
			}
		}

		pipeline_key.primitive_type = surf->primitive;

		RID xforms_uniform_set = surf->owner->transforms_uniform_set;

		SceneShaderFlux::ShaderSpecialization pipeline_specialization = p_params->base_specialization;
		pipeline_specialization.multimesh = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH);
		pipeline_specialization.multimesh_format_2d = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D);
		pipeline_specialization.multimesh_has_color = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR);
		pipeline_specialization.multimesh_has_custom_data = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA);

		if constexpr (p_pass_mode == PASS_MODE_COLOR) {
			pipeline_specialization.use_light_soft_shadows = element_info.uses_softshadow;
			pipeline_specialization.use_light_projector = element_info.uses_projector;
			pipeline_specialization.use_directional_soft_shadows = p_params->use_directional_soft_shadow;
		}

		pipeline_key.color_pass_flags = 0;

		switch (p_pass_mode) {
			case PASS_MODE_COLOR: {
				if (element_info.uses_lightmap) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_LIGHTMAP;
				} else {
					pipeline_specialization.use_forward_gi = element_info.uses_forward_gi;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_TRANSPARENT) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_TRANSPARENT;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_MULTIVIEW) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MULTIVIEW;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_PRIMARY_SURFACE) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_PRIMARY_SURFACE;
				}

				pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_COLOR_PASS;
			} break;
			case PASS_MODE_SHADOW:
			case PASS_MODE_DEPTH: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS;
			} break;
			case PASS_MODE_SHADOW_DP: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for shadow DP pass");
				pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_DP;
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_HYBRID_MATERIAL_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_HYBRID_MATERIAL;
			} break;
			case PASS_MODE_DEPTH_MATERIAL: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for material pass");
				pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL;
			} break;
			case PASS_MODE_SDF: {
				// Note, SDF is prepared in world space, this shouldn't be a multiview buffer even when stereoscopic rendering is used.
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for SDF pass");
				pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF;
			} break;
		}

		pipeline_key.framebuffer_format_id = framebuffer_format;
		pipeline_key.wireframe = p_params->force_wireframe;
		pipeline_key.ubershader = 0;

		bool emulate_point_size = shader->uses_point_size && scene_shader.emulate_point_size;

		const RD::PolygonCullMode cull_mode = shader->get_cull_mode_from_cull_variant(cull_variant);
		RID vertex_array_rd;
		RID index_array_rd;
		RID pipeline_rd;
		const uint32_t ubershader_iterations = 2;
		bool pipeline_valid = false;
		while (pipeline_key.ubershader < ubershader_iterations) {
			// Skeleton and blend shape.
			RD::VertexFormatID vertex_format = -1;
			bool pipeline_motion_vectors = pipeline_key.color_pass_flags & SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
			uint64_t input_mask = shader->get_vertex_input_mask(pipeline_key.version, pipeline_key.color_pass_flags, pipeline_key.ubershader);
			if (surf->owner->mesh_instance.is_valid()) {
				mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(surf->owner->mesh_instance, surf->surface_index, input_mask, pipeline_motion_vectors, emulate_point_size, vertex_array_rd, vertex_format);
			} else {
				mesh_storage->mesh_surface_get_vertex_arrays_and_format(mesh_surface, input_mask, pipeline_motion_vectors, emulate_point_size, vertex_array_rd, vertex_format);
			}

			pipeline_key.vertex_format_id = vertex_format;

			if (pipeline_key.ubershader) {
				pipeline_key.shader_specialization = {};
				pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
			} else {
				pipeline_key.shader_specialization = pipeline_specialization;
				pipeline_key.cull_mode = cull_mode;
			}

			pipeline_hash = pipeline_key.hash();

			if (shader != prev_shader || pipeline_hash != prev_pipeline_hash) {
				RSE::PipelineSource pipeline_source = pipeline_key.ubershader ? RSE::PIPELINE_SOURCE_DRAW : RSE::PIPELINE_SOURCE_SPECIALIZATION;
				pipeline_rd = shader->pipeline_hash_map.get_pipeline(pipeline_key, pipeline_hash, pipeline_key.ubershader, pipeline_source);

				if (pipeline_rd.is_valid()) {
					pipeline_valid = true;
					prev_shader = shader;
					prev_pipeline_hash = pipeline_hash;
					break;
				} else {
					pipeline_key.ubershader++;
				}
			} else {
				// The same pipeline is bound already.
				pipeline_valid = true;
				break;
			}
		}

		if (pipeline_valid) {
			if (!emulate_point_size) {
				index_array_rd = mesh_storage->mesh_surface_get_index_array(mesh_surface, element_info.lod_index);
			} else {
				index_array_rd = RID();
			}

			if (prev_vertex_array_rd != vertex_array_rd) {
				RD::get_singleton()->draw_list_bind_vertex_array(draw_list, vertex_array_rd);
				prev_vertex_array_rd = vertex_array_rd;
			}

			if (prev_index_array_rd != index_array_rd) {
				if (index_array_rd.is_valid()) {
					RD::get_singleton()->draw_list_bind_index_array(draw_list, index_array_rd);
				}
				prev_index_array_rd = index_array_rd;
			}

			if (!pipeline_rd.is_null()) {
				RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline_rd);
			}

			if (xforms_uniform_set.is_valid() && prev_xforms_uniform_set != xforms_uniform_set) {
				RD::get_singleton()->draw_list_bind_uniform_set(draw_list, xforms_uniform_set, TRANSFORMS_UNIFORM_SET);
				prev_xforms_uniform_set = xforms_uniform_set;
			}

			if (material_uniform_set != prev_material_uniform_set) {
				// Update uniform set.
				if (material_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(material_uniform_set)) { // Material may not have a uniform set.
					RD::get_singleton()->draw_list_bind_uniform_set(draw_list, material_uniform_set, MATERIAL_UNIFORM_SET);
				}

				prev_material_uniform_set = material_uniform_set;
			}

			if (surf->owner->base_flags & INSTANCE_DATA_FLAG_PARTICLES) {
				particles_storage->particles_get_instance_buffer_motion_vectors_offsets(surf->owner->data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
			} else if (surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) {
				mesh_storage->_multimesh_get_motion_vectors_offsets(surf->owner->data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
			} else {
				push_constant.multimesh_motion_vectors_current_offset = 0;
				push_constant.multimesh_motion_vectors_previous_offset = 0;
			}

			size_t push_constant_size = 0;
			if (pipeline_key.ubershader) {
				push_constant_size = sizeof(SceneState::PushConstant);
				push_constant.ubershader.specialization = pipeline_specialization;
				push_constant.ubershader.constants = {};
				push_constant.ubershader.constants.cull_mode = cull_mode;
			} else {
				push_constant_size = sizeof(SceneState::PushConstant) - sizeof(SceneState::PushConstantUbershader);
			}

			RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, push_constant_size);

			uint32_t instance_count = surf->owner->instance_count > 1 ? surf->owner->instance_count : element_info.repeat;
			if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS) {
				instance_count /= surf->owner->trail_steps;
			}

			bool indirect = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT);

			if (emulate_point_size) {
				if (indirect) {
					WARN_PRINT("Indirect draws are not supported when emulating point size.");
				}
				RD::get_singleton()->draw_list_draw(draw_list, false, mesh_storage->mesh_surface_get_vertex_count(mesh_surface), instance_count * 6);
			} else if (indirect) {
				RD::get_singleton()->draw_list_draw_indirect(draw_list, index_array_rd.is_valid(), mesh_storage->_multimesh_get_command_buffer_rd_rid(surf->owner->data->base), surf->surface_index * sizeof(uint32_t) * mesh_storage->INDIRECT_MULTIMESH_COMMAND_STRIDE, 1, 0);
			} else {
				RD::get_singleton()->draw_list_draw(draw_list, index_array_rd.is_valid(), instance_count);
			}
		}

		i += element_info.repeat - 1; //skip equal elements
	}

	// Make the actual redraw request
	if (should_request_redraw) {
		RenderingServerDefault::redraw_request();
	}
}

void RenderFlux::_render_list(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	//use template for faster performance (pass mode comparisons are inlined)

	switch (p_params->pass_mode) {
#define VALID_FLAG_COMBINATION(f) \
	case f: { \
		_render_list_template<PASS_MODE_COLOR, f>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element); \
	} break;

		case PASS_MODE_COLOR: {
			switch (p_params->color_pass_flags) {
				VALID_FLAG_COMBINATION(0);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_PRIMARY_SURFACE);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_PRIMARY_SURFACE | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_PRIMARY_SURFACE | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_PRIMARY_SURFACE | COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				default: {
					ERR_FAIL_MSG("Invalid color pass flag combination " + itos(p_params->color_pass_flags));
				}
			}

		} break;
		case PASS_MODE_SHADOW: {
			_render_list_template<PASS_MODE_SHADOW>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SHADOW_DP: {
			_render_list_template<PASS_MODE_SHADOW_DP>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH: {
			_render_list_template<PASS_MODE_DEPTH>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_MATERIAL: {
			_render_list_template<PASS_MODE_DEPTH_MATERIAL>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SDF: {
			_render_list_template<PASS_MODE_SDF>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		default: {
			// Unknown pass mode.
		} break;
	}
}

void RenderFlux::_render_list_with_draw_list(RenderListParameters *p_params, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_color_values, float p_clear_depth_value, uint32_t p_clear_stencil_value, const Rect2 &p_region) {
	RD::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_framebuffer);
	p_params->framebuffer_format = fb_format;
	_prepare_virtual_geometry(fb_format, p_params);

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, p_draw_flags, p_clear_color_values, p_clear_depth_value, p_clear_stencil_value, p_region);
	_render_list(draw_list, fb_format, p_params, 0, p_params->element_count);
	_render_virtual_geometry(draw_list, fb_format, p_params);
	RD::get_singleton()->draw_list_end();
}

namespace {
struct VirtualGeometryCandidateGPU {
	uint32_t draw[4] = {};
	uint32_t metadata[4] = {};
};
static_assert(sizeof(VirtualGeometryCandidateGPU) == 32);
}

RenderFlux::VirtualGeometryRasterState::Buffers *RenderFlux::_get_virtual_geometry_buffers(uint64_t p_instance_key, uint32_t p_material_slot, uint32_t p_capacity, uint64_t p_completed_serial, uint64_t p_pending_serial) {
	VirtualGeometryRasterState::Buffers *best = nullptr;
	for (VirtualGeometryRasterState::Buffers &buffers : virtual_geometry_raster.buffers) {
		if (buffers.last_use_serial > p_completed_serial || buffers.capacity < p_capacity) continue;
		if (!best || buffers.capacity < best->capacity) best = &buffers;
	}
	if (best) {
		best->instance_key = p_instance_key;
		best->material_slot = p_material_slot;
		best->last_use_serial = p_pending_serial;
		best->last_command_count = p_capacity;
		return best;
	}
	const uint32_t capacity = MAX(1u, p_capacity);
	if (capacity > VirtualGeometryRasterState::MAX_FRAME_ARENA_RECORDS - virtual_geometry_raster.arena_records) return nullptr;
	VirtualGeometryRasterState::Buffers buffers;
	buffers.instance_key = p_instance_key;
	buffers.material_slot = p_material_slot;
	buffers.capacity = capacity;
	buffers.last_use_serial = p_pending_serial;
	buffers.last_command_count = p_capacity;
	buffers.candidates = RD::get_singleton()->storage_buffer_create(buffers.capacity * sizeof(VirtualGeometryCandidateGPU));
	buffers.commands = RD::get_singleton()->storage_buffer_create(buffers.capacity * sizeof(RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand), {}, RD::STORAGE_BUFFER_USAGE_INDIRECT);
	buffers.counters = RD::get_singleton()->storage_buffer_create(sizeof(uint32_t) * 2);
	if (!buffers.candidates.is_valid() || !buffers.commands.is_valid() || !buffers.counters.is_valid()) {
		if (buffers.candidates.is_valid()) RD::get_singleton()->free_rid(buffers.candidates);
		if (buffers.commands.is_valid()) RD::get_singleton()->free_rid(buffers.commands);
		if (buffers.counters.is_valid()) RD::get_singleton()->free_rid(buffers.counters);
		return nullptr;
	}
	virtual_geometry_raster.buffers.push_back(buffers);
	virtual_geometry_raster.arena_records += buffers.capacity;
	return &virtual_geometry_raster.buffers.write[virtual_geometry_raster.buffers.size() - 1];
}

void RenderFlux::_append_virtual_geometry_instance_data(const RenderDataRD *p_render_data) {
	virtual_geometry_raster.instance_data_base = render_list[RENDER_LIST_OPAQUE].elements.size();
	if (!p_render_data || !p_render_data->virtual_geometry_instances || p_render_data->virtual_geometry_instances->is_empty()) return;
	const uint32_t count = p_render_data->virtual_geometry_instances->size();
	scene_state.grow_instance_buffer(RENDER_LIST_OPAQUE, virtual_geometry_raster.instance_data_base + count, true);
	if (!scene_state.curr_gpu_ptr[RENDER_LIST_OPAQUE]) {
		// Appending VG data can select a fresh UMA buffer. Rebuild conventional
		// entries when there are any, but a VG-only scene has no conventional
		// element for _fill_instance_data() to map. Map that buffer explicitly
		// before writing the first virtual instance.
		if (virtual_geometry_raster.instance_data_base > 0) {
			_fill_instance_data(RENDER_LIST_OPAQUE, nullptr, 0, -1, false);
		}
		if (!scene_state.curr_gpu_ptr[RENDER_LIST_OPAQUE]) {
			scene_state.curr_gpu_ptr[RENDER_LIST_OPAQUE] = reinterpret_cast<SceneState::InstanceData *>(scene_state.instance_buffer[RENDER_LIST_OPAQUE].map_raw_for_upload(0u));
		}
	}
	if (!scene_state.curr_gpu_ptr[RENDER_LIST_OPAQUE]) {
		virtual_geometry_raster.failed_submissions += count;
		return;
	}
	SceneState::InstanceData *dst = scene_state.curr_gpu_ptr[RENDER_LIST_OPAQUE] + virtual_geometry_raster.instance_data_base;
	for (uint32_t i = 0; i < count; i++) {
		const RendererSceneRender::VirtualGeometryInstance &instance = (*p_render_data->virtual_geometry_instances)[i];
		SceneState::InstanceData data = {};
		RendererRD::MaterialStorage::store_transform_transposed_3x4(instance.transform, data.transform);
		RendererRD::MaterialStorage::store_transform_transposed_3x4(instance.previous_transform, data.prev_transform);
		data.set_compressed_aabb(AABB(Vector3(), Vector3(1.0f, 1.0f, 1.0f)));
		data.set_uv_scale(Vector4());
		data.layer_mask = instance.visibility_layers;
		// VG records bypass _fill_instance_data(), which normally packs the
		// per-instance fade. A zero fade makes an otherwise valid opaque draw
		// transparent and lets an opaque depth prepass discard every fragment.
		data.flags = uint32_t(255) << INSTANCE_DATA_FLAGS_FADE_SHIFT;
		data.gi_offset = 0xFFFFFFFF;
		const uint64_t identity = instance.semantic_instance_id;
		data.hybrid_identity_low = uint32_t(identity);
		data.hybrid_identity_high = uint32_t(identity >> 32);
		data.set_lightmap_uv_scale(Rect2());
		dst[i] = data;
	}
	RD::get_singleton()->buffer_flush(scene_state.instance_buffer[RENDER_LIST_OPAQUE]._get(0u));
}

void RenderFlux::_prepare_virtual_geometry(RD::FramebufferFormatID p_framebuffer_format, RenderListParameters *p_params) {
	virtual_geometry_raster.prepared_draws.clear();
	if (!virtual_geometry_render_data || !virtual_geometry_render_data->virtual_geometry_instances || virtual_geometry_render_data->virtual_geometry_instances->is_empty()) return;
	switch (p_params->pass_mode) {
		case PASS_MODE_COLOR:
		case PASS_MODE_DEPTH:
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS:
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI:
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL:
		case PASS_MODE_DEPTH_MATERIAL:
			break;
		default:
			return;
	}
	if (p_params->pass_mode == PASS_MODE_COLOR && (p_params->color_pass_flags & COLOR_PASS_FLAG_TRANSPARENT)) return;
	if (!virtual_geometry_raster.pipeline.is_valid()) return;

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RD *rd = RD::get_singleton();
	const uint32_t command_capacity = 4096;
	const uint64_t completed_serial = rd->get_completed_submission_serial();
	const uint64_t pending_serial = rd->get_pending_submission_serial();
	const bool collect_virtual_geometry_diagnostics = GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/diagnostics/collect_gpu_timings");
	const uint32_t missing_materials_before = virtual_geometry_raster.missing_materials;
	const uint32_t failed_submissions_before = virtual_geometry_raster.failed_submissions;
	uint32_t selected_candidates = 0;
	if (collect_virtual_geometry_diagnostics && !virtual_geometry_raster.command_readback_proven) {
		for (VirtualGeometryRasterState::Buffers &buffers : virtual_geometry_raster.buffers) {
			if (buffers.last_command_count == 0 || buffers.last_readback_serial == buffers.last_use_serial || buffers.last_use_serial > completed_serial) continue;
			const Vector<uint8_t> bytes = rd->buffer_get_data(buffers.commands, 0, buffers.last_command_count * sizeof(RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand));
			if (bytes.size() != int64_t(buffers.last_command_count * sizeof(RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand))) continue;
			if (buffers.expected_commands.size() != int(buffers.last_command_count)) continue;
			uint32_t nonzero_commands = 0;
			uint32_t index_count_matches = 0;
			uint32_t instance_count_matches = 0;
			uint32_t first_index_matches = 0;
			uint32_t vertex_offset_matches = 0;
			uint32_t first_instance_matches = 0;
			for (uint32_t command_index = 0; command_index < buffers.last_command_count; command_index++) {
				RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand command;
				memcpy(&command, bytes.ptr() + command_index * sizeof(command), sizeof(command));
				const RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand &expected = buffers.expected_commands[command_index];
				nonzero_commands += command.index_count != 0 && command.instance_count != 0;
				index_count_matches += command.index_count == expected.index_count;
				instance_count_matches += command.instance_count == expected.instance_count;
				first_index_matches += command.first_index == expected.first_index;
				vertex_offset_matches += command.vertex_offset == expected.vertex_offset;
				first_instance_matches += command.first_instance == expected.first_instance;
			}
			const uint32_t field_mismatches = (buffers.last_command_count - index_count_matches) + (buffers.last_command_count - instance_count_matches) + (buffers.last_command_count - first_index_matches) + (buffers.last_command_count - vertex_offset_matches) + (buffers.last_command_count - first_instance_matches);
			if (nonzero_commands == buffers.last_command_count && field_mismatches == 0) {
				virtual_geometry_raster.command_readback_proven = true;
			}
			print_line(vformat("Flux virtual raster command readback: commands=%d nonzero=%d rejected=%d index_count=%d/%d instance_count=%d/%d first_index=%d/%d vertex_offset=%d/%d first_instance=%d/%d serial=%d.", buffers.last_command_count, nonzero_commands, field_mismatches, index_count_matches, buffers.last_command_count, instance_count_matches, buffers.last_command_count, first_index_matches, buffers.last_command_count, vertex_offset_matches, buffers.last_command_count, first_instance_matches, buffers.last_command_count, buffers.last_use_serial));
			buffers.last_readback_serial = buffers.last_use_serial;
			break;
		}
	}
	HashSet<uint64_t> visible_semantic_ids;
	for (const RendererSceneRender::VirtualGeometryInstance &instance : *virtual_geometry_render_data->virtual_geometry_instances) {
		if (instance.semantic_instance_id != 0) visible_semantic_ids.insert(instance.semantic_instance_id);
	}
	Vector<uint64_t> stale_selection_states;
	for (const KeyValue<uint64_t, VirtualGeometryRasterState::InstanceSelectionState> &entry : virtual_geometry_raster.instance_selection_states) {
		if (!visible_semantic_ids.has(entry.key)) stale_selection_states.push_back(entry.key);
	}
	for (uint64_t stale_id : stale_selection_states) virtual_geometry_raster.instance_selection_states.erase(stale_id);

	for (uint32_t instance_index = 0; instance_index < virtual_geometry_render_data->virtual_geometry_instances->size(); instance_index++) {
		const RendererSceneRender::VirtualGeometryInstance &instance = (*virtual_geometry_render_data->virtual_geometry_instances)[instance_index];
		if (instance.semantic_instance_id == 0) {
			virtual_geometry_raster.failed_submissions++;
			continue;
		}
		RendererVirtualGeometry::VirtualGeometryStorage *storage = virtual_geometry_get_storage(instance.resource);
		if (!storage || !storage->is_raster_integration_enabled() || storage->get_active_descriptor_generation() == 0 || !storage->get_position_heap_rid().is_valid() || !storage->get_index_heap_rid().is_valid() || !storage->get_attribute_heap_rid().is_valid() || !storage->get_cluster_descriptor_rid().is_valid() || !storage->get_index_array_rid().is_valid()) {
			virtual_geometry_raster.failed_submissions++;
			continue;
		}

		RendererVirtualGeometry::VirtualGeometryRasterSelectionInput input;
		input.command_capacity = command_capacity;
		input.backend_compute_indirect_available = true;
		const Transform3D local_from_world = instance.transform.affine_inverse();
		for (uint32_t eye = 0; eye < p_params->view_count; eye++) {
			RendererVirtualGeometry::VirtualGeometryRasterView view;
			const Transform3D local_from_view = local_from_world * (virtual_geometry_render_data->scene_data->cam_transform * Transform3D(Basis(), virtual_geometry_render_data->scene_data->view_eye_offset[eye]));
			const Projection &eye_projection = virtual_geometry_render_data->scene_data->view_projection[eye];
			view.camera_position = local_from_view.origin;
			view.view_direction = -local_from_view.basis.get_column(Vector3::AXIS_Z).normalized();
			view.frustum_planes = eye_projection.get_projection_planes(local_from_view);
			view.viewport_height = MAX(1, virtual_geometry_render_data->render_buffers->get_internal_size().y);
			view.vertical_fov_radians = Math::deg_to_rad(eye_projection.get_fovy(eye_projection.get_fov(), 1.0 / eye_projection.get_aspect()));
			view.near_plane_uncertain = true; // No VG HZB yet: hardware raster clips conservatively.
			view.stereo_boundary_uncertain = p_params->view_count > 1;
			input.views.push_back(view);
		}
		VirtualGeometryRasterState::InstanceSelectionState &selection_state = virtual_geometry_raster.instance_selection_states[instance.semantic_instance_id];
		if (selection_state.resource_revision != instance.resource_revision) {
			selection_state.resource_revision = instance.resource_revision;
			selection_state.selection.refined_group_ids.clear();
		}
		RendererVirtualGeometry::VirtualGeometryRasterSelection selection = storage->select_raster_reference(input, &selection_state.selection);
		virtual_geometry_raster.overflows += selection.diagnostics.overflow_clusters;
		if (collect_virtual_geometry_diagnostics && selection_state.reported_descriptor_generation != storage->get_active_descriptor_generation()) {
			selection_state.reported_descriptor_generation = storage->get_active_descriptor_generation();
			const RendererVirtualGeometry::VirtualGeometryRuntimeDiagnostics diagnostics = storage->get_diagnostics();
			print_line(vformat("Flux virtual raster: semantic=%d generation=%d active_pages=%d requested_pages=%d selected=%d fallback=%d demand=%d descriptor_publications=%d arena_records=%d/%d.", instance.semantic_instance_id, storage->get_active_descriptor_generation(), diagnostics.active_pages, diagnostics.requested_pages, selection.cluster_ids.size(), selection.diagnostics.fallback_clusters, selection.requested_page_ids.size(), diagnostics.descriptor_publications, virtual_geometry_raster.arena_records, VirtualGeometryRasterState::MAX_FRAME_ARENA_RECORDS));
		}
		for (uint32_t request_index = 0; request_index < uint32_t(selection.requested_page_ids.size()); request_index++) {
			const uint64_t page_id = selection.requested_page_ids[request_index];
			const uint32_t priority = selection.requested_page_priorities[request_index];
			storage->request_page(page_id, RendererVirtualGeometry::VirtualGeometryRequestReason::STEREO_UNION, priority);
			storage->mark_raster_interest(page_id, pending_serial, priority);
		}
		if (selection.cluster_ids.is_empty()) continue;

		HashMap<uint32_t, Vector<VirtualGeometryCandidateGPU>> by_material;
		const Vector<RendererVirtualGeometry::VirtualGeometryGPUClusterDescriptor> &descriptors = storage->get_gpu_cluster_descriptors();
		for (uint32_t selected = 0; selected < selection.cluster_ids.size(); selected++) {
			const uint32_t *descriptor_slot = storage->get_gpu_cluster_descriptor_slot(selection.cluster_ids[selected]);
			if (!descriptor_slot || *descriptor_slot >= uint32_t(descriptors.size()) || descriptors[*descriptor_slot].stable_id != selection.cluster_ids[selected] || descriptors[*descriptor_slot].generation != uint32_t(storage->get_active_descriptor_generation())) {
				virtual_geometry_raster.failed_submissions++;
				continue;
			}
			const RendererVirtualGeometry::VirtualGeometryGPUClusterDescriptor &descriptor = descriptors[*descriptor_slot];
			selected_candidates++;
			if (descriptor.material_slot >= uint32_t(instance.material_bindings.size()) || !instance.material_bindings[descriptor.material_slot].is_valid()) {
				virtual_geometry_raster.missing_materials++;
				continue;
			}
			VirtualGeometryCandidateGPU candidate;
			candidate.draw[0] = descriptor.index_count;
			candidate.draw[1] = descriptor.first_index;
			candidate.draw[2] = descriptor.base_vertex;
			candidate.metadata[0] = *descriptor_slot;
			candidate.metadata[1] = uint32_t(storage->get_active_descriptor_generation());
			candidate.metadata[2] = selection.eye_visibility_masks[selected];
			candidate.metadata[3] = descriptor.source_stream_flags;
			by_material[descriptor.material_slot].push_back(candidate);
		}

		for (KeyValue<uint32_t, Vector<VirtualGeometryCandidateGPU>> &entry : by_material) {
			const uint32_t material_slot = entry.key;
			Vector<VirtualGeometryCandidateGPU> &candidates = entry.value;
			RID material_rid = instance.material_bindings[material_slot];
			SceneShaderFlux::MaterialData *material = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(material_rid, RendererRD::MaterialStorage::SHADER_TYPE_3D));
			if (!material || !material->shader_data || !material->shader_data->is_valid() || !material->uniform_set.is_valid() || !rd->uniform_set_is_valid(material->uniform_set) || material->shader_data->uses_alpha_pass() || material->shader_data->uses_vertex || material->shader_data->uses_position || material->shader_data->uses_point_size) {
				// The initial path is static opaque canonical vertex data only. Do not
				// substitute Flux's default material for a rejected authored shader.
				virtual_geometry_raster.missing_materials++;
				continue;
			}
			const uint64_t buffer_key = instance.semantic_instance_id ^ (uint64_t(instance_index) << 32);
			VirtualGeometryRasterState::Buffers *buffers = _get_virtual_geometry_buffers(buffer_key, material_slot, candidates.size(), completed_serial, pending_serial);
			if (!buffers) {
				virtual_geometry_raster.failed_submissions++;
				continue;
			}
			uint32_t zero_counters[2] = {};
			if (rd->buffer_update(buffers->candidates, 0, candidates.size() * sizeof(VirtualGeometryCandidateGPU), candidates.ptr()) != OK || rd->buffer_update(buffers->counters, 0, sizeof(zero_counters), zero_counters) != OK) {
				virtual_geometry_raster.failed_submissions++;
				continue;
			}
			buffers->expected_commands.resize(candidates.size());
			for (uint32_t candidate_index = 0; candidate_index < uint32_t(candidates.size()); candidate_index++) {
				const VirtualGeometryCandidateGPU &candidate = candidates[candidate_index];
				RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand &expected = buffers->expected_commands.write[candidate_index];
				expected.index_count = candidate.draw[0];
				expected.instance_count = 1;
				expected.first_index = candidate.draw[1];
				expected.vertex_offset = int32_t(candidate.draw[2]);
				expected.first_instance = 0;
			}

			Vector<RD::Uniform> uniforms;
			auto bind_storage = [&uniforms](uint32_t p_binding, RID p_rid) {
				RD::Uniform uniform;
				uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
				uniform.binding = p_binding;
				uniform.append_id(p_rid);
				uniforms.push_back(uniform);
			};
			bind_storage(0, buffers->candidates);
			bind_storage(1, buffers->commands);
			bind_storage(2, buffers->counters);
			bind_storage(3, storage->get_cluster_descriptor_rid());
			RID selector_set = rd->uniform_set_create(uniforms, virtual_geometry_raster.shader.version_get_shader(virtual_geometry_raster.shader_version, 0), 0, true);
			if (!selector_set.is_valid()) {
				virtual_geometry_raster.failed_submissions++;
				continue;
			}
			struct SelectorPushConstants { uint32_t candidate_count; uint32_t command_capacity; uint32_t descriptor_generation; uint32_t eye_index; } constants = { uint32_t(candidates.size()), buffers->capacity, uint32_t(storage->get_active_descriptor_generation()), 0 };
			RD::ComputeListID compute_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(compute_list, virtual_geometry_raster.pipeline);
			rd->compute_list_bind_uniform_set(compute_list, selector_set, 0);
			rd->compute_list_set_push_constant(compute_list, &constants, sizeof(constants));
			rd->compute_list_dispatch_threads(compute_list, candidates.size(), 1, 1);
			rd->compute_list_end();
			rd->barrier(RD::BARRIER_MASK_COMPUTE, RD::BARRIER_MASK_RASTER);

			SceneShaderFlux::ShaderData *shader = material->shader_data;
			SceneShaderFlux::ShaderData::PipelineKey key;
			key.vertex_format_id = virtual_geometry_raster.vertex_format;
			key.framebuffer_format_id = p_framebuffer_format;
			key.primitive_type = RSE::PRIMITIVE_TRIANGLES;
			key.cull_mode = shader->get_cull_mode_from_cull_variant(p_params->reverse_cull ? SceneShaderFlux::ShaderData::CULL_VARIANT_REVERSED : SceneShaderFlux::ShaderData::CULL_VARIANT_NORMAL);
			key.shader_specialization = p_params->base_specialization;
			key.wireframe = p_params->force_wireframe;
			if (p_params->pass_mode == PASS_MODE_COLOR) {
				key.version = SceneShaderFlux::PIPELINE_VERSION_COLOR_PASS;
				if (p_params->color_pass_flags & COLOR_PASS_FLAG_MULTIVIEW) key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MULTIVIEW;
				if (p_params->color_pass_flags & COLOR_PASS_FLAG_PRIMARY_SURFACE) key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_PRIMARY_SURFACE;
				if (p_params->color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
			} else {
				switch (p_params->pass_mode) {
					case PASS_MODE_DEPTH:
						key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS;
						break;
					case PASS_MODE_DEPTH_NORMAL_ROUGHNESS:
						key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
						break;
					case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI:
						key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
						break;
					case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL:
						key.version = p_params->view_count > 1 ? SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_HYBRID_MATERIAL_MULTIVIEW : SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_HYBRID_MATERIAL;
						break;
					case PASS_MODE_DEPTH_MATERIAL:
						key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL;
						break;
					default:
						ERR_FAIL_MSG("Unsupported virtual geometry depth pass.");
				}
			}
			// Virtual geometry has no MeshSurface to compile this draw pipeline ahead of the
			// first indirect submission. Wait once here so a pure-VG scene cannot drop every
			// batch while an otherwise identical mixed scene supplies the conventional warmup.
			RID pipeline = shader->pipeline_hash_map.get_pipeline(key, key.hash(), true, RSE::PIPELINE_SOURCE_DRAW);
			if (!pipeline.is_valid()) {
				virtual_geometry_raster.failed_submissions++;
				continue;
			}
			Vector<RID> vertex_buffers = { storage->get_position_vertex_buffer_rid(), storage->get_attribute_vertex_buffer_rid() };
			VirtualGeometryRasterState::PreparedDraw prepared;
			prepared.pipeline = pipeline;
			prepared.material_uniform_set = material->uniform_set;
			prepared.position_buffer = vertex_buffers[0];
			prepared.attribute_buffer = vertex_buffers[1];
			prepared.index_array = storage->get_index_array_rid();
			prepared.commands = buffers->commands;
			prepared.command_count = candidates.size();
			prepared.instance_index = instance_index;
			prepared.vertex_count = storage->get_position_vertex_count();
			virtual_geometry_raster.prepared_draws.push_back(prepared);
			for (uint64_t selected_cluster : selection.cluster_ids) for (const RendererVirtualGeometry::ClusterDescriptor &cluster : storage->get_package().manifest.clusters) if (cluster.stable_id == selected_cluster) { storage->mark_page_used(cluster.page_id, pending_serial, 0); storage->mark_raster_interest(cluster.page_id, pending_serial, UINT32_MAX); break; }
			virtual_geometry_raster.submitted_commands += candidates.size();
		}
	}
	if (collect_virtual_geometry_diagnostics && p_params->pass_mode == PASS_MODE_COLOR && selected_candidates > 0 && virtual_geometry_raster.prepared_draws.is_empty()) {
		if (!virtual_geometry_raster.reported_empty_prepared_draws) {
			print_line(vformat("Flux virtual raster submitted no color draw: candidates=%d material_rejections=%d submission_failures=%d.", selected_candidates, virtual_geometry_raster.missing_materials - missing_materials_before, virtual_geometry_raster.failed_submissions - failed_submissions_before));
			virtual_geometry_raster.reported_empty_prepared_draws = true;
		}
	} else if (!virtual_geometry_raster.prepared_draws.is_empty()) {
		virtual_geometry_raster.reported_empty_prepared_draws = false;
	}
}

void RenderFlux::_render_virtual_geometry(RD::DrawListID p_draw_list, RD::FramebufferFormatID p_framebuffer_format, RenderListParameters *p_params) {
	(void)p_framebuffer_format;
	RD *rd = RD::get_singleton();
	for (const VirtualGeometryRasterState::PreparedDraw &prepared : virtual_geometry_raster.prepared_draws) {
		if (!prepared.pipeline.is_valid() || !prepared.material_uniform_set.is_valid() || !prepared.index_array.is_valid() || !prepared.commands.is_valid()) continue;
		rd->draw_list_bind_uniform_set(p_draw_list, render_base_uniform_set, SCENE_UNIFORM_SET);
		rd->draw_list_bind_uniform_set(p_draw_list, p_params->render_pass_uniform_set, RENDER_PASS_UNIFORM_SET);
		rd->draw_list_bind_uniform_set(p_draw_list, scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);
		rd->draw_list_bind_uniform_set(p_draw_list, prepared.material_uniform_set, MATERIAL_UNIFORM_SET);
		const Vector<RID> vertex_buffers = { prepared.position_buffer, prepared.attribute_buffer };
		rd->draw_list_bind_vertex_buffers_format(p_draw_list, virtual_geometry_raster.vertex_format, prepared.vertex_count, vertex_buffers);
		rd->draw_list_bind_index_array(p_draw_list, prepared.index_array);
		rd->draw_list_bind_render_pipeline(p_draw_list, prepared.pipeline);
		SceneState::PushConstant draw_constants = {};
		draw_constants.base_index = virtual_geometry_raster.instance_data_base + prepared.instance_index;
		rd->draw_list_set_push_constant(p_draw_list, &draw_constants, sizeof(SceneState::PushConstant) - sizeof(SceneState::PushConstantUbershader));
		rd->draw_list_draw_indirect(p_draw_list, true, prepared.commands, 0, prepared.command_count, sizeof(RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand));
	}
}

uint32_t RenderFlux::_setup_environment(const RenderDataRD *p_render_data, bool p_no_fog, const Size2i &p_screen_size, const Size2 &p_viewport_size, const Color &p_default_bg_color, bool p_opaque_render_buffers, bool p_apply_alpha_multiplier, bool p_pancake_shadows) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rd = p_render_data->render_buffers;
	RID env = is_environment(p_render_data->environment) ? p_render_data->environment : RID();
	RID reflection_probe_instance = p_render_data->reflection_probe.is_valid() ? light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe) : RID();

	// May do this earlier in RenderSceneRenderRD::render_scene
	uint32_t uniform_buffer_index = scene_state.used_uniform_buffer_count;
	++scene_state.used_uniform_buffer_count;

	if (uniform_buffer_index >= scene_state.uniform_buffers.size()) {
		uint32_t from = scene_state.uniform_buffers.size();
		scene_state.uniform_buffers.resize(uniform_buffer_index + 1);
		for (uint32_t i = from; i < scene_state.uniform_buffers.size(); i++) {
			scene_state.uniform_buffers[i] = p_render_data->scene_data->create_uniform_buffer();
		}
	}

	float luminance_multiplier = rd.is_valid() ? rd->get_luminance_multiplier() : 1.0;

	p_render_data->scene_data->update_ubo(scene_state.uniform_buffers[uniform_buffer_index], get_debug_draw_mode(), env, reflection_probe_instance, p_render_data->camera_attributes, p_pancake_shadows, p_screen_size, p_viewport_size, p_default_bg_color, luminance_multiplier, p_opaque_render_buffers, p_apply_alpha_multiplier);

	// now do implementation UBO

	scene_state.ubo.cluster_shift = Math::get_shift_from_power_of_2(p_render_data->cluster_size);
	scene_state.ubo.max_cluster_element_count_div_32 = p_render_data->cluster_max_elements / 32;
	{
		uint32_t cluster_screen_width = Math::division_round_up((uint32_t)p_screen_size.width, p_render_data->cluster_size);
		uint32_t cluster_screen_height = Math::division_round_up((uint32_t)p_screen_size.height, p_render_data->cluster_size);
		scene_state.ubo.cluster_type_size = cluster_screen_width * cluster_screen_height * (scene_state.ubo.max_cluster_element_count_div_32 + 32);
		scene_state.ubo.cluster_width = cluster_screen_width;
	}

	scene_state.ubo.gi_upscale_for_msaa = false;
	scene_state.ubo.volumetric_fog_enabled = false;
	memset(scene_state.ubo.sky_solar_direction_enabled, 0, sizeof(scene_state.ubo.sky_solar_direction_enabled));
	memset(scene_state.ubo.sky_solar_irradiance_size, 0, sizeof(scene_state.ubo.sky_solar_irradiance_size));
	memset(scene_state.ubo.sky_lunar_direction_enabled, 0, sizeof(scene_state.ubo.sky_lunar_direction_enabled));
	memset(scene_state.ubo.sky_lunar_irradiance_size, 0, sizeof(scene_state.ubo.sky_lunar_irradiance_size));

	// Finite procedural-Sky lobes are injected once by RendererSceneCull as an
	// internal directional source. Do not also add the old unshadowed UBO term:
	// that would give the same sky lobe two direct-light owners.

	if (rd.is_valid()) {
		if (rd->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED) {
			scene_state.ubo.gi_upscale_for_msaa = true;
		}

		if (rd->has_custom_data(RB_SCOPE_FOG)) {
			Ref<RendererRD::Fog::VolumetricFog> fog = rd->get_custom_data(RB_SCOPE_FOG);

			scene_state.ubo.volumetric_fog_enabled = true;
			float fog_end = fog->length;
			if (fog_end > 0.0) {
				scene_state.ubo.volumetric_fog_inv_length = 1.0 / fog_end;
			} else {
				scene_state.ubo.volumetric_fog_inv_length = 1.0;
			}

			float fog_detail_spread = fog->spread; //reverse lookup
			if (fog_detail_spread > 0.0) {
				scene_state.ubo.volumetric_fog_detail_spread = 1.0 / fog_detail_spread;
			} else {
				scene_state.ubo.volumetric_fog_detail_spread = 1.0;
			}
		}
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_UNSHADED) {
		scene_state.ubo.ss_effects_flags = 0;
	} else if (p_render_data->reflection_probe.is_null() && is_environment(p_render_data->environment)) {
		scene_state.ubo.ssao_ao_affect = environment_get_ssao_ao_channel_affect(p_render_data->environment);
		scene_state.ubo.ssao_light_affect = environment_get_ssao_direct_light_affect(p_render_data->environment);
		uint32_t ss_flags = 0;
		if (p_opaque_render_buffers) {
			ss_flags |= environment_get_ssao_enabled(p_render_data->environment) ? (1 << 0) : 0;
			ss_flags |= environment_get_ssil_enabled(p_render_data->environment) ? (1 << 1) : 0;
			ss_flags |= environment_get_ssr_enabled(p_render_data->environment) ? (1 << 2) : 0;

			if (rd.is_valid()) {
				Ref<RenderBufferDataFlux> rb_data;
				if (rd->has_custom_data(RB_SCOPE_FLUX)) {
					rb_data = rd->get_custom_data(RB_SCOPE_FLUX);
					ss_flags |= (rb_data.is_valid() && !rb_data->ss_effects_data.ssr.half_size) ? (1 << 3) : 0;
				}
			}
		}
		scene_state.ubo.ss_effects_flags = ss_flags;
	} else {
		scene_state.ubo.ss_effects_flags = 0;
	}

	if (uniform_buffer_index >= scene_state.implementation_uniform_buffers.size()) {
		uint32_t from = scene_state.implementation_uniform_buffers.size();
		scene_state.implementation_uniform_buffers.resize(uniform_buffer_index + 1);
		for (uint32_t i = from; i < scene_state.implementation_uniform_buffers.size(); i++) {
			scene_state.implementation_uniform_buffers[i] = RD::get_singleton()->uniform_buffer_create(sizeof(SceneState::UBO));
		}
	}

	RD::get_singleton()->buffer_update(scene_state.implementation_uniform_buffers[uniform_buffer_index], 0, sizeof(SceneState::UBO), &scene_state.ubo);

	return uniform_buffer_index;
}

void RenderFlux::SceneState::grow_instance_buffer(RenderListType p_render_list, uint32_t p_req_element_count, bool p_append) {
	if (p_req_element_count > 0) {
		if (instance_buffer[p_render_list].get_size(0u) < p_req_element_count * sizeof(SceneState::InstanceData)) {
			instance_buffer[p_render_list].uninit();
			uint32_t new_size = Math::nearest_power_of_2_templated(MAX(uint64_t(INSTANCE_DATA_BUFFER_MIN_SIZE), p_req_element_count));
			instance_buffer[p_render_list].set_storage_size(0u, new_size * sizeof(SceneState::InstanceData));
			curr_gpu_ptr[p_render_list] = nullptr;
		}

		const bool must_remap = instance_buffer[p_render_list].prepare_for_map(p_append);
		if (must_remap) {
			curr_gpu_ptr[p_render_list] = nullptr;
		}
	}
}

void RenderFlux::_fill_instance_data(RenderListType p_render_list, int *p_render_info, uint32_t p_offset, int32_t p_max_elements, bool p_update_buffer) {
	RenderList *rl = &render_list[p_render_list];
	uint32_t element_total = p_max_elements >= 0 ? uint32_t(p_max_elements) : rl->elements.size();

	rl->element_info.resize(p_offset + element_total);

	// If p_offset == 0, grow_instance_buffer resets and increment the buffer.
	// If this behavior ever changes, _render_shadow_begin may need to change.
	scene_state.grow_instance_buffer(p_render_list, p_offset + element_total, p_offset != 0u);
	if (!scene_state.curr_gpu_ptr[p_render_list] && element_total > 0u) {
		// The old buffer was replaced for another larger one. We must start copying from scratch.
		element_total += p_offset;
		p_offset = 0u;
		scene_state.curr_gpu_ptr[p_render_list] = reinterpret_cast<SceneState::InstanceData *>(scene_state.instance_buffer[p_render_list].map_raw_for_upload(0u));
	}

	if (p_render_info) {
		p_render_info[RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += element_total;
	}

	uint32_t repeats = 0;
	GeometryInstanceSurfaceDataCache *prev_surface = nullptr;
	for (uint32_t i = 0; i < element_total; i++) {
		GeometryInstanceSurfaceDataCache *surface = rl->elements[i + p_offset];
		GeometryInstanceFlux *inst = surface->owner;

		SceneState::InstanceData instance_data;

		if (likely(inst->store_transform_cache)) {
			RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->transform, instance_data.transform);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->prev_transform, instance_data.prev_transform);

#ifdef REAL_T_IS_DOUBLE
			// Split the origin into two components, the float approximation and the missing precision.
			// In the shader we will combine these back together to restore the lost precision.
			RendererRD::MaterialStorage::split_double(inst->transform.origin.x, &instance_data.transform[3], &instance_data.model_precision[0]);
			RendererRD::MaterialStorage::split_double(inst->transform.origin.y, &instance_data.transform[7], &instance_data.model_precision[1]);
			RendererRD::MaterialStorage::split_double(inst->transform.origin.z, &instance_data.transform[11], &instance_data.model_precision[2]);
			RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.x, &instance_data.prev_transform[3], &instance_data.prev_model_precision[0]);
			RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.y, &instance_data.prev_transform[7], &instance_data.prev_model_precision[1]);
			RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.z, &instance_data.prev_transform[11], &instance_data.prev_model_precision[2]);
#endif
		} else {
			RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), instance_data.transform);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), instance_data.prev_transform);
#ifdef REAL_T_IS_DOUBLE
			memset(instance_data.model_precision, 0, sizeof(instance_data.model_precision));
			memset(instance_data.prev_model_precision, 0, sizeof(instance_data.prev_model_precision));
#endif
		}

		instance_data.flags = inst->flags_cache;
		instance_data.gi_offset = inst->gi_offset_cache;
		instance_data.layer_mask = inst->layer_mask;
		instance_data.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
		const uint64_t hybrid_identity = uint64_t(uintptr_t(inst)) ^ uint64_t(surface->surface_index + 1);
		instance_data.hybrid_identity_low = uint32_t(hybrid_identity);
		instance_data.hybrid_identity_high = uint32_t(hybrid_identity >> 32);
		instance_data.hybrid_identity_padding_0 = 0;
		instance_data.hybrid_identity_padding_1 = 0;
		instance_data.set_lightmap_uv_scale(inst->lightmap_uv_scale);

		AABB surface_aabb = AABB(Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0));
		uint64_t format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(surface->surface);
		Vector4 uv_scale = Vector4(0.0, 0.0, 0.0, 0.0);

		if (format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
			surface_aabb = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_aabb(surface->surface);
			uv_scale = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_uv_scale(surface->surface);
		}

		instance_data.set_compressed_aabb(surface_aabb);
		instance_data.set_uv_scale(uv_scale);

		scene_state.curr_gpu_ptr[p_render_list][i + p_offset] = instance_data;

		const bool cant_repeat = instance_data.flags & INSTANCE_DATA_FLAG_MULTIMESH || inst->mesh_instance.is_valid();

		if (prev_surface != nullptr && !cant_repeat && prev_surface->sort.sort_key1 == surface->sort.sort_key1 && prev_surface->sort.sort_key2 == surface->sort.sort_key2 && inst->mirror == prev_surface->owner->mirror && repeats < RenderElementInfo::MAX_REPEATS) {
			//this element is the same as the previous one, count repeats to draw it using instancing
			repeats++;
		} else {
			if (repeats > 0) {
				for (uint32_t j = 1; j <= repeats; j++) {
					rl->element_info[p_offset + i - j].repeat = j;
				}
			}
			repeats = 1;
			if (p_render_info) {
				p_render_info[RSE::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME]++;
			}
		}

		RenderElementInfo &element_info = rl->element_info[p_offset + i];

		element_info.value = uint32_t(surface->sort.sort_key1 & 0xFFF);

		if (cant_repeat) {
			prev_surface = nullptr;
		} else {
			prev_surface = surface;
		}
	}

	if (repeats > 0) {
		for (uint32_t j = 1; j <= repeats; j++) {
			rl->element_info[p_offset + element_total - j].repeat = j;
		}
	}

	if (p_update_buffer && element_total > 0u) {
		RenderingDevice::get_singleton()->buffer_flush(scene_state.instance_buffer[p_render_list]._get(0u));
	}
}

_FORCE_INLINE_ static uint32_t _indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}
void RenderFlux::_fill_render_list(RenderListType p_render_list, const RenderDataRD *p_render_data, PassMode p_pass_mode, bool p_using_sdfgi, bool p_using_opaque_gi, bool p_using_motion_pass, bool p_append) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint64_t frame = RSG::rasterizer->get_frame_number();

	if (p_render_list == RENDER_LIST_OPAQUE) {
		scene_state.used_sss = false;
		scene_state.used_screen_texture = false;
		scene_state.used_normal_texture = false;
		scene_state.used_depth_texture = false;
		scene_state.used_lightmap = false;
		scene_state.used_opaque_stencil = false;
	}
	uint32_t lightmap_captures_used = 0;

	Plane near_plane = Plane(-p_render_data->scene_data->cam_transform.basis.get_column(Vector3::AXIS_Z), p_render_data->scene_data->cam_transform.origin);
	near_plane.d += p_render_data->scene_data->cam_projection.get_z_near();
	float z_max = p_render_data->scene_data->cam_projection.get_z_far() - p_render_data->scene_data->cam_projection.get_z_near();

	RenderList *rl = &render_list[p_render_list];
	_update_dirty_geometry_instances();

	if (!p_append) {
		rl->clear();
		if (p_render_list == RENDER_LIST_OPAQUE) {
			// Opaque fills motion and alpha lists.
			render_list[RENDER_LIST_MOTION].clear();
			render_list[RENDER_LIST_ALPHA].clear();
		}
	}

	//fill list

	for (int i = 0; i < (int)p_render_data->instances->size(); i++) {
		GeometryInstanceFlux *inst = static_cast<GeometryInstanceFlux *>((*p_render_data->instances)[i]);

		Vector3 center = inst->transform.origin;
		if (p_render_data->scene_data->cam_orthogonal) {
			if (inst->use_aabb_center) {
				center = inst->transformed_aabb.get_support(-near_plane.normal);
			}
			inst->depth = near_plane.distance_to(center) - inst->sorting_offset;
		} else {
			if (inst->use_aabb_center) {
				center = inst->transformed_aabb.position + (inst->transformed_aabb.size * 0.5);
			}
			inst->depth = p_render_data->scene_data->cam_transform.origin.distance_to(center) - inst->sorting_offset;
		}
		uint32_t depth_layer = CLAMP(int(inst->depth * 16 / z_max), 0, 15);

		uint32_t flags = inst->base_flags; //fill flags if appropriate

		if (inst->non_uniform_scale) {
			flags |= INSTANCE_DATA_FLAGS_NON_UNIFORM_SCALE;
		}
		bool uses_lightmap = false;
		bool uses_gi = false;
		bool uses_motion = false;
		float fade_alpha = 1.0;

		if (inst->fade_near || inst->fade_far) {
			float fade_dist = inst->transformed_aabb.get_center().distance_to(p_render_data->scene_data->cam_transform.origin);
			// Use `smoothstep()` to make opacity changes more gradual and less noticeable to the player.
			if (inst->fade_far && fade_dist > inst->fade_far_begin) {
				fade_alpha = Math::smoothstep(0.0f, 1.0f, 1.0f - (fade_dist - inst->fade_far_begin) / (inst->fade_far_end - inst->fade_far_begin));
			} else if (inst->fade_near && fade_dist < inst->fade_near_end) {
				fade_alpha = Math::smoothstep(0.0f, 1.0f, (fade_dist - inst->fade_near_begin) / (inst->fade_near_end - inst->fade_near_begin));
			}
		}

		fade_alpha *= inst->force_alpha * inst->parent_fade_alpha;

		flags = (flags & ~INSTANCE_DATA_FLAGS_FADE_MASK) | (uint32_t(fade_alpha * 255.0) << INSTANCE_DATA_FLAGS_FADE_SHIFT);

		if (p_render_list == RENDER_LIST_OPAQUE) {
			// Setup GI
			if (inst->lightmap_instance.is_valid()) {
				// find index of the lightmap_instance of the instance being rendered
				int32_t lightmap_cull_index = -1;
				for (uint32_t j = 0; j < scene_state.lightmaps_used; j++) {
					if (scene_state.lightmap_ids[j] == inst->lightmap_instance) {
						lightmap_cull_index = j;
						break;
					}
				}
				if (lightmap_cull_index >= 0) {
					inst->gi_offset_cache = inst->lightmap_slice_index << 16;
					inst->gi_offset_cache |= lightmap_cull_index;
					flags |= INSTANCE_DATA_FLAG_USE_LIGHTMAP;
					if (scene_state.lightmap_has_sh[lightmap_cull_index]) {
						flags |= INSTANCE_DATA_FLAG_USE_SH_LIGHTMAP;
					}
					uses_lightmap = true;
				} else {
					inst->gi_offset_cache = 0xFFFFFFFF;
				}

			} else if (inst->lightmap_sh) {
				if (lightmap_captures_used < scene_state.max_lightmap_captures) {
					const Color *src_capture = inst->lightmap_sh->sh;
					LightmapCaptureData &lcd = scene_state.lightmap_captures[lightmap_captures_used];
					for (int j = 0; j < 9; j++) {
						lcd.sh[j * 4 + 0] = src_capture[j].r;
						lcd.sh[j * 4 + 1] = src_capture[j].g;
						lcd.sh[j * 4 + 2] = src_capture[j].b;
						lcd.sh[j * 4 + 3] = src_capture[j].a;
					}
					flags |= INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE;
					inst->gi_offset_cache = lightmap_captures_used;
					lightmap_captures_used++;
					uses_lightmap = true;
				}

			} else {
				if (p_using_opaque_gi) {
					flags |= INSTANCE_DATA_FLAG_USE_GI_BUFFERS;
				}

				if (inst->voxel_gi_instances[0].is_valid()) {
					uint32_t probe0_index = 0xFFFF;
					uint32_t probe1_index = 0xFFFF;

					for (uint32_t j = 0; j < scene_state.voxelgis_used; j++) {
						if (scene_state.voxelgi_ids[j] == inst->voxel_gi_instances[0]) {
							probe0_index = j;
						} else if (scene_state.voxelgi_ids[j] == inst->voxel_gi_instances[1]) {
							probe1_index = j;
						}
					}

					if (probe0_index == 0xFFFF && probe1_index != 0xFFFF) {
						//0 must always exist if a probe exists
						SWAP(probe0_index, probe1_index);
					}

					inst->gi_offset_cache = probe0_index | (probe1_index << 16);
					flags |= INSTANCE_DATA_FLAG_USE_VOXEL_GI;
					uses_gi = true;
				} else {
					if (p_using_sdfgi && inst->can_sdfgi) {
						flags |= INSTANCE_DATA_FLAG_USE_SDFGI;
						uses_gi = true;
					}
					inst->gi_offset_cache = 0xFFFFFFFF;
				}
			}
			if (p_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || p_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI || p_pass_mode == PASS_MODE_COLOR) {
				bool transform_changed = inst->transform_status == GeometryInstanceFlux::TransformStatus::MOVED;
				bool has_mesh_instance = inst->mesh_instance.is_valid();
				bool uses_particles = inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES;
				bool is_multimesh_with_motion = !uses_particles && (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) && mesh_storage->_multimesh_uses_motion_vectors_offsets(inst->data->base);
				bool is_dynamic = transform_changed || has_mesh_instance || uses_particles || is_multimesh_with_motion;
				if (p_pass_mode == PASS_MODE_COLOR && p_using_motion_pass) {
					uses_motion = is_dynamic;
				} else if (is_dynamic) {
					flags |= INSTANCE_DATA_FLAGS_DYNAMIC;
				}
			}
		}
		inst->flags_cache = flags;

		GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;

		float lod_distance = 0.0;

		if (p_render_data->scene_data->cam_orthogonal) {
			lod_distance = 1.0;
		} else {
			Vector3 aabb_min = inst->transformed_aabb.position;
			Vector3 aabb_max = inst->transformed_aabb.position + inst->transformed_aabb.size;
			Vector3 camera_position = p_render_data->scene_data->main_cam_transform.origin;
			Vector3 surface_distance = Vector3(0.0, 0.0, 0.0).max(aabb_min - camera_position).max(camera_position - aabb_max);

			lod_distance = surface_distance.length();
		}

		if (unlikely(inst->transform_status != GeometryInstanceFlux::TransformStatus::NONE && frame > inst->prev_transform_change_frame && inst->prev_transform_change_frame)) {
			inst->prev_transform = inst->transform;
			inst->transform_status = GeometryInstanceFlux::TransformStatus::NONE;
		}

		while (surf) {
			surf->sort.uses_forward_gi = 0;
			surf->sort.uses_lightmap = 0;

			// LOD
			if (p_render_data->scene_data->screen_mesh_lod_threshold > 0.0 && mesh_storage->mesh_surface_has_lod(surf->surface)) {
				uint32_t indices = 0;
				surf->sort.lod_index = mesh_storage->mesh_surface_get_lod(surf->surface, inst->lod_model_scale * inst->lod_bias, lod_distance * p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, indices);
				if (p_render_data->render_info) {
					indices = _indices_to_primitives(surf->primitive, indices);
					if (p_render_list == RENDER_LIST_OPAQUE) { //opaque
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += indices;
					} else if (p_render_list == RENDER_LIST_SECONDARY) { //shadow
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += indices;
					}
				}
			} else {
				surf->sort.lod_index = 0;
				if (p_render_data->render_info) {
					// This does not include primitives rendered via indirect draw calls.
					uint32_t to_draw = mesh_storage->mesh_surface_get_vertices_drawn_count(surf->surface);
					to_draw = _indices_to_primitives(surf->primitive, to_draw);
					to_draw *= inst->instance_count;
					if (p_render_list == RENDER_LIST_OPAQUE) { //opaque
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += to_draw;
					} else if (p_render_list == RENDER_LIST_SECONDARY) { //shadow
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += to_draw;
					}
				}
			}

			// ADD Element
			if (p_pass_mode == PASS_MODE_COLOR) {
#ifdef DEBUG_ENABLED
				bool force_alpha = unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW);
#else
				bool force_alpha = false;
#endif

				if (fade_alpha < FADE_ALPHA_PASS_THRESHOLD) {
					force_alpha = true;
				}

				if (!force_alpha && (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE))) {
					rl->add_element(surf);
				}

				if (force_alpha || (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
					surf->color_pass_inclusion_mask = COLOR_PASS_FLAG_TRANSPARENT;
					render_list[RENDER_LIST_ALPHA].add_element(surf);
					if (uses_gi) {
						surf->sort.uses_forward_gi = 1;
					}
				} else if (p_using_motion_pass && (uses_motion || (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR))) {
					surf->color_pass_inclusion_mask = COLOR_PASS_FLAG_MOTION_VECTORS;
					render_list[RENDER_LIST_MOTION].add_element(surf);
				} else {
					surf->color_pass_inclusion_mask = 0;
				}

				if (uses_lightmap) {
					surf->sort.uses_lightmap = 1;
					scene_state.used_lightmap = true;
				}

				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_SUBSURFACE_SCATTERING) {
					scene_state.used_sss = true;
				}
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_SCREEN_TEXTURE) {
					scene_state.used_screen_texture = true;
				}
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_NORMAL_TEXTURE) {
					scene_state.used_normal_texture = true;
				}
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DEPTH_TEXTURE) {
					scene_state.used_depth_texture = true;
				}
				if ((surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_STENCIL) && !force_alpha && (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE))) {
					scene_state.used_opaque_stencil = true;
				}
			} else if (p_pass_mode == PASS_MODE_SHADOW || p_pass_mode == PASS_MODE_SHADOW_DP) {
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW) {
					rl->add_element(surf);
				}
			} else if (p_pass_mode == PASS_MODE_DEPTH_MATERIAL) {
				if (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE | GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
					rl->add_element(surf);
				}
			} else {
				if (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE)) {
					rl->add_element(surf);
				}
			}

			surf->sort.depth_layer = depth_layer;

			surf = surf->next;
		}
	}

	if (p_render_list == RENDER_LIST_OPAQUE && lightmap_captures_used) {
		RD::get_singleton()->buffer_update(scene_state.lightmap_capture_buffer, 0, sizeof(LightmapCaptureData) * lightmap_captures_used, scene_state.lightmap_captures);
	}
}

void RenderFlux::_setup_voxelgis(const PagedArray<RID> &p_voxelgis) {
	scene_state.voxelgis_used = MIN(p_voxelgis.size(), uint32_t(MAX_VOXEL_GI_INSTANCESS));
	for (uint32_t i = 0; i < scene_state.voxelgis_used; i++) {
		scene_state.voxelgi_ids[i] = p_voxelgis[i];
	}
}

void RenderFlux::_setup_lightmaps(const RenderDataRD *p_render_data, const PagedArray<RID> &p_lightmaps, const Transform3D &p_cam_transform) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	scene_state.lightmaps_used = 0;
	for (int i = 0; i < (int)p_lightmaps.size(); i++) {
		if (i >= (int)scene_state.max_lightmaps) {
			break;
		}

		RID lightmap = light_storage->lightmap_instance_get_lightmap(p_lightmaps[i]);

		// Transform (for directional lightmaps).
		Basis to_lm = light_storage->lightmap_instance_get_transform(p_lightmaps[i]).basis.inverse() * p_cam_transform.basis;
		to_lm = to_lm.inverse().transposed(); //will transform normals
		RendererRD::MaterialStorage::store_transform_3x3(to_lm, scene_state.lightmaps[i].normal_xform);

		// Light texture size.
		Vector2i lightmap_size = light_storage->lightmap_get_light_texture_size(lightmap);
		scene_state.lightmaps[i].texture_size[0] = lightmap_size[0];
		scene_state.lightmaps[i].texture_size[1] = lightmap_size[1];

		// Exposure.
		scene_state.lightmaps[i].exposure_normalization = 1.0;
		scene_state.lightmaps[i].flags = light_storage->lightmap_get_shadowmask_mode(lightmap);
		if (p_render_data->camera_attributes.is_valid()) {
			float baked_exposure = light_storage->lightmap_get_baked_exposure_normalization(lightmap);
			float enf = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
			scene_state.lightmaps[i].exposure_normalization = enf / baked_exposure;
		}

		scene_state.lightmap_ids[i] = p_lightmaps[i];
		scene_state.lightmap_has_sh[i] = light_storage->lightmap_uses_spherical_harmonics(lightmap);

		scene_state.lightmaps_used++;
	}
	if (scene_state.lightmaps_used > 0) {
		RD::get_singleton()->buffer_update(scene_state.lightmap_buffer, 0, sizeof(LightmapData) * scene_state.lightmaps_used, scene_state.lightmaps);
	}
}

/* SDFGI */

void RenderFlux::_update_sdfgi(RenderDataRD *p_render_data) {
	if (p_render_data->sdfgi_update_data == nullptr) {
		return;
	}

	Ref<RenderSceneBuffersRD> rb;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		rb = p_render_data->render_buffers;
	}

	if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
		RENDER_TIMESTAMP("Render SDFGI");
		Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
		float exposure_normalization = 1.0;

		if (p_render_data->camera_attributes.is_valid()) {
			exposure_normalization = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		for (int i = 0; i < p_render_data->render_sdfgi_region_count; i++) {
			sdfgi->render_region(rb, p_render_data->render_sdfgi_regions[i].region, p_render_data->render_sdfgi_regions[i].instances, exposure_normalization);
		}
		if (p_render_data->sdfgi_update_data->update_static) {
			sdfgi->render_static_lights(p_render_data, rb, p_render_data->sdfgi_update_data->static_cascade_count, p_render_data->sdfgi_update_data->static_cascade_indices, p_render_data->sdfgi_update_data->static_positional_lights);
		}
	}
}

/* Debug */

void RenderFlux::_debug_draw_cluster(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers.is_valid() && current_cluster_builder != nullptr) {
		RSE::ViewportDebugDraw dd = get_debug_draw_mode();

		if (dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES) {
			ClusterBuilderRD::ElementType elem_type = ClusterBuilderRD::ELEMENT_TYPE_MAX;
			switch (dd) {
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_OMNI_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_SPOT_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_DECAL;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_REFLECTION_PROBE;
					break;
				default: {
				}
			}
			current_cluster_builder->debug(elem_type);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// FOG SHADER

void RenderFlux::_update_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const Projection &p_cam_projection, const Transform3D &p_cam_transform, const Transform3D &p_prev_cam_inv_transform, RID p_shadow_atlas, int p_directional_light_count, bool p_use_directional_shadows, int p_positional_light_count, int p_voxel_gi_count, const PagedArray<RID> &p_fog_volumes) {
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataFlux> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FLUX);
	ERR_FAIL_COND(rb_data.is_null());

	ERR_FAIL_COND(!p_render_buffers->has_custom_data(RB_SCOPE_GI));
	Ref<RendererRD::GI::RenderBuffersGI> rbgi = p_render_buffers->get_custom_data(RB_SCOPE_GI);

	Ref<RendererRD::GI::SDFGI> sdfgi;
	if (p_render_buffers->has_custom_data(RB_SCOPE_SDFGI)) {
		sdfgi = p_render_buffers->get_custom_data(RB_SCOPE_SDFGI);
	}

	Size2i size = p_render_buffers->get_internal_size();
	float ratio = float(size.x) / float((size.x + size.y) / 2);
	uint32_t target_width = uint32_t(float(get_volumetric_fog_size()) * ratio);
	uint32_t target_height = uint32_t(float(get_volumetric_fog_size()) / ratio);

	if (p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_buffers->get_custom_data(RB_SCOPE_FOG);
		//validate
		if (p_environment.is_null() || !environment_get_volumetric_fog_enabled(p_environment) || fog->width != target_width || fog->height != target_height || fog->depth != get_volumetric_fog_depth()) {
			p_render_buffers->set_custom_data(RB_SCOPE_FOG, Ref<RenderBufferCustomDataRD>());
		}
	}

	if (p_environment.is_null() || !environment_get_volumetric_fog_enabled(p_environment)) {
		//no reason to enable or update, bye
		return;
	}

	if (p_environment.is_valid() && environment_get_volumetric_fog_enabled(p_environment) && !p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		//required volumetric fog but not existing, create
		Ref<RendererRD::Fog::VolumetricFog> fog;

		fog.instantiate();
		fog->init(Vector3i(target_width, target_height, get_volumetric_fog_depth()), sky.sky_shader.default_shader_rd);

		p_render_buffers->set_custom_data(RB_SCOPE_FOG, fog);
	}

	if (p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_buffers->get_custom_data(RB_SCOPE_FOG);

		RendererRD::Fog::VolumetricFogSettings settings;
		settings.rb_size = size;
		settings.time = time;
		settings.is_using_radiance_octmap_array = is_using_radiance_octmap_array();
		settings.max_cluster_elements = RendererRD::LightStorage::get_singleton()->get_max_cluster_elements();
		settings.volumetric_fog_filter_active = get_volumetric_fog_filter_active();

		settings.shadow_sampler = shadow_sampler;
		settings.shadow_atlas_depth = RendererRD::LightStorage::get_singleton()->owns_shadow_atlas(p_shadow_atlas) ? RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_shadow_atlas) : RID();
		settings.voxel_gi_buffer = rbgi->get_voxel_gi_buffer();
		settings.omni_light_buffer = RendererRD::LightStorage::get_singleton()->get_omni_light_buffer();
		settings.spot_light_buffer = RendererRD::LightStorage::get_singleton()->get_spot_light_buffer();
		settings.area_light_buffer = RendererRD::LightStorage::get_singleton()->get_area_light_buffer();
		settings.area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
		settings.directional_shadow_depth = RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture();
		settings.directional_light_buffer = RendererRD::LightStorage::get_singleton()->get_directional_light_buffer();

		settings.vfog = fog;
		settings.cluster_builder = rb_data->cluster_builder;
		settings.rbgi = rbgi;
		settings.sdfgi = sdfgi;
		settings.env = p_environment;
		settings.sky = &sky;
		settings.gi = &gi;

		RendererRD::Fog::get_singleton()->volumetric_fog_update(settings, p_cam_projection, p_cam_transform, p_prev_cam_inv_transform, p_shadow_atlas, p_directional_light_count, p_use_directional_shadows, p_positional_light_count, p_voxel_gi_count, p_fog_volumes);
	}
}

/* Lighting */

void RenderFlux::setup_added_reflection_probe(const Transform3D &p_transform, const Vector3 &p_half_size) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_REFLECTION_PROBE, p_transform, p_half_size);
	}
}

void RenderFlux::setup_added_light(const RSE::LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size) {
	if (current_cluster_builder != nullptr) {
		ClusterBuilderRD::LightType type;
		if (p_type == RSE::LIGHT_SPOT) {
			type = ClusterBuilderRD::LIGHT_TYPE_SPOT;
		} else if (p_type == RSE::LIGHT_OMNI) {
			type = ClusterBuilderRD::LIGHT_TYPE_OMNI;
		} else {
			type = ClusterBuilderRD::LIGHT_TYPE_AREA;
		}

		current_cluster_builder->add_light(type, p_transform, p_radius, p_spot_aperture, p_area_size);
	}
}

void RenderFlux::setup_added_decal(const Transform3D &p_transform, const Vector3 &p_half_size) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_DECAL, p_transform, p_half_size);
	}
}

/* Render scene */

void RenderFlux::_process_ssao(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_environment.is_null());

	Ref<RenderBufferDataFlux> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FLUX);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSAO");

	RendererRD::SSEffects::SSAOSettings settings;
	settings.radius = environment_get_ssao_radius(p_environment);
	settings.intensity = environment_get_ssao_intensity(p_environment);
	settings.power = environment_get_ssao_power(p_environment);
	settings.detail = environment_get_ssao_detail(p_environment);
	settings.horizon = environment_get_ssao_horizon(p_environment);
	settings.sharpness = environment_get_ssao_sharpness(p_environment);
	settings.full_screen_size = p_render_buffers->get_internal_size();

	ss_effects->ssao_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssao, settings);

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		ss_effects->generate_ssao(p_render_buffers, rb_data->ss_effects_data.ssao, v, p_normal_buffers[v], p_projections[v], settings);
	}
}

void RenderFlux::_process_ssil(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections, const Transform3D &p_transform) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_environment.is_null());

	Ref<RenderBufferDataFlux> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FLUX);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSIL");

	RendererRD::SSEffects::SSILSettings settings;
	settings.radius = environment_get_ssil_radius(p_environment);
	settings.intensity = environment_get_ssil_intensity(p_environment);
	settings.sharpness = environment_get_ssil_sharpness(p_environment);
	settings.normal_rejection = environment_get_ssil_normal_rejection(p_environment);
	settings.full_screen_size = p_render_buffers->get_internal_size();

	ss_effects->ssil_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssil, settings);

	Transform3D transform = p_transform;
	transform.set_origin(Vector3(0.0, 0.0, 0.0));

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		Projection correction;
		correction.set_depth_correction(true);
		Projection projection = correction * p_projections[v];
		Projection last_frame_projection = rb_data->ss_effects_data.ssil_last_frame_projections[v] * Projection(rb_data->ss_effects_data.ssil_last_frame_transform.affine_inverse()) * Projection(transform) * projection.inverse();

		ss_effects->screen_space_indirect_lighting(p_render_buffers, rb_data->ss_effects_data.ssil, v, p_normal_buffers[v], p_projections[v], last_frame_projection, settings);

		rb_data->ss_effects_data.ssil_last_frame_projections[v] = projection;
	}
	rb_data->ss_effects_data.ssil_last_frame_transform = transform;
}

void RenderFlux::_process_ssr(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_slices, const Projection *p_projections, const Vector3 *p_eye_offsets, const Transform3D &p_transform) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataFlux> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FLUX);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSR");

	ss_effects->ssr_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssr, p_render_buffers->get_base_data_format());

	Projection reprojections[RendererSceneRender::MAX_RENDER_VIEWS];

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		Projection correction;
		correction.set_depth_correction(true);

		Projection projection = correction * p_projections[v];

		reprojections[v] = rb_data->ss_effects_data.ssr_last_frame_projections[v] * Projection(rb_data->ss_effects_data.ssr_last_frame_transform.affine_inverse()) * Projection(p_transform) * projection.inverse();

		rb_data->ss_effects_data.ssr_last_frame_projections[v] = projection;
	}
	rb_data->ss_effects_data.ssr_last_frame_transform = p_transform;

	ss_effects->screen_space_reflection(p_render_buffers, rb_data->ss_effects_data.ssr, p_normal_slices, environment_get_ssr_max_steps(p_environment), environment_get_ssr_fade_in(p_environment), environment_get_ssr_fade_out(p_environment), environment_get_ssr_depth_tolerance(p_environment), p_projections, reprojections, p_eye_offsets, *copy_effects);
}

void RenderFlux::_copy_framebuffer_to_ss_effects(Ref<RenderSceneBuffersRD> p_render_buffers, bool p_use_ssil, bool p_use_ssr) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	ss_effects->copy_internal_texture_to_last_frame(p_render_buffers, *copy_effects);
}

void RenderFlux::_pre_opaque_render(RenderDataRD *p_render_data, bool p_use_ssao, bool p_use_ssil, bool p_use_ssr, bool p_use_gi, bool p_render_raster_shadows, const RID *p_normal_roughness_slices, RID p_voxel_gi_buffer) {
	// Render shadows while GI is rendering, due to how barriers are handled, this should happen at the same time
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	Ref<RenderBufferDataFlux> rb_data;
	if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_FLUX)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FLUX);
	}

	RENDER_TIMESTAMP("Setup Shadows");

	if (rb.is_valid() && p_use_gi && rb->has_custom_data(RB_SCOPE_SDFGI)) {
		Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
		sdfgi->store_probes();
	}

	Size2i viewport_size = Size2i(1, 1);
	if (rb.is_valid()) {
		viewport_size = rb->get_internal_size();
	}

	p_render_data->cube_shadows.clear();
	p_render_data->shadows.clear();
	p_render_data->directional_shadows.clear();

	float lod_distance_multiplier = p_render_data->scene_data->cam_projection.get_lod_multiplier();
	{
		const int raster_shadow_count = p_render_raster_shadows ? p_render_data->render_shadow_count : 0;
		for (int i = 0; i < raster_shadow_count; i++) {
			RID li = p_render_data->render_shadows[i].light;
			RID base = light_storage->light_instance_get_base_light(li);

			if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
				p_render_data->directional_shadows.push_back(i);
			} else if (light_storage->light_get_type(base) == RSE::LIGHT_OMNI && light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
				p_render_data->cube_shadows.push_back(i);
			} else {
				p_render_data->shadows.push_back(i);
			}
		}

		if (p_render_data->cube_shadows.size()) {
			RENDER_TIMESTAMP("Render OmniLight Shadows");
			// Cube shadows are rendered in their own way.
			for (const int &index : p_render_data->cube_shadows) {
				_render_shadow_pass(p_render_data->render_shadows[index].light, p_render_data->shadow_atlas, p_render_data->render_shadows[index].pass, p_render_data->render_shadows[index].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, true, true, true, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform);
			}
		}

		if (p_render_data->directional_shadows.size()) {
			//open the pass for directional shadows
			light_storage->update_directional_shadow_atlas();
			RD::get_singleton()->draw_list_begin(light_storage->direction_shadow_get_fb(), RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
			RD::get_singleton()->draw_list_end();
		}
	}

	// Render GI

	bool render_shadows = p_render_data->directional_shadows.size() || p_render_data->shadows.size();
	bool render_gi = rb.is_valid() && p_use_gi;

	if (render_shadows && render_gi) {
		RENDER_TIMESTAMP("Render GI + Render Directional/SpotLight Shadows (Parallel)");
	} else if (render_shadows) {
		RENDER_TIMESTAMP("Render Directional/SpotLight Shadows");
	} else if (render_gi) {
		RENDER_TIMESTAMP("Render GI");
	}

	//prepare shadow rendering
	if (render_shadows) {
		_render_shadow_begin();

		//render directional shadows
		for (uint32_t i = 0; i < p_render_data->directional_shadows.size(); i++) {
			_render_shadow_pass(p_render_data->render_shadows[p_render_data->directional_shadows[i]].light, p_render_data->shadow_atlas, p_render_data->render_shadows[p_render_data->directional_shadows[i]].pass, p_render_data->render_shadows[p_render_data->directional_shadows[i]].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, false, i == p_render_data->directional_shadows.size() - 1, false, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform);
		}
		//render positional shadows
		for (uint32_t i = 0; i < p_render_data->shadows.size(); i++) {
			_render_shadow_pass(p_render_data->render_shadows[p_render_data->shadows[i]].light, p_render_data->shadow_atlas, p_render_data->render_shadows[p_render_data->shadows[i]].pass, p_render_data->render_shadows[p_render_data->shadows[i]].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, i == 0, i == p_render_data->shadows.size() - 1, true, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform);
		}

		_render_shadow_process();
	}

	if (render_gi) {
		gi.process_gi(rb, p_normal_roughness_slices, p_voxel_gi_buffer, p_render_data->environment, p_render_data->scene_data->view_count, p_render_data->scene_data->view_projection, p_render_data->scene_data->view_eye_offset, p_render_data->scene_data->cam_transform, *p_render_data->voxel_gi_instances);
	}

	if (render_shadows) {
		_render_shadow_end();
	}

	if (rb_data.is_valid() && ss_effects) {
		// Note, in multiview we're allocating buffers for each eye/view we're rendering.
		// This should allow most of the processing to happen in parallel even if we're doing
		// drawcalls per eye/view. It will all sync up at the barrier.

		if (p_use_ssil || p_use_ssr) {
			ss_effects->allocate_last_frame_buffer(rb, p_use_ssil, p_use_ssr);
		}

		if (p_use_ssao || p_use_ssil) {
			RENDER_TIMESTAMP("Prepare Depth for SSAO/SSIL");
			// Convert our depth buffer data to linear data in
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				ss_effects->downsample_depth(rb, v, p_render_data->scene_data->view_projection[v]);
			}

			if (p_use_ssao) {
				_process_ssao(rb, p_render_data->environment, p_normal_roughness_slices, p_render_data->scene_data->view_projection);
			}

			if (p_use_ssil) {
				_process_ssil(rb, p_render_data->environment, p_normal_roughness_slices, p_render_data->scene_data->view_projection, p_render_data->scene_data->cam_transform);
			}
		}

		if (p_use_ssr) {
			_process_ssr(rb, p_render_data->environment, p_normal_roughness_slices, p_render_data->scene_data->view_projection, p_render_data->scene_data->view_eye_offset, p_render_data->scene_data->cam_transform);
		}
	}

	RENDER_TIMESTAMP("Pre Opaque Render");

	if (current_cluster_builder) {
		// Note: when rendering stereoscopic (multiview) we are using our combined frustum projection to create
		// our cluster data. We use reprojection in the shader to adjust for our left/right eye.
		// This only works as we don't filter our cluster by depth buffer.
		// If we ever make this optimization we should make it optional and only use it in mono.
		// What we win by filtering out a few lights, we loose by having to do the work double for stereo.
		current_cluster_builder->begin(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, !p_render_data->reflection_probe.is_valid());
	}

	bool using_shadows = true;

	if (p_render_data->reflection_probe.is_valid()) {
		if (!RSG::light_storage->reflection_probe_renders_shadows(light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe))) {
			using_shadows = false;
		}
	} else {
		//do not render reflections when rendering a reflection probe
		light_storage->update_reflection_probe_buffer(p_render_data, *p_render_data->reflection_probes, p_render_data->scene_data->cam_transform.affine_inverse(), p_render_data->environment);
	}

	uint32_t directional_light_count = 0;
	uint32_t positional_light_count = 0;
	light_storage->update_light_buffers(p_render_data, *p_render_data->lights, p_render_data->scene_data->cam_transform, p_render_data->shadow_atlas, using_shadows, directional_light_count, positional_light_count, p_render_data->directional_light_soft_shadows);
	texture_storage->update_decal_buffer(*p_render_data->decals, p_render_data->scene_data->cam_transform);

	p_render_data->directional_light_count = directional_light_count;

	if (current_cluster_builder) {
		current_cluster_builder->bake_cluster();
	}

	if (rb_data.is_valid()) {
		RENDER_TIMESTAMP("Update Volumetric Fog");
		bool directional_shadows = RendererRD::LightStorage::get_singleton()->has_directional_shadows(directional_light_count);
		_update_volumetric_fog(rb, p_render_data->environment, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform, p_render_data->scene_data->prev_cam_transform.affine_inverse(), p_render_data->shadow_atlas, directional_light_count, directional_shadows, positional_light_count, p_render_data->voxel_gi_count, *p_render_data->fog_volumes);
	}
}

void RenderFlux::_process_sss(Ref<RenderSceneBuffersRD> p_render_buffers, const Projection &p_camera) {
	ERR_FAIL_COND(p_render_buffers.is_null());

	Size2i internal_size = p_render_buffers->get_internal_size();
	bool can_use_effects = internal_size.x >= 8 && internal_size.y >= 8;

	if (!can_use_effects) {
		//just copy
		return;
	}

	p_render_buffers->allocate_blur_textures();

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		RID internal_texture = p_render_buffers->get_internal_texture(v);
		RID depth_texture = p_render_buffers->get_depth_texture(v);
		ss_effects->sub_surface_scattering(p_render_buffers, internal_texture, depth_texture, p_camera, internal_size);
	}
}

#ifdef METAL_ENABLED
bool RenderFlux::_process_metal_flux(RenderDataRD *p_render_data, const Ref<RenderBufferDataFlux> &p_render_buffer_data, bool p_shadow_only) {
	ERR_FAIL_NULL_V(metal_flux_effect, false);
	ERR_FAIL_COND_V(p_render_data->render_buffers.is_null() || p_render_buffer_data.is_null(), false);
	const uint64_t current_diagnostics_frame = p_render_data->render_info ? p_render_data->render_info->flux_current_frame : 0;
	auto pending_capture_count = [this](uint64_t p_owner) -> uint32_t {
		if (const Vector<RenderingServerTypes::FluxDiagnosticsPendingState> *pending_frames = metal_flux_pending_diagnostics.getptr(p_owner)) {
			return pending_frames->size();
		}
		return 0;
	};
	auto retain_completed_snapshot = [this](uint64_t p_owner, RenderingServerTypes::FluxDiagnostics &r_diagnostics) {
		if (r_diagnostics.timings_valid) {
			metal_flux_completed_timing_diagnostics.insert(p_owner, r_diagnostics);
		}
		// A Flux timing belongs only to its submitted diagnostics frame.  Carrying
		// a completed sample into a newer snapshot made queued wall pacing look
		// like current GPU work, which is especially misleading while Metal is
		// backlogged.
		metal_flux_completed_diagnostics.insert(p_owner, r_diagnostics);
	};
	auto timing_capture_state_for_owner = [this](uint64_t p_owner) -> MetalFluxTimingCaptureState * {
		if (p_owner == 0) {
			return nullptr;
		}
		MetalFluxTimingCaptureState *state = metal_flux_timing_capture_states.getptr(p_owner);
		if (!state) {
			if (metal_flux_timing_capture_states.size() >= 64) {
				uint64_t owner_to_evict = 0;
				for (const KeyValue<uint64_t, MetalFluxTimingCaptureState> &entry : metal_flux_timing_capture_states) {
					owner_to_evict = entry.key;
					break;
				}
				metal_flux_timing_capture_states.erase(owner_to_evict);
			}
			metal_flux_timing_capture_states.insert(p_owner, MetalFluxTimingCaptureState());
			state = metal_flux_timing_capture_states.getptr(p_owner);
		}
		return state;
	};
	auto publish_completed_for_current_owner = [&]() -> bool {
		if (!p_render_data->render_info) {
			return false;
		}
		const uint64_t owner = p_render_data->render_info->flux_owner_id;
		if (const RenderingServerTypes::FluxDiagnostics *completed = metal_flux_completed_diagnostics.getptr(owner)) {
			RenderingServerTypes::FluxDiagnostics observed = *completed;
			observed.set_completion_observation(current_diagnostics_frame, pending_capture_count(owner));
			metal_flux_completed_diagnostics.insert(owner, observed);
			p_render_data->render_info->flux = observed;
			return true;
		}
		return false;
	};
	const bool collect_gpu_timings = GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/diagnostics/collect_gpu_timings");
	if (collect_gpu_timings) {
		const bool report_gpu_timings = !metal_flux_timing_reported;
		Vector<RendererRD::MetalFluxEffect::StageTiming> timings;
		if (metal_flux_effect->collect_completed_timings(timings) == OK) {
			String report = "Flux GPU stages:";
			Vector<Pair<uint64_t, uint64_t>> completed_snapshots;
			for (const RendererRD::MetalFluxEffect::StageTiming &timing : timings) {
				report += vformat(" %s=%.3f ms", timing.stage, timing.milliseconds);
				RenderingServerTypes::FluxDiagnosticsPendingState *pending = nullptr;
				if (Vector<RenderingServerTypes::FluxDiagnosticsPendingState> *pending_frames = metal_flux_pending_diagnostics.getptr(timing.diagnostics_owner_id)) {
					for (RenderingServerTypes::FluxDiagnosticsPendingState &candidate : *pending_frames) {
						if (candidate.diagnostics.frame == timing.diagnostics_frame) {
							pending = &candidate;
							break;
						}
					}
				}
				if (pending) {
					if (timing.stage == StringName("blas")) {
						pending->diagnostics.timings_ms.blas = timing.milliseconds;
					} else if (timing.stage == StringName("tlas")) {
						pending->diagnostics.timings_ms.tlas = timing.milliseconds;
					} else if (timing.stage == StringName("ray_shadows")) {
						pending->diagnostics.timings_ms.ray_shadows = timing.milliseconds;
						pending->diagnostics.raw_shadow_timing_available = true;
					} else if (timing.stage == StringName("ray_effects")) {
						pending->diagnostics.timings_ms.ray_effects = timing.milliseconds;
						pending->diagnostics.timings_valid = true;
						pending->diagnostics.timings_frame = timing.diagnostics_frame;
						pending->timing_ready = true;
						const Pair<uint64_t, uint64_t> completed(timing.diagnostics_owner_id, timing.diagnostics_frame);
						if (!completed_snapshots.has(completed)) {
							completed_snapshots.push_back(completed);
						}
					} else if (timing.stage == StringName("spatial_reconstruction")) {
						pending->diagnostics.timings_ms.spatial = timing.milliseconds;
					} else if (timing.stage == StringName("temporal_reconstruction")) {
						pending->diagnostics.timings_ms.temporal = timing.milliseconds;
					} else if (timing.stage == StringName("composition")) {
						pending->diagnostics.timings_ms.composition = timing.milliseconds;
					} else if (timing.stage == StringName("gpu_timing_unavailable")) {
						// This frame has completed but Metal did not resolve its counter
						// range. Publish it without a stale predecessor and permit a new
						// frame-matched capture on the next render submission.
						pending->diagnostics.timings_valid = false;
						pending->diagnostics.timings_frame = timing.diagnostics_frame;
						pending->timing_ready = true;
						const Pair<uint64_t, uint64_t> completed(timing.diagnostics_owner_id, timing.diagnostics_frame);
						if (!completed_snapshots.has(completed)) {
							completed_snapshots.push_back(completed);
						}
					}
				}
				if (MetalFluxTimingCaptureState *state = metal_flux_timing_capture_states.getptr(timing.diagnostics_owner_id)) {
					if (timing.stage == StringName("ray_shadows")) {
						state->shadow_capture_submitted = false;
						state->shadow_capture_reported = true;
					} else if (timing.stage == StringName("ray_effects") || timing.stage == StringName("gpu_timing_unavailable")) {
						state->effect_capture_submitted = false;
						state->effect_capture_reported = true;
					}
				}
			}
			for (const Pair<uint64_t, uint64_t> &completed : completed_snapshots) {
				if (Vector<RenderingServerTypes::FluxDiagnosticsPendingState> *pending_frames = metal_flux_pending_diagnostics.getptr(completed.first)) {
					for (int frame_index = 0; frame_index < pending_frames->size(); frame_index++) {
						if ((*pending_frames)[frame_index].diagnostics.frame == completed.second && (*pending_frames)[frame_index].ready_to_publish()) {
							RenderingServerTypes::FluxDiagnostics diagnostics = (*pending_frames)[frame_index].diagnostics;
							retain_completed_snapshot(completed.first, diagnostics);
							pending_frames->remove_at(frame_index);
							break;
						}
					}
					if (pending_frames->is_empty()) {
						metal_flux_pending_diagnostics.erase(completed.first);
					}
				}
			}
			if (report_gpu_timings && !timings.is_empty()) {
				print_line(report);
			}
			metal_flux_timing_reported |= !timings.is_empty();
		}
	}
	{
		Vector<RendererRD::MetalFluxEffect::WorkAttribution> attribution_records;
		if (metal_flux_effect->collect_completed_work_attribution(attribution_records) == OK) {
			for (const RendererRD::MetalFluxEffect::WorkAttribution &record : attribution_records) {
				Vector<RenderingServerTypes::FluxDiagnosticsPendingState> *pending_frames = metal_flux_pending_diagnostics.getptr(record.diagnostics_owner_id);
				if (!pending_frames) {
					continue;
				}
				for (int frame_index = 0; frame_index < pending_frames->size(); frame_index++) {
					RenderingServerTypes::FluxDiagnosticsPendingState &pending = pending_frames->write[frame_index];
					if (pending.diagnostics.frame != record.diagnostics_frame) {
						continue;
					}
					RenderingServerTypes::FluxDiagnostics &diagnostics = pending.diagnostics;
					diagnostics.work_attribution_valid = true;
					diagnostics.trace_compaction_active = record.trace_compaction_active;
					diagnostics.trace_compaction_fallback = record.trace_compaction_fallback;
					diagnostics.trace_compaction_active_pixel_count = record.trace_compaction_active_pixels;
					diagnostics.trace_compaction_inactive_pixel_count = record.trace_compaction_inactive_pixels;
					for (uint32_t class_index = 0; class_index < 5; class_index++) {
						diagnostics.trace_compaction_need_counts[class_index] = record.trace_compaction_need[class_index];
						diagnostics.trace_compaction_enqueued_counts[class_index] = record.trace_compaction_enqueued[class_index];
						diagnostics.trace_compaction_dispatched_counts[class_index] = record.trace_compaction_dispatched[class_index];
					}
					diagnostics.invalid_pdf_sample_count = record.invalid_pdf_samples;
					diagnostics.nonfinite_lobe_sample_count = record.nonfinite_lobe_samples;
					diagnostics.rejected_energy_sample_count = record.rejected_energy_samples;
					diagnostics.primary_valid_pixel_count = record.primary_valid_pixels;
					diagnostics.primary_invalid_pixel_count = record.primary_invalid_pixels;
					diagnostics.primary_lit_pixel_count = record.primary_lit_pixels;
					diagnostics.alpha_candidate_count = record.alpha_candidates;
					diagnostics.alpha_rejection_count = record.alpha_rejections;
					diagnostics.alpha_candidate_exhaustion_count = record.alpha_candidate_exhaustions;
					diagnostics.alpha_mixed_intersection_count = record.alpha_mixed_intersections;
					for (uint32_t ray_class = 0; ray_class < 4; ray_class++) {
						diagnostics.alpha_split_opaque_query_counts[ray_class] = record.alpha_split_opaque_queries[ray_class];
						diagnostics.alpha_split_opaque_hit_counts[ray_class] = record.alpha_split_opaque_hits[ray_class];
						diagnostics.alpha_split_alpha_query_counts[ray_class] = record.alpha_split_alpha_queries[ray_class];
						diagnostics.alpha_split_alpha_hit_counts[ray_class] = record.alpha_split_alpha_hits[ray_class];
						diagnostics.alpha_split_alpha_rejection_counts[ray_class] = record.alpha_split_alpha_rejections[ray_class];
						diagnostics.alpha_split_mixed_fallback_counts[ray_class] = record.alpha_split_mixed_fallbacks[ray_class];
					}
					diagnostics.alpha_rear_opaque_hit_count = record.alpha_rear_opaque_hits;
					diagnostics.alpha_max_candidates_per_ray = record.alpha_max_candidates_per_ray;
					diagnostics.alpha_primary = { record.alpha_primary_candidates, record.alpha_primary_rejections };
					diagnostics.alpha_visibility = { record.alpha_visibility_candidates, record.alpha_visibility_rejections };
					diagnostics.alpha_reflection = { record.alpha_reflection_candidates, record.alpha_reflection_rejections };
					diagnostics.alpha_indirect = { record.alpha_indirect_candidates, record.alpha_indirect_rejections };
					diagnostics.alpha_occupancy_empty_rejection_count = record.alpha_occupancy_empty_rejections;
					diagnostics.alpha_occupancy_opaque_accept_count = record.alpha_occupancy_opaque_accepts;
					diagnostics.alpha_occupancy_mixed_sample_count = record.alpha_occupancy_mixed_samples;
					diagnostics.primary_analytic_selected_count = record.primary_analytic_selected;
					diagnostics.primary_analytic_contributed_count = record.primary_analytic_contributed;
						diagnostics.primary_analytic_visibility_test_count = record.primary_analytic_visibility_tests;
						diagnostics.reusable_path_cache_staged_count = record.reusable_path_staged;
						diagnostics.reusable_path_cache_update_count = record.reusable_path_updates;
						diagnostics.reusable_path_cache_query_count = record.reusable_path_queries;
						diagnostics.reusable_path_cache_valid_candidate_count = record.reusable_path_valid_candidates;
						diagnostics.reusable_path_cache_reused_candidate_count = record.reusable_path_reused_candidates;
						diagnostics.reusable_path_cache_rejection_count = record.reusable_path_rejections;
						diagnostics.reusable_path_cache_occupied_cell_count = record.reusable_path_occupied;
						diagnostics.reusable_path_cache_occupancy_valid = record.reusable_path_occupied > 0;
						diagnostics.reusable_path_cache_invalid_record_count = record.reusable_path_invalid_record;
						diagnostics.reusable_path_cache_endpoint_blocked_count = record.reusable_path_endpoint_blocked;
						diagnostics.reusable_path_cache_shading_invalid_count = record.reusable_path_shading_invalid;
						diagnostics.reusable_path_cache_zero_target_count = record.reusable_path_zero_target;
						diagnostics.reusable_path_cache_invalid_weight_count = record.reusable_path_invalid_weight;
						diagnostics.reusable_path_cache_considered_count = record.reusable_path_considered;
						diagnostics.reusable_path_cache_accepted_count = record.reusable_path_accepted;
						diagnostics.reusable_path_cache_selected_count = record.reusable_path_selected;
						diagnostics.reusable_path_cache_reevaluation_count = record.reusable_path_reevaluations;
						diagnostics.reusable_path_cache_reconnection_visibility_count = record.reusable_path_reconnection_visibility;
						diagnostics.restir_gi_current_candidate_count = record.restir_gi_current_candidates;
						diagnostics.restir_gi_reused_candidate_count = record.restir_gi_reused_candidates;
						diagnostics.restir_gi_selected_reuse_count = record.restir_gi_selected_reuse;
						diagnostics.bidirectional_caustic_candidate_count = record.bidirectional_caustic_candidates;
						diagnostics.bidirectional_caustic_valid_count = record.bidirectional_caustic_valid;
						diagnostics.bidirectional_caustic_contributed_count = record.bidirectional_caustic_contributed;
						diagnostics.bidirectional_caustic_visibility_ray_count = record.bidirectional_caustic_visibility_rays;
						diagnostics.bidirectional_caustic_rejection_count = record.bidirectional_caustic_rejections;
						diagnostics.bidirectional_caustic_nonfinite_or_pdf_failure_count = record.bidirectional_caustic_nonfinite_or_pdf_failures;
						diagnostics.reusable_path_cache_lighting_reevaluation_count = record.reusable_path_lighting_reevaluations;
						diagnostics.reusable_path_cache_environment_reevaluation_count = record.reusable_path_environment_reevaluations;
						diagnostics.direct_reservoir_candidate_count = record.direct_candidate_evaluations;
						diagnostics.direct_reservoir_visibility_test_count = record.direct_selected_visibility;
						diagnostics.direct_reservoir_temporal_reuse_count = record.direct_temporal_reuse;
						diagnostics.direct_reservoir_spatial_reuse_count = record.direct_spatial_reuse;
						diagnostics.direct_reservoir_valid = record.direct_candidate_evaluations > 0;
						diagnostics.light_revision_completed = record.light_revision_completed;
						diagnostics.gi_fresh_ray_count = record.gi_fresh_rays;
						diagnostics.reflection_ray_count = record.reflection_rays;
						diagnostics.gi_converged_skip_count = record.gi_converged_skips;
						diagnostics.reflection_converged_skip_count = record.reflection_converged_skips;
					diagnostics.apply_work_attribution_transport_validation();
					pending.work_attribution_ready = true;
					if (pending.ready_to_publish()) {
						retain_completed_snapshot(record.diagnostics_owner_id, diagnostics);
						pending_frames->remove_at(frame_index);
					}
					break;
				}
				if (pending_frames->is_empty()) {
					metal_flux_pending_diagnostics.erase(record.diagnostics_owner_id);
				}
			}
		}
	}
	publish_completed_for_current_owner();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::MetalFluxEffect::FrameRequest request;
	bool collect_stage_probe = false;
	bool force_stbn_disabled = false;
	bool force_stbn_enabled = false;
	bool force_restir_di_disabled = false;
	bool force_restir_di_enabled = false;
	bool force_regir_disabled = false;
	bool force_regir_enabled = false;
	bool force_reusable_path_disabled = false;
	bool force_reusable_path_enabled = false;
	bool force_unified_finite_light_enabled = false;
	bool force_bidirectional_caustics_disabled = false;
	bool force_reconstruction_disabled = false;
	bool force_fresh_ray_oracle = false;
	float stage_probe_light_scale = 1.0f;
	for (const String &argument : OS::get_singleton()->get_cmdline_user_args()) {
		if (argument == "--flux-stage-probe") {
			collect_stage_probe = true;
		} else if (argument.begins_with("--flux-stage-probe-energy-scale=")) {
			stage_probe_light_scale = MAX(0.0f, argument.trim_prefix("--flux-stage-probe-energy-scale=").to_float());
		} else if (argument == "--flux-stbn-disabled") {
			force_stbn_disabled = true;
		} else if (argument == "--flux-stbn-enabled") {
			force_stbn_enabled = true;
		} else if (argument == "--flux-restir-di-disabled") {
			force_restir_di_disabled = true;
		} else if (argument == "--flux-restir-di-enabled") {
			force_restir_di_enabled = true;
		} else if (argument == "--flux-regir-disabled") {
			force_regir_disabled = true;
		} else if (argument == "--flux-regir-enabled") {
			force_regir_enabled = true;
		} else if (argument == "--flux-reusable-path-disabled") {
			force_reusable_path_disabled = true;
		} else if (argument == "--flux-reusable-path-enabled") {
			force_reusable_path_enabled = true;
		} else if (argument == "--flux-unified-finite-light-enabled") {
			force_unified_finite_light_enabled = true;
		} else if (argument == "--flux-reconstruction-disabled") {
			force_reconstruction_disabled = true;
		} else if (argument == "--flux-fresh-ray-oracle") {
			force_fresh_ray_oracle = true;
		} else if (argument == "--flux-caustic-disabled") {
			// Validation-only startup override. It is intentionally resolved before
			// request construction so a fresh-process A/B cannot inherit a cached
			// ProjectSettings value from the feature-on leg.
			force_bidirectional_caustics_disabled = true;
		}
	}
	if (p_render_data->render_info) {
		request.diagnostics_owner_id = p_render_data->render_info->flux_owner_id;
		request.diagnostics_frame = p_render_data->render_info->flux_current_frame;
	}
	request.maximum_blas_builds_per_frame = CLAMP(uint32_t(int(GLOBAL_GET("rendering/flux/ray_tracing/residency/max_blas_builds_per_frame"))), 1u, 1024u);
	request.fresh_ray_oracle = force_fresh_ray_oracle;
	request.maximum_blas_build_triangles_per_frame = CLAMP(uint64_t(int64_t(GLOBAL_GET("rendering/flux/ray_tracing/residency/max_blas_build_triangles_per_frame"))), uint64_t(1000), uint64_t(100000000));
	if (p_render_data->render_info && p_render_data->render_info->flux_preview_blas_build_limit > 0 && p_render_data->render_info->flux_preview_blas_triangle_limit > 0) {
		// The editor owns this temporary, per-viewport request. It cannot lower a
		// project limit, cannot survive a disabled preview, and is cleared by
		// RendererViewport only after complete-residency diagnostics.
		request.maximum_blas_builds_per_frame = MAX(request.maximum_blas_builds_per_frame, p_render_data->render_info->flux_preview_blas_build_limit);
		request.maximum_blas_build_triangles_per_frame = MAX(request.maximum_blas_build_triangles_per_frame, p_render_data->render_info->flux_preview_blas_triangle_limit);
		request.preview_admission_active = true;
	}
	const float ray_lod_near_field_distance = MAX(0.0f, float(GLOBAL_GET("rendering/flux/ray_tracing/geometry_lod/near_field_distance")));
	request.bounded_transport = p_render_data->hybrid_transport_bounded;
	request.transport_max_distance = p_render_data->hybrid_transport_max_distance;
	request.transport_primary_geometry_count = p_render_data->hybrid_transport_primary_geometry_count;
	request.transport_selected_geometry_count = p_render_data->hybrid_transport_selected_geometry_count;
	request.transport_eligible_geometry_count = p_render_data->hybrid_transport_eligible_geometry_count;
	request.transport_selected_light_count = p_render_data->hybrid_transport_selected_light_count;
	request.transport_eligible_light_count = p_render_data->hybrid_transport_eligible_light_count;
	request.ray_proxy_source_count = p_render_data->hybrid_ray_proxy_source_count;
	request.ray_proxy_substituted_count = p_render_data->hybrid_ray_proxy_substituted_count;
	request.ray_proxy_fail_open_count = p_render_data->hybrid_ray_proxy_fail_open_count;
	request.ray_proxy_duplicate_count = p_render_data->hybrid_ray_proxy_duplicate_count;
	for (uint32_t reason = 0; reason < RendererPathTracing::RAY_PROXY_RELATION_MAX; reason++) {
		request.ray_proxy_rejection_counts[reason] = p_render_data->hybrid_ray_proxy_rejection_counts[reason];
	}
	request.transport_state = p_render_data->hybrid_transport_bounded ? RendererPathTracing::TRANSPORT_CULLING_BOUNDED : (p_render_data->hybrid_transport_fail_open ? RendererPathTracing::TRANSPORT_CULLING_FAIL_OPEN : RendererPathTracing::TRANSPORT_CULLING_DISABLED);
	request.transport_reason = p_render_data->hybrid_transport_reason;
	request.environment.portals = p_render_data->hybrid_environment_portals;
	request.environment.portal_generation = p_render_data->hybrid_environment_portal_generation;
	HashMap<uint64_t, uint32_t> added_surfaces;
	HashMap<uint64_t, _MetalHybridShaderClassification> shader_classifications;
	HashMap<uint64_t, RendererRD::MetalFluxEffect::Instance> material_templates;
	bool shadow_scene_complete = true;
	bool shadow_scene_uses_high_layers = false;
	// The acceleration-structure list is the conservative transport superset;
	// disabled or fail-open culling deliberately restores every eligible mesh.
	const PagedArray<RenderGeometryInstance *> *hybrid_instances = p_render_data->hybrid_instances;
	if (!hybrid_instances && (!p_render_data->virtual_geometry_instances || p_render_data->virtual_geometry_instances->is_empty())) {
		return false;
	}
	const uint32_t hybrid_instance_count = hybrid_instances ? hybrid_instances->size() : 0;
	for (uint32_t instance_index = 0; instance_index < hybrid_instance_count; instance_index++) {
		GeometryInstanceFlux *instance = static_cast<GeometryInstanceFlux *>((*hybrid_instances)[instance_index]);
		if (!instance || instance->data->base_type != RSE::INSTANCE_MESH) {
			continue;
		}
		// Culling validates proxy topology, static/opaque equivalence, transform,
		// and material identity before the hidden proxy reaches this list. Geometry
		// buffers remain proxy-owned; source identity keeps temporal/reservoir and
		// material ownership stable across a proxy substitution.
		GeometryInstanceFlux *ray_source = static_cast<GeometryInstanceFlux *>(instance->get_ray_tracing_source());
		if (ray_source && (!ray_source->data || ray_source->data->base_type != RSE::INSTANCE_MESH)) {
			ray_source = nullptr;
		}
		shadow_scene_uses_high_layers |= ((ray_source ? ray_source->layer_mask : instance->layer_mask) & ~0xffu) != 0;
		for (GeometryInstanceSurfaceDataCache *surface_cache = instance->surface_caches; surface_cache; surface_cache = surface_cache->next) {
			const uint64_t shader_key = uint64_t(uintptr_t(surface_cache->shader));
			const _MetalHybridShaderClassification *classification_ptr = shader_classifications.getptr(shader_key);
			if (!classification_ptr) {
				_MetalHybridShaderClassification classification;
				const String &material_code = surface_cache->shader ? surface_cache->shader->code : String();
				classification.canonical_standard_material = material_code.contains("'s StandardMaterial3D.") || material_code.contains("'s ORMMaterial3D.");
				classification.alpha_scissor_used = material_code.contains("ALPHA_SCISSOR_THRESHOLD") || material_code.contains("ALPHA_SCISSOR_USED") || material_code.contains("alpha_scissor_threshold");
				classification.alpha_hash_used = material_code.contains("ALPHA_HASH_SCALE") || material_code.contains("ALPHA_HASH_USED") || material_code.contains("alpha_hash_scale");
				classification.alpha_antialiasing_used = material_code.contains("ALPHA_ANTIALIASING_EDGE") || material_code.contains("ALPHA_ANTIALIASING_EDGE_USED") || material_code.contains("alpha_antialiasing_edge") || material_code.contains("alpha_to_coverage");
				classification.emission_multiply = !material_code.contains("Emission Operator: Add");
				// Flux currently admits the supported opaque StandardMaterial closure
				// and deliberately does not pretend to implement these extensions.
				// Record their presence explicitly while keeping the material eligible
				// for the existing conservative opaque path.
				classification.clearcoat_used = material_code.contains("clearcoat");
				classification.anisotropy_used = material_code.contains("anisotropy");
				// Thin transmission is an explicit material parameter, not a generated
				// shader-text heuristic. Keeping this false avoids classifying every
				// StandardMaterial3D as unsupported merely because the scalar ABI
				// declaration is shared by all generated shaders.
				classification.transmission_used = false;
				classification.refraction_used = material_code.contains("refraction");
				if (material_code.contains("roughness_texture_channel = vec4(0.0, 1.0")) {
					classification.roughness_texture_channel = Color(0.0, 1.0, 0.0, 0.0);
				} else if (material_code.contains("roughness_texture_channel = vec4(0.0, 0.0, 1.0")) {
					classification.roughness_texture_channel = Color(0.0, 0.0, 1.0, 0.0);
				} else if (material_code.contains("roughness_texture_channel = vec4(0.0, 0.0, 0.0, 1.0")) {
					classification.roughness_texture_channel = Color(0.0, 0.0, 0.0, 1.0);
				} else if (material_code.contains("roughness_texture_channel = vec4(0.333333")) {
					classification.roughness_texture_channel = Color(0.333333, 0.333333, 0.333333, 0.0);
				}
				shader_classifications.insert(shader_key, classification);
				classification_ptr = shader_classifications.getptr(shader_key);
			}
			const _MetalHybridShaderClassification &classification = *classification_ptr;
			if (classification.clearcoat_used) _flux_bounded_feature_count(request.unsupported_clearcoat_materials);
			if (classification.anisotropy_used) _flux_bounded_feature_count(request.unsupported_anisotropy_materials);
			// StandardMaterial3D's historical raster refraction is still outside
			// Flux. Imported glTF thin transmission is carried by explicit scalar
			// material parameters below, not inferred from generated shader text.
			if (classification.refraction_used) _flux_bounded_feature_count(request.unsupported_refraction_materials);
			const bool canonical_standard_material = classification.canonical_standard_material;
			// Arbitrary ShaderMaterial closures cannot be reconstructed by the
			// renderer-owned transport ABI. PrimarySurfaceV1 still supplies a
			// deterministic first-hit material approximation, but the geometry is
			// explicitly unsupported for secondary transport.
			if (!canonical_standard_material) {
				request.unsupported_materials++;
				shadow_scene_complete = false;
				continue;
			}
			const bool alpha_clip_shader = surface_cache->shader && surface_cache->shader->uses_alpha_clip;
			const Variant alpha_cutoff_parameter = alpha_clip_shader && classification.alpha_scissor_used && surface_cache->material_rid.is_valid() ? material_storage->material_get_param(surface_cache->material_rid, SNAME("alpha_scissor_threshold")) : Variant();
			// Only deterministic alpha scissor is part of the opaque ray world. Alpha
			// hash, alpha-to-coverage without a scissor threshold, blended surfaces,
			// and arbitrary discard closures remain explicit transparent overlays.
			const bool strict_alpha_mask = canonical_standard_material && alpha_clip_shader && classification.alpha_scissor_used && !classification.alpha_hash_used && !classification.alpha_antialiasing_used && alpha_cutoff_parameter.is_num();
			const bool opaque_surface = (surface_cache->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE) != 0;
			const bool ray_opaque_surface = surface_cache->shader && surface_cache->shader->uses_alpha_clip ? strict_alpha_mask : opaque_surface;
			if (surface_cache->primitive != RSE::PRIMITIVE_TRIANGLES || !ray_opaque_surface) {
				shadow_scene_complete = false;
				continue;
			}
			const uint32_t surface_index = surface_cache->surface_index;
			void *mesh_surface = surface_cache->surface;
			const uint64_t format = mesh_storage->mesh_surface_get_format(mesh_surface);
			if (format & RSE::ARRAY_FLAG_USE_2D_VERTICES) {
				shadow_scene_complete = false;
				continue;
			}
			const bool dynamic = instance->mesh_instance.is_valid();
			const uint32_t base_index_count = mesh_storage->mesh_surface_get_index_count(mesh_surface);
			uint32_t selected_index_count = base_index_count;
			uint32_t ray_lod = 0;
			RendererPathTracing::RayGeometryLODPolicyInput lod_policy_input;
			lod_policy_input.world_bounds = instance->transformed_aabb;
			lod_policy_input.camera_position = p_render_data->scene_data->main_cam_transform.origin;
			lod_policy_input.near_field_distance = ray_lod_near_field_distance;
			lod_policy_input.generated_lods_available = p_render_data->scene_data->screen_mesh_lod_threshold > 0.0f && mesh_storage->mesh_surface_has_lod(mesh_surface);
			lod_policy_input.deforming = dynamic;
			lod_policy_input.alpha_masked = strict_alpha_mask;
			lod_policy_input.camera_distance_is_meaningful = !p_render_data->scene_data->cam_orthogonal;
			const RendererPathTracing::RayGeometryLODPolicyDecision lod_policy = RendererPathTracing::select_ray_geometry_lod(lod_policy_input);
			float admission_camera_distance = 1.0e30f;
			if (instance->transformed_aabb.is_finite() && instance->transformed_aabb.size.x >= 0.0f && instance->transformed_aabb.size.y >= 0.0f && instance->transformed_aabb.size.z >= 0.0f) {
				const Vector3 closest = p_render_data->scene_data->main_cam_transform.origin.clamp(instance->transformed_aabb.position, instance->transformed_aabb.get_end());
				admission_camera_distance = p_render_data->scene_data->main_cam_transform.origin.distance_to(closest);
			}
			if (lod_policy.use_generated_lod) {
				ray_lod = mesh_storage->mesh_surface_get_lod(mesh_surface, instance->lod_model_scale * instance->lod_bias, lod_policy.camera_distance * p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, selected_index_count);
			}
			switch (lod_policy.reason) {
				case RendererPathTracing::RAY_GEOMETRY_LOD_REASON_DYNAMIC:
					request.ray_lod_base_dynamic_surfaces++;
					break;
				case RendererPathTracing::RAY_GEOMETRY_LOD_REASON_ALPHA_MASK:
					request.ray_lod_base_alpha_mask_surfaces++;
					break;
				case RendererPathTracing::RAY_GEOMETRY_LOD_REASON_NEAR_FIELD:
					request.ray_lod_base_near_field_surfaces++;
					break;
				default:
					break;
			}
			const uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(mesh_surface);
			request.ray_geometry_base_triangles += uint64_t(base_index_count > 0 ? base_index_count : vertex_count) / 3;
			request.ray_geometry_selected_triangles += uint64_t(selected_index_count > 0 ? selected_index_count : vertex_count) / 3;
			request.ray_lod_instance_surfaces += ray_lod > 0 ? 1u : 0u;
			const uint64_t resource_id = dynamic ? instance->mesh_instance.get_id() : instance->data->base.get_id();
			const uint64_t surface_id = resource_id ^ (0x9e3779b97f4a7c15ULL * uint64_t(surface_index + 1)) ^ (ray_lod > 0 ? 0xd6e8feb86659fd93ULL * uint64_t(ray_lod) : 0);
			if (!added_surfaces.has(surface_id)) {
				RendererRD::MetalFluxEffect::Surface surface;
				surface.stable_id = surface_id;
				surface.topology_revision = mesh_storage->mesh_surface_get_content_version(instance->data->base, surface_index) ^ (uint64_t(ray_lod) << 56);
				surface.deformation_revision = dynamic ? mesh_storage->mesh_instance_surface_get_last_change(instance->mesh_instance, surface_index) : surface.topology_revision;
				surface.vertex_buffer = dynamic ? mesh_storage->mesh_instance_surface_get_vertex_buffer_rd_rid(instance->mesh_instance, surface_index) : mesh_storage->mesh_surface_get_vertex_buffer_rd_rid(instance->data->base, surface_index);
				surface.index_buffer = mesh_storage->mesh_surface_get_index_buffer_rd_rid(mesh_surface, ray_lod);
				surface.attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer_rd_rid(instance->data->base, surface_index);
				surface.vertex_count = vertex_count;
				surface.index_count = selected_index_count;
				surface.index_stride = surface.vertex_count <= 65536 ? sizeof(uint16_t) : sizeof(uint32_t);
				surface.dynamic = dynamic;
				surface.compressed = !dynamic && (format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES);
				surface.vertex_stride = surface.compressed ? sizeof(uint16_t) * 4 : sizeof(float) * 3;
				surface.has_normals = format & RSE::ARRAY_FORMAT_NORMAL;
				surface.has_tangents = format & RSE::ARRAY_FORMAT_TANGENT;
				surface.normal_offset = surface.vertex_stride * surface.vertex_count;
				surface.normal_stride = surface.compressed ? sizeof(uint32_t) : sizeof(uint32_t) * ((format & RSE::ARRAY_FORMAT_TANGENT) ? 2 : 1);
				_metal_flux_get_uv0_layout(format, surface.attribute_stride, surface.uv_offset, surface.has_uv);
				if (!surface.has_uv || surface.attribute_buffer.is_null()) {
					surface.has_uv = false;
				}
				surface.compressed_aabb = mesh_storage->mesh_surface_get_aabb(mesh_surface);
				request.surfaces.push_back(surface);
				added_surfaces.insert(surface_id, request.surfaces.size() - 1);
			}
			RendererRD::MetalFluxEffect::Instance hybrid_instance;
				hybrid_instance.stable_id = uint64_t(uintptr_t(ray_source ? ray_source : instance)) ^ uint64_t(surface_index + 1);
				hybrid_instance.dynamic = dynamic;
			hybrid_instance.surface_id = surface_id;
			hybrid_instance.material_stable_id = surface_cache->material_rid.is_valid() ? surface_cache->material_rid.get_id() : hybrid_instance.stable_id;
			hybrid_instance.transform = ray_source ? ray_source->transform * instance->get_ray_tracing_proxy_to_source() : instance->transform;
			hybrid_instance.visibility_mask = (ray_source ? ray_source->layer_mask : instance->layer_mask) & 0xff;
			hybrid_instance.canonical_material = canonical_standard_material;
			hybrid_instance.alpha_mode = strict_alpha_mask ? RendererRD::MetalFluxEffect::Instance::ALPHA_MASK : RendererRD::MetalFluxEffect::Instance::ALPHA_OPAQUE;
			const RSE::CullMode material_cull_mode = surface_cache->shader ? surface_cache->shader->cull_mode : RSE::CULL_MODE_BACK;
			hybrid_instance.face_flags = (material_cull_mode == RSE::CULL_MODE_DISABLED ? 1u : 0u) | (uint32_t(material_cull_mode) << 1u);
			if (surface_cache->material_rid.is_valid()) {
				const uint64_t material_key = surface_cache->material_rid.get_id();
				if (const RendererRD::MetalFluxEffect::Instance *material_template = material_templates.getptr(material_key)) {
					const uint64_t stable_id = hybrid_instance.stable_id;
					const uint64_t surface_stable_id = hybrid_instance.surface_id;
					const Transform3D transform = hybrid_instance.transform;
					const uint32_t visibility_mask = hybrid_instance.visibility_mask;
					hybrid_instance = *material_template;
					hybrid_instance.stable_id = stable_id;
					hybrid_instance.surface_id = surface_stable_id;
					hybrid_instance.transform = transform;
					hybrid_instance.visibility_mask = visibility_mask;
				} else {
				const Variant albedo = material_storage->material_get_param(surface_cache->material_rid, SNAME("albedo"));
				const Variant emission = material_storage->material_get_param(surface_cache->material_rid, SNAME("emission"));
				const Variant emission_energy = material_storage->material_get_param(surface_cache->material_rid, SNAME("emission_energy"));
				const Variant metallic = material_storage->material_get_param(surface_cache->material_rid, SNAME("metallic"));
				const Variant roughness = material_storage->material_get_param(surface_cache->material_rid, SNAME("roughness"));
				const Variant specular = material_storage->material_get_param(surface_cache->material_rid, SNAME("specular"));
				const Variant thin_transmission = material_storage->material_get_param(surface_cache->material_rid, SNAME("thin_transmission"));
				const Variant thin_ior = material_storage->material_get_param(surface_cache->material_rid, SNAME("thin_ior"));
				const Variant thin_transmission_unsupported_features = material_storage->material_get_param(surface_cache->material_rid, SNAME("thin_transmission_unsupported_features"));
				const Variant albedo_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_albedo"));
				const Variant normal_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_normal"));
				const Variant orm_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_orm"));
				const Variant metallic_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_metallic"));
				const Variant roughness_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_roughness"));
				const Variant ao_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_ambient_occlusion"));
				const Variant emission_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_emission"));
				const Variant alpha_occupancy_texture = material_storage->material_get_param(surface_cache->material_rid, SNAME("texture_hybrid_alpha_occupancy"));
				const Variant normal_scale = material_storage->material_get_param(surface_cache->material_rid, SNAME("normal_scale"));
				const Variant uv1_scale = material_storage->material_get_param(surface_cache->material_rid, SNAME("uv1_scale"));
				const Variant uv1_offset = material_storage->material_get_param(surface_cache->material_rid, SNAME("uv1_offset"));
				const Variant metallic_channel = material_storage->material_get_param(surface_cache->material_rid, SNAME("metallic_texture_channel"));
				const Variant ao_channel = material_storage->material_get_param(surface_cache->material_rid, SNAME("ao_texture_channel"));
				if (albedo.get_type() == Variant::COLOR) {
					hybrid_instance.albedo = Color(albedo).srgb_to_linear();
				}
				if (emission.get_type() == Variant::COLOR) {
					hybrid_instance.emission = Color(emission).srgb_to_linear() * (emission_energy.is_num() ? float(emission_energy) : 1.0f);
				}
				hybrid_instance.emission_texture_scale = emission_energy.is_num() ? float(emission_energy) : 1.0f;
				if (metallic.is_num()) {
					hybrid_instance.metallic = metallic;
				}
				if (roughness.is_num()) {
					hybrid_instance.roughness = roughness;
				}
				if (specular.is_num()) {
					hybrid_instance.specular = CLAMP(float(specular), 0.0f, 1.0f);
				}
				if (thin_transmission.is_num()) {
					hybrid_instance.thin_transmission = CLAMP(float(thin_transmission), 0.0f, 1.0f);
				}
				if (thin_ior.is_num() && Math::is_finite(float(thin_ior))) {
					hybrid_instance.thin_ior = CLAMP(float(thin_ior), 1.0f, 4.0f);
				}
				if (thin_transmission_unsupported_features.is_num()) {
					hybrid_instance.thin_transmission_unsupported_features = MAX(0, int(thin_transmission_unsupported_features));
				}
				if (hybrid_instance.thin_transmission > 0.0f) {
					if (hybrid_instance.thin_transmission_unsupported_features == 0u) {
						_flux_bounded_feature_count(request.supported_thin_transmission_materials);
					} else {
						_flux_bounded_feature_count(request.unsupported_transmission_materials);
						if ((hybrid_instance.thin_transmission_unsupported_features & 1u) != 0u) _flux_bounded_feature_count(request.unsupported_transmission_texture_materials);
						if ((hybrid_instance.thin_transmission_unsupported_features & 2u) != 0u) _flux_bounded_feature_count(request.unsupported_transmission_volume_materials);
					}
				}
				if (normal_scale.is_num()) {
					hybrid_instance.normal_scale = normal_scale;
				}
				if (uv1_scale.get_type() == Variant::VECTOR3) {
					const Vector3 scale = uv1_scale;
					hybrid_instance.uv_scale = Vector2(scale.x, scale.y);
				}
				if (uv1_offset.get_type() == Variant::VECTOR3) {
					const Vector3 offset = uv1_offset;
					hybrid_instance.uv_offset = Vector2(offset.x, offset.y);
				}
				if (metallic_channel.get_type() == Variant::COLOR) {
					hybrid_instance.metallic_texture_channel = Color(metallic_channel);
				}
				if (ao_channel.get_type() == Variant::COLOR) {
					hybrid_instance.ambient_occlusion_texture_channel = Color(ao_channel);
				}
				hybrid_instance.roughness_texture_channel = classification.roughness_texture_channel;
				hybrid_instance.orm_packed = orm_texture.get_type() == Variant::RID;
				hybrid_instance.emission_multiply = classification.emission_multiply;
				if (strict_alpha_mask) {
					hybrid_instance.alpha_cutoff = alpha_cutoff_parameter;
				}
				RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
				auto resolve_texture = [texture_storage](const Variant &p_texture, bool p_srgb) -> RID {
					if (!texture_storage || p_texture.get_type() != Variant::RID) {
						return RID();
					}
					const RID texture = p_texture;
					return texture.is_valid() ? texture_storage->texture_get_rd_texture(texture, p_srgb) : RID();
				};
				if (canonical_standard_material) {
					hybrid_instance.albedo_texture = resolve_texture(albedo_texture, true);
					hybrid_instance.opacity_texture = strict_alpha_mask ? hybrid_instance.albedo_texture : RID();
					hybrid_instance.normal_texture = resolve_texture(normal_texture, false);
					hybrid_instance.orm_texture = resolve_texture(orm_texture, false);
					hybrid_instance.metallic_texture = resolve_texture(metallic_texture, false);
					hybrid_instance.roughness_texture = resolve_texture(roughness_texture, false);
					hybrid_instance.ambient_occlusion_texture = resolve_texture(ao_texture, false);
					hybrid_instance.emission_texture = resolve_texture(emission_texture, true);
					hybrid_instance.alpha_occupancy_texture = strict_alpha_mask ? resolve_texture(alpha_occupancy_texture, false) : RID();
				}
				uint64_t generation = hybrid_instance.material_stable_id ^ (uint64_t(surface_cache->shader ? surface_cache->shader->index : 0) << 32);
				generation ^= uint64_t(Math::round(hybrid_instance.thin_transmission * 65535.0f)) << 16;
				generation ^= uint64_t(Math::round(hybrid_instance.thin_ior * 65535.0f)) << 32;
				generation ^= hybrid_instance.thin_transmission_unsupported_features;
				const RID material_textures[] = { hybrid_instance.albedo_texture, hybrid_instance.normal_texture, hybrid_instance.orm_texture, hybrid_instance.metallic_texture, hybrid_instance.roughness_texture, hybrid_instance.ambient_occlusion_texture, hybrid_instance.emission_texture, hybrid_instance.opacity_texture, hybrid_instance.alpha_occupancy_texture };
				for (uint32_t texture_index = 0; texture_index < sizeof(material_textures) / sizeof(material_textures[0]); texture_index++) {
					generation ^= material_textures[texture_index].get_id() + 0x9e3779b97f4a7c15ULL + (generation << 6) + (generation >> 2);
				}
				hybrid_instance.material_generation = MAX(uint64_t(1), generation);
				material_templates.insert(material_key, hybrid_instance);
				}
			}
			if (const uint32_t *surface_request_index = added_surfaces.getptr(surface_id)) {
				RendererRD::MetalFluxEffect::Surface &request_surface = request.surfaces.write[*surface_request_index];
				request_surface.admission_emissive = request_surface.admission_emissive || hybrid_instance.emission_texture.is_valid() || hybrid_instance.emission.get_luminance() > 0.0f;
				request_surface.admission_camera_distance = MIN(request_surface.admission_camera_distance, admission_camera_distance);
			}
			hybrid_instance.dynamic = dynamic;
			request.instances.push_back(hybrid_instance);
		}
	}
	// Virtual transport is assembled from compiler ray-group hints, never from
	// this frame's raster cut. Every requested region submits its persistent far
	// cut plus every complete resident tier up to the role-selected target; the
	// Metal owner performs the atomic highest-complete TLAS substitution.
	if (p_render_data->virtual_geometry_instances) {
		auto mix_id = [](uint64_t p_hash, uint64_t p_value) {
			p_hash ^= p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2);
			return p_hash ? p_hash : 1;
		};
		for (const RendererSceneRender::VirtualGeometryInstance &vg_instance : *p_render_data->virtual_geometry_instances) {
			RendererVirtualGeometry::VirtualGeometryStorage *storage = virtual_geometry_get_storage(vg_instance.resource);
			if (!storage || vg_instance.semantic_instance_id == 0 || (vg_instance.visibility_layers & 0xffffff00u) != 0 || !storage->get_position_heap_rid().is_valid() || !storage->get_index_heap_rid().is_valid() || !storage->get_attribute_heap_rid().is_valid()) continue;
			const uint64_t ray_interest_serial = RD::get_singleton()->get_pending_submission_serial();
			const Vector3 camera_position = p_render_data->scene_data->main_cam_transform.origin;
			const Vector3 closest = camera_position.clamp(vg_instance.world_bounds.position, vg_instance.world_bounds.get_end());
			RendererVirtualGeometry::RayTransportSelectionInput tier_input;
			tier_input.role = p_shadow_only ? RendererVirtualGeometry::RayTransportRole::DIRECT_VISIBILITY : RendererVirtualGeometry::RayTransportRole::SHARP_REFLECTION;
			tier_input.distance = camera_position.distance_to(closest);
			tier_input.ray_footprint = tier_input.distance > 0.0f ? vg_instance.world_bounds.size.length() / MAX(1.0f, tier_input.distance) * 0.001f : 0.0f;
			tier_input.roughness = p_shadow_only ? 1.0f : 0.0f;
			tier_input.expected_contribution = 1.0f;
			tier_input.off_screen_influence = true;
			const RendererVirtualGeometry::RayTransportTier desired_tier = RendererVirtualGeometry::VirtualGeometryRayHierarchy::select_desired_tier(tier_input);
			const Vector<RendererVirtualGeometry::RayGroupDescriptor> &ray_groups = storage->get_ray_group_descriptors();
			HashMap<uint64_t, bool> coarse_complete_by_region;
			for (const RendererVirtualGeometry::RayGroupDescriptor &ray_group : ray_groups) {
				if (!ray_group.persistent_coarse) continue;
				bool coarse_complete = true;
				for (uint64_t cluster_id : ray_group.cluster_ids) {
					const RendererVirtualGeometry::ClusterDescriptor *cluster = storage->get_cluster_descriptor(cluster_id);
					if (!cluster) {
						coarse_complete = false;
						continue;
					}
					storage->request_page(cluster->page_id, RendererVirtualGeometry::VirtualGeometryRequestReason::TRANSPORT, 0xffffff00u);
					storage->mark_page_used(cluster->page_id, 0, ray_interest_serial);
					coarse_complete &= storage->get_gpu_cluster_descriptor(cluster_id) != nullptr;
				}
				coarse_complete_by_region.insert(ray_group.transport_region_id, coarse_complete);
			}
			for (const RendererVirtualGeometry::RayGroupDescriptor &ray_group : ray_groups) {
				if (uint32_t(ray_group.tier) > uint32_t(desired_tier)) continue;
				const bool *coarse_complete = coarse_complete_by_region.getptr(ray_group.transport_region_id);
				if (!ray_group.persistent_coarse && (!coarse_complete || !*coarse_complete)) continue;
				bool group_complete = true;
				for (uint64_t cluster_id : ray_group.cluster_ids) {
					const RendererVirtualGeometry::ClusterDescriptor *cluster = storage->get_cluster_descriptor(cluster_id);
					if (cluster) {
						storage->request_page(cluster->page_id, RendererVirtualGeometry::VirtualGeometryRequestReason::TRANSPORT, 0xffffff00u - uint32_t(ray_group.tier));
						storage->mark_page_used(cluster->page_id, 0, ray_interest_serial);
					}
					group_complete &= storage->get_gpu_cluster_descriptor(cluster_id) != nullptr;
				}
				if (!group_complete) continue;
				for (uint64_t cluster_id : ray_group.cluster_ids) {
					const RendererVirtualGeometry::ClusterDescriptor *cluster = storage->get_cluster_descriptor(cluster_id);
					const RendererVirtualGeometry::VirtualGeometryGPUClusterDescriptor *gpu = storage->get_gpu_cluster_descriptor(cluster_id);
					if (!cluster || !gpu || gpu->material_slot >= uint32_t(vg_instance.material_bindings.size())) continue;
					uint64_t surface_id = mix_id(ray_group.stable_id, cluster_id);
					surface_id = mix_id(surface_id, vg_instance.resource_revision);
					if (!added_surfaces.has(surface_id)) {
						RendererRD::MetalFluxEffect::Surface surface;
						surface.stable_id = surface_id;
						surface.topology_revision = mix_id(cluster->topology_revision, ray_group.revision);
						surface.deformation_revision = surface.topology_revision;
						surface.vertex_buffer = storage->get_position_heap_rid();
						surface.index_buffer = storage->get_index_heap_rid();
						surface.attribute_buffer = storage->get_attribute_heap_rid();
						surface.vertex_buffer_offset = gpu->position_offset;
						surface.index_buffer_offset = gpu->index_offset;
						surface.attribute_buffer_offset = gpu->attribute_offset;
						surface.vertex_count = cluster->vertex_count;
						surface.index_count = gpu->index_count;
						surface.index_stride = sizeof(uint32_t);
						surface.vertex_stride = sizeof(float) * 3;
						surface.attribute_stride = sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes);
						surface.uv_offset = offsetof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes, uv0);
						surface.has_uv = (gpu->source_stream_flags & RendererVirtualGeometry::STREAM_UV0) != 0;
						surface.compressed_aabb = cluster->bounds;
						surface.ray_group_id = ray_group.stable_id;
						surface.transport_region_id = ray_group.transport_region_id;
						surface.ray_group_revision = ray_group.revision;
						surface.ray_tier = uint32_t(ray_group.tier);
						surface.persistent_coarse = ray_group.persistent_coarse;
						surface.admission_camera_distance = tier_input.distance;
						request.surfaces.push_back(surface);
						added_surfaces.insert(surface_id, request.surfaces.size() - 1);
					}
					RendererRD::MetalFluxEffect::Instance instance;
					instance.stable_id = mix_id(vg_instance.semantic_instance_id, surface_id);
					instance.surface_id = surface_id;
					instance.transform = vg_instance.transform;
					instance.visibility_mask = vg_instance.visibility_layers;
					instance.ray_group_id = ray_group.stable_id;
					instance.transport_region_id = ray_group.transport_region_id;
					instance.ray_tier = uint32_t(ray_group.tier);
					instance.off_screen_transport = true;
					const RID material = vg_instance.material_bindings[gpu->material_slot];
					if (!material.is_valid()) continue;
					const Variant albedo = material_storage->material_get_param(material, SNAME("albedo"));
					const Variant emission = material_storage->material_get_param(material, SNAME("emission"));
					const Variant emission_energy = material_storage->material_get_param(material, SNAME("emission_energy"));
					const Variant metallic = material_storage->material_get_param(material, SNAME("metallic"));
					const Variant roughness = material_storage->material_get_param(material, SNAME("roughness"));
					// ShaderMaterial semantics cannot be reconstructed from generic parameter
					// names. Admit only the canonical StandardMaterial contract instead of
					// silently tracing fallback values.
					if (albedo.get_type() != Variant::COLOR || !metallic.is_num() || !roughness.is_num()) continue;
					instance.material_stable_id = material.get_id();
					instance.material_generation = MAX(uint64_t(1), instance.material_stable_id);
					instance.albedo = Color(albedo).srgb_to_linear();
					if (emission.get_type() == Variant::COLOR) instance.emission = Color(emission).srgb_to_linear() * (emission_energy.is_num() ? float(emission_energy) : 1.0f);
					instance.metallic = metallic;
					instance.roughness = roughness;
					request.instances.push_back(instance);
					request.ray_geometry_base_triangles += cluster->triangle_count;
					request.ray_geometry_selected_triangles += cluster->triangle_count;
					storage->mark_page_used(cluster->page_id, 0, RD::get_singleton()->get_pending_submission_serial());
				}
			}
		}
	}
	// RenderGeometryInstance iteration is not a scene-identity contract. Keep
	// the AS user-ID/material/geometry tables in a stable order so a static scene
	// cannot change ray-hit material records merely because the culler presents
	// the same instances in a different order on a later frame.
	// Large imported scenes routinely contain thousands of surfaces. The old
	// insertion sorts copied large request records O(n^2) every frame and became
	// the dominant hybrid CPU cost even after GPU work was bounded.
	request.surfaces.sort_custom<_MetalHybridSurfaceAdmissionLess>();
	request.instances.sort_custom<_MetalHybridInstanceStableIdLess>();
	request.alpha_visibility_reuse_allowed = true;
	for (const RendererRD::MetalFluxEffect::Instance &instance : request.instances) {
		if (instance.alpha_mode == RendererRD::MetalFluxEffect::Instance::ALPHA_MASK && instance.dynamic) {
			request.alpha_visibility_reuse_allowed = false;
			break;
		}
	}
	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	p_render_buffer_data->ensure_hybrid_effect();
	for (uint32_t view_index = 0; view_index < render_buffers->get_view_count(); view_index++) {
		RendererRD::MetalFluxEffect::View view;
		// The adapter must not key per-eye reservoirs by submission order: one
		// Metal effect instance serves editor, game, reflection, and stereo
		// render buffers. This custom-data object is owned by one render buffer
		// for its lifetime; eye_index remains explicit and independent.
		view.history_owner_id = uint64_t(uintptr_t(p_render_buffer_data.ptr()));
		view.eye_index = view_index;
		view.color = p_shadow_only ? RID() : render_buffers->get_internal_texture(view_index);
		view.depth = render_buffers->get_depth_texture(view_index);
		view.normal_roughness = p_render_buffer_data->get_normal_roughness(view_index);
		if (!p_shadow_only && p_render_buffer_data->has_primary_surface_v1()) {
			view.primary_material = p_render_buffer_data->get_primary_surface_material(view_index);
			view.primary_identity = p_render_buffer_data->get_primary_surface_identity(view_index);
			view.primary_geometry = p_render_buffer_data->get_primary_surface_geometry(view_index);
			view.primary_flags = p_render_buffer_data->get_primary_surface_flags(view_index);
		}
		view.effect_output = p_render_buffer_data->get_hybrid_effect(view_index);
		view.filtered_output = p_render_buffer_data->get_hybrid_filtered(view_index);
		view.guide_normal = p_render_buffer_data->get_hybrid_guide_normal(view_index);
		view.guide_diffuse = p_render_buffer_data->get_hybrid_guide_diffuse(view_index);
		view.guide_specular = p_render_buffer_data->get_hybrid_guide_specular(view_index);
		view.guide_roughness = p_render_buffer_data->get_hybrid_guide_roughness(view_index);
		view.guide_denoise_strength = p_render_buffer_data->get_hybrid_guide_denoise_strength(view_index);
		view.guide_reactive = p_render_buffer_data->get_hybrid_guide_reactive(view_index);
		view.guide_specular_distance = p_render_buffer_data->get_hybrid_guide_specular_distance(view_index);
		view.guide_transparency = p_render_buffer_data->get_hybrid_guide_transparency(view_index);
		if (!p_shadow_only) {
			view.velocity = render_buffers->get_velocity_buffer(false, view_index);
			view.history_input = p_render_buffer_data->get_hybrid_history(view_index, true);
			view.history_output = p_render_buffer_data->get_hybrid_history(view_index, false);
			view.depth_history_input = p_render_buffer_data->get_hybrid_depth_history(view_index, true);
			view.depth_history_output = p_render_buffer_data->get_hybrid_depth_history(view_index, false);
			view.normal_history_input = p_render_buffer_data->get_hybrid_normal_history(view_index, true);
			view.normal_history_output = p_render_buffer_data->get_hybrid_normal_history(view_index, false);
		}
		// The sampled raster depth is produced with RenderSceneDataRD's device
		// correction (Y flip, reversed [0, 1] depth, and current TAA jitter).
		// Supplying the raw camera projection here reconstructs a different world
		// ray whose error changes with FOV and camera distance.
		view.clip_from_view = p_render_data->scene_data->get_view_projection(view_index);
		view.world_from_view = p_render_data->scene_data->cam_transform * Transform3D(Basis(), p_render_data->scene_data->view_eye_offset[view_index]);
		const Transform3D prev_world_from_view = p_render_data->scene_data->prev_cam_transform * Transform3D(Basis(), p_render_data->scene_data->view_eye_offset[view_index]);
		Projection previous_correction;
		previous_correction.set_depth_correction(p_render_data->scene_data->flip_y);
		previous_correction.add_jitter_offset(p_render_data->scene_data->prev_taa_jitter);
		view.prev_clip_from_world = (previous_correction * p_render_data->scene_data->prev_view_projection[view_index]) * Projection(prev_world_from_view.affine_inverse());
		request.views.push_back(view);
	}
	request.reflections = true;
	// Authored material AO remains part of the raster/material response. A
	// second stochastic contact/AO multiplier made the same creases go dark
	// twice, so Flux ray transport does not add another primary AO pass.
	request.ambient_occlusion = false;
	request.contact_visibility = false;
	request.contact_visibility_strength = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/contact_visibility/strength");
	request.contact_visibility_distance = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/contact_visibility/distance");
	request.contact_visibility_samples = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/contact_visibility/sample_count");
	request.global_illumination = true;
	request.global_illumination_strength = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/global_illumination/strength");
	request.global_illumination_samples = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/global_illumination/sample_count");
	request.transport_adaptive_min_samples = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/indoor_transport/adaptive_min_samples");
	request.transport_adaptive_max_samples = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/indoor_transport/adaptive_max_samples");
	request.transport_adaptive_variance_reference = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/indoor_transport/adaptive_variance_reference");
	request.diffuse_cache_cell_size = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/indoor_transport/diffuse_cache_cell_size");
	request.restir_di_reuse = (GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/restir_di/enabled") || force_restir_di_enabled) && !force_restir_di_disabled;
	request.regir_direct_reuse = (GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/regir/enabled") || force_regir_enabled) && !force_regir_disabled;
	request.reusable_path_reuse = (GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/reusable_path_reuse/enabled") || force_reusable_path_enabled) && !force_reusable_path_disabled;
	request.unified_finite_light_reservoir = (GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/unified_finite_light_reservoir/enabled") || force_unified_finite_light_enabled) && request.restir_di_reuse;
	request.bidirectional_caustics = GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/bidirectional_caustics/enabled");
	if (force_bidirectional_caustics_disabled) {
		request.bidirectional_caustics = false;
	}
	request.bidirectional_caustic_delta_roughness_threshold = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/bidirectional_caustics/delta_roughness_threshold");
	request.bidirectional_caustic_max_mirror_triangles = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/bidirectional_caustics/max_mirror_triangles");
	request.bidirectional_caustic_max_candidates = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/bidirectional_caustics/max_candidates");
	request.frame_index = metal_flux_rendered_frames;
	const int configured_sampling_sequence = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/sampling_sequence");
	request.sampling_sequence_mode = !force_stbn_disabled && (configured_sampling_sequence != 0 || force_stbn_enabled) ? RendererPathTracing::SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE : RendererPathTracing::SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY;
	request.disable_reconstruction = force_reconstruction_disabled || request.fresh_ray_oracle;
	if (request.fresh_ray_oracle) {
		// This is a fresh-ray reference path, not a quality/performance mode. Keep
		// direct, GI, and reflection tracing enabled while removing every proposal,
		// history, cache, and reconstruction dependency before submission.
		request.restir_di_reuse = false;
		request.regir_direct_reuse = false;
		request.reusable_path_reuse = false;
		request.unified_finite_light_reservoir = false;
		request.alpha_visibility_reuse_allowed = false;
	}
	const bool environment_configured = !p_shadow_only && GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/environment_lighting/enabled");
	// Full Flux mode has no raster-owned ambient/reflection term to preserve.
	// Request the renderer-owned Sky/solar transport whenever it is configured,
	// matching the pre-cutover behavior even when the Environment's raster
	// ambient or reflection source is disabled.
	const bool environment_requested = environment_configured;
	String environment_reason = environment_configured ? "no coherent full-float Sky frame is ready" : "disabled by rendering/flux/ray_tracing/environment_lighting/enabled";
	RendererPathTracing::EnvironmentImportanceStatus environment_status = RendererPathTracing::ENVIRONMENT_IMPORTANCE_FALLBACK;
	if (environment_requested) {
		request.environment.legacy_miss_fallback = false;
		request.environment.primary_replacement = true;
		if (!is_environment(p_render_data->environment)) {
			environment_reason = "no valid Environment";
		} else {
			const RID sky_rid = environment_get_sky(p_render_data->environment);
			const RID full_sharp_radiance = sky_rid.is_valid() ? sky.sky_get_radiance_sharp_texture_rd(sky_rid) : RID();
			const uint64_t generation = sky_rid.is_valid() ? sky.sky_get_radiance_content_generation(sky_rid) : 0;
			if (!sky_rid.is_valid() || !full_sharp_radiance.is_valid() || generation == 0 || !RD::get_singleton()->texture_is_valid(full_sharp_radiance)) {
				environment_reason = "Sky sharp radiance is unavailable";
			} else {
				const RD::TextureFormat format = RD::get_singleton()->texture_get_format(full_sharp_radiance);
				if (format.format != RD::DATA_FORMAT_R32G32B32A32_SFLOAT) {
					environment_status = RendererPathTracing::ENVIRONMENT_IMPORTANCE_UNSUPPORTED;
					environment_reason = "Sky sharp radiance is not finite full-float RGBA32F";
				} else {
					RendererSkyLighting::SkyLightingSolarLobeRuntime solar_runtime;
					const bool solar_contract_present = sky.sky_get_hybrid_solar_lobe(sky_rid, solar_runtime);
					String solar_error;
					const bool solar_contract_valid = solar_contract_present && (!solar_runtime.enabled || RendererSkyLighting::sky_lighting_validate_solar_lobe_runtime(solar_runtime, &solar_error));
					const RID residual_radiance = solar_contract_valid ? sky.sky_get_hybrid_environment_residual_radiance_texture_rd(sky_rid) : RID();
					const uint64_t residual_generation = solar_contract_valid ? sky.sky_get_hybrid_environment_residual_content_generation(sky_rid) : 0;
					const bool residual_valid = residual_radiance.is_valid() && residual_generation == generation && RD::get_singleton()->texture_is_valid(residual_radiance) && RD::get_singleton()->texture_get_format(residual_radiance).format == RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
					const RID transport_radiance = residual_valid ? residual_radiance : full_sharp_radiance;
					RendererPathTracing::EnvironmentImportanceMetadata &metadata = request.environment.metadata;
					metadata.source_id = sky_rid.get_id();
					metadata.sample_id = sky_rid.get_id();
					metadata.original_resource_id = transport_radiance.get_id();
					// Visible realtime skies may rerender every frame. A proposal remains
					// unbiased while its finite nonzero floor provides full support, so
					// give the expensive transport distribution a bounded revision cadence.
					// The explicit finite solar lobe below still updates every frame.
					const uint32_t distribution_update_interval = uint32_t(MAX(1, GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/environment_lighting/distribution_update_interval_frames")));
					if (residual_valid && solar_runtime.enabled) {
						// The explicit analytic solar lobe updates every frame and is not
						// part of the residual proposal texture. Key the residual
						// distribution from its partition/profile contract, not the Sky's
						// per-frame radiance generation, so moving the sun cannot rebuild
						// the importance pyramid.
						uint64_t residual_distribution_generation = solar_runtime.profile_version;
						residual_distribution_generation ^= solar_runtime.partition_version + 0x9e3779b97f4a7c15ULL + (residual_distribution_generation << 6) + (residual_distribution_generation >> 2);
						residual_distribution_generation ^= solar_runtime.history_epoch + 0x9e3779b97f4a7c15ULL + (residual_distribution_generation << 6) + (residual_distribution_generation >> 2);
						metadata.generation = MAX(uint64_t(1), residual_distribution_generation);
					} else {
						metadata.generation = RendererPathTracing::environment_importance_transport_generation(generation, distribution_update_interval);
					}
					metadata.width = format.width;
					metadata.height = format.height;
					metadata.border = sky.sky_get_uv_border_size(sky_rid);
					metadata.array_layout = sky.sky_radiance_uses_array_layout(sky_rid);
					metadata.world_from_radiance = environment_get_sky_orientation(p_render_data->environment);
					request.environment.sharp_radiance = transport_radiance;
					request.environment.full_sharp_radiance = full_sharp_radiance;
					request.environment.residual_radiance = residual_radiance;
					request.environment.radiance_content_generation = generation;
					if (residual_valid && solar_runtime.enabled) {
						const Basis &world_from_radiance = metadata.world_from_radiance;
						RendererRD::MetalFluxEffect::FrameRequest::SolarLobe &solar_lobe = request.environment.solar_lobe;
						solar_lobe.current_direction = world_from_radiance.xform(solar_runtime.lobe.current_direction).normalized();
						solar_lobe.previous_direction = world_from_radiance.xform(solar_runtime.lobe.previous_direction).normalized();
						solar_lobe.perpendicular_irradiance = solar_runtime.lobe.perpendicular_irradiance;
						solar_lobe.angular_radius = solar_runtime.lobe.angular_radius;
						solar_lobe.cloud_transmittance = solar_runtime.cloud_transmittance;
						solar_lobe.source_id = solar_runtime.lobe.source_id;
						solar_lobe.sample_id = solar_runtime.lobe.sample_id;
						solar_lobe.profile_version = solar_runtime.profile_version;
						solar_lobe.partition_version = solar_runtime.partition_version;
						solar_lobe.state_generation = solar_runtime.state_generation;
						solar_lobe.history_epoch = solar_runtime.history_epoch;
						solar_lobe.active = true;
					}
					request.environment.active = metadata.width > 0 && metadata.height > 0 && metadata.border >= 0.0f && metadata.border < 0.5f;
					if (request.environment.active) {
						environment_status = RendererPathTracing::ENVIRONMENT_IMPORTANCE_ACTIVE;
						environment_reason = residual_valid ? "finite full-float residual Sky transport with an explicit finite solar lobe" : "finite full-float sharp renderer-owned Sky radiance";
						uint64_t history_key = metadata.history_key();
						// Portal changes alter the environment proposal mixture even when
						// the sky texture itself is unchanged. This invalidates only this
						// view's temporal state; portal records remain scene-shareable.
						history_key ^= request.environment.portal_generation + 0x9e3779b97f4a7c15ULL + (history_key << 6) + (history_key >> 2);
						if (residual_valid && solar_contract_present) {
							history_key ^= solar_runtime.lobe.source_id + 0x9e3779b97f4a7c15ULL + (history_key << 6) + (history_key >> 2);
							history_key ^= solar_runtime.lobe.sample_id + 0x9e3779b97f4a7c15ULL + (history_key << 6) + (history_key >> 2);
							history_key ^= solar_runtime.partition_version + 0x9e3779b97f4a7c15ULL + (history_key << 6) + (history_key >> 2);
							history_key ^= solar_runtime.history_epoch + 0x9e3779b97f4a7c15ULL + (history_key << 6) + (history_key >> 2);
						}
						p_render_buffer_data->update_hybrid_environment_history_key(history_key);
					} else {
						environment_reason = "Sky sharp radiance metadata is invalid";
					}
				}
			}
		}
	}
	if (!request.environment.active && environment_requested && collect_gpu_timings) {
		print_verbose(vformat("Flux environment: status=%s cache=no-distribution reason=%s; renderer-owned environment transport is unavailable for this Sky.", RendererPathTracing::environment_importance_status_name(environment_status), environment_reason));
	}
	if (!p_shadow_only && !request.environment.active) {
		p_render_buffer_data->update_hybrid_environment_history_key(0);
	}
	// A render-buffer custom-data object survives scene switches in the editor.
	// Key the temporal owner by the admitted scene identity before deciding
	// whether either the transport history or MetalFX may consume history.
	auto mix_scene_key = [](uint64_t p_hash, uint64_t p_value) {
		return p_hash ^ (p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2));
	};
	uint64_t scene_history_key = mix_scene_key(0x5343454e455f4944ULL, p_render_data->environment.get_id());
	if (p_render_data->hybrid_instances) {
		scene_history_key = mix_scene_key(scene_history_key, uint64_t(p_render_data->hybrid_instances->size()));
		for (uint32_t instance_index = 0; instance_index < p_render_data->hybrid_instances->size(); instance_index++) {
			// The culler may present the same scene in a different order. XOR keeps
			// this identity set-based rather than making iteration order temporal.
			scene_history_key ^= uint64_t(uintptr_t((*p_render_data->hybrid_instances)[instance_index])) * 0xd6e8feb86659fd93ULL;
		}
	}
	if (p_render_data->virtual_geometry_instances) {
		scene_history_key = mix_scene_key(scene_history_key, uint64_t(p_render_data->virtual_geometry_instances->size()));
		for (const RendererSceneRender::VirtualGeometryInstance &instance : *p_render_data->virtual_geometry_instances) {
			scene_history_key ^= instance.resource.get_id() * 0xa0761d6478bd642fULL;
			scene_history_key ^= instance.semantic_instance_id * 0xe7037ed1a0b428dbULL;
		}
	}
	p_render_buffer_data->update_hybrid_scene_history_key(scene_history_key);
	// MetalFX active state is not transport history. Only a validated Flux
	// history may authorize reprojection after a scene/render-buffer change.
	request.history_valid = p_render_buffer_data->has_hybrid_history();
	request.use_metalfx_denoiser = false;
#ifdef METAL_MFXTEMPORAL_ENABLED
	// Full Flux mode is entirely ray-owned. Its HDR color is stochastic shading,
	// not deterministic raster color, so use MetalFX's denoised temporal scaler
	// whenever the viewport selected MetalFX Temporal and the device supports it.
	if (!p_shadow_only && render_buffers->get_scaling_3d_mode() == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL) {
		String denoised_error;
		request.use_metalfx_denoiser = p_render_buffer_data->ensure_mfx_denoised(mfx_denoised_effect, &denoised_error);
		if (!request.use_metalfx_denoiser && collect_gpu_timings) {
			WARN_PRINT_ONCE("Flux: MetalFX temporal denoising unavailable; using ordinary MetalFX temporal scaling. " + denoised_error);
		}
	}
#endif
	if (request.fresh_ray_oracle) {
		// MetalFX owns temporal image state, so the oracle bypasses it together
		// with Flux's own reconstruction/history paths.
		request.use_metalfx_denoiser = false;
		request.history_valid = false;
	}
	request.shadow_only = p_shadow_only;
	if (!p_shadow_only) {
		const Transform3D &camera = p_render_data->scene_data->cam_transform;
		const Transform3D &previous_camera = p_render_data->scene_data->prev_cam_transform;
		const bool camera_cut = camera.origin.distance_to(previous_camera.origin) > 2.0f || camera.basis.get_column(2).dot(previous_camera.basis.get_column(2)) < 0.70710678f;
		p_render_buffer_data->set_hybrid_mfx_denoised_active(request.use_metalfx_denoiser, !request.history_valid || camera_cut);
	}
	request.ray_traced_shadows = true;
	request.shadow_sample_count = GLOBAL_GET_CACHED(int, "rendering/flux/ray_tracing/shadows/sample_count");
	bool directional_shadow_replacement_valid = false;
	// Match directional slot zero exactly: LightStorage fills the Flux raster
	// directional buffer from this same list and skips SKY_ONLY entries. The ray
	// visibility texture may replace only this light's raster visibility.
	const PagedArray<RID> *raster_lights = p_render_data->lights;
	for (uint32_t light_index = 0; raster_lights && light_index < raster_lights->size(); light_index++) {
		const RID light_instance = (*raster_lights)[light_index];
		if (!light_storage->owns_light_instance(light_instance)) {
			continue;
		}
		const RID light = light_storage->light_instance_get_base_light(light_instance);
		if (!light.is_valid() || light_storage->light_get_type(light) != RSE::LIGHT_DIRECTIONAL || light_storage->light_directional_get_sky_mode(light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			continue;
		}

		const Transform3D light_transform = light_storage->light_instance_get_base_transform(light_instance);
		request.directional_light_direction = light_transform.basis.xform(Vector3(0.0f, 0.0f, 1.0f)).normalized();
		request.directional_light_angular_radius = Math::deg_to_rad(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE) * 0.5f);
		request.directional_light_cull_mask = light_storage->light_get_cull_mask(light);
		request.directional_shadow_caster_mask = light_storage->light_get_shadow_caster_mask(light);
		request.directional_shadow_enabled = light_storage->light_has_shadow(light);
		request.directional_shadow_opacity = CLAMP(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SHADOW_OPACITY), 0.0f, 1.0f);
		request.directional_specular_amount = MAX(0.0f, light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPECULAR));
		request.directional_negative = light_storage->light_is_negative(light);
		if (shadow_scene_uses_high_layers && (request.directional_shadow_caster_mask & ~0xffu) != 0) {
			shadow_scene_complete = false;
		}
		directional_shadow_replacement_valid = light_storage->light_has_shadow(light) && light_storage->light_get_param(light, RSE::LIGHT_PARAM_SHADOW_OPACITY) > 0.001f;

		{
			// LIGHT_PARAM_INDIRECT_ENERGY is a GI-path control. Primary direct
			// lighting uses the authored direct energy exactly as the raster packet.
			float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
			energy *= request.directional_negative ? -1.0f : 1.0f;
			if (RendererSceneRenderRD::get_singleton()->is_using_physical_light_units()) {
				energy *= light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY);
			} else {
				energy *= Math::PI;
			}
			if (p_render_data->camera_attributes.is_valid()) {
				energy *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
			}
			if (Math::is_finite(energy) && !Math::is_zero_approx(energy)) {
				request.directional_light_radiance = light_storage->light_get_color(light).srgb_to_linear() * energy;
				request.directional_light_active = !Math::is_zero_approx(request.directional_light_radiance.get_luminance());
			}
		}
		break;
	}
	struct HybridPunctualCandidate {
		uint64_t stable_id = 0;
		RendererRD::MetalFluxEffect::PunctualLight light;
		double score = 0.0;
	};
	Vector<HybridPunctualCandidate> punctual_candidates;
	const PagedArray<RID> *hybrid_lights = p_render_data->hybrid_lights;
	for (uint32_t light_index = 0; hybrid_lights && light_index < hybrid_lights->size(); light_index++) {
		RID light_instance = (*hybrid_lights)[light_index];
		if (!light_storage->owns_light_instance(light_instance)) {
			continue;
		}
		RID light = light_storage->light_instance_get_base_light(light_instance);
		if (!light.is_valid()) {
			continue;
		}
		const RSE::LightType type = light_storage->light_get_type(light);
		if (!p_shadow_only) {
			const bool supported_directional = type == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(light) != RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY;
			if (type == RSE::LIGHT_OMNI || type == RSE::LIGHT_SPOT || type == RSE::LIGHT_AREA || supported_directional) {
				// Projected and textured area emission have their own texture-space
				// contracts. Preserve that semantic boundary instead of treating the
				// texture as an unmodulated finite emitter.
				if ((type == RSE::LIGHT_SPOT && light_storage->light_has_projector(light)) ||
						(type == RSE::LIGHT_AREA && light_storage->light_area_get_texture(light).is_valid())) {
					request.unsupported_punctual_lights++;
					continue;
				}
				RendererRD::MetalFluxEffect::PunctualLight punctual;
				const Transform3D light_transform = light_storage->light_instance_get_base_transform(light_instance);
				punctual.position = light_transform.origin;
				punctual.direction = light_transform.basis.xform(type == RSE::LIGHT_DIRECTIONAL ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 0.0f, -1.0f)).normalized();
				punctual.type = type == RSE::LIGHT_OMNI ? RendererRD::MetalFluxEffect::PunctualLight::TYPE_OMNI :
						type == RSE::LIGHT_SPOT ? RendererRD::MetalFluxEffect::PunctualLight::TYPE_SPOT :
						type == RSE::LIGHT_AREA ? RendererRD::MetalFluxEffect::PunctualLight::TYPE_AREA : RendererRD::MetalFluxEffect::PunctualLight::TYPE_DIRECTIONAL;
				const float raw_range = type == RSE::LIGHT_DIRECTIONAL ? Math::deg_to_rad(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE) * 0.5f) : light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE);
				punctual.range = Math::is_finite(raw_range) ? MAX(type == RSE::LIGHT_DIRECTIONAL ? 0.0f : 0.001f, raw_range) : (type == RSE::LIGHT_DIRECTIONAL ? 0.0f : 0.001f);
				const float raw_attenuation = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION);
				punctual.attenuation = Math::is_finite(raw_attenuation) ? MAX(0.0f, raw_attenuation) : 0.0f;
				if (type == RSE::LIGHT_SPOT) {
					const float angle = light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ANGLE);
					const float spot_attenuation = light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ATTENUATION);
					punctual.spot_cos_outer = Math::cos(Math::deg_to_rad(CLAMP(angle, 0.0f, 89.9f)));
					punctual.spot_attenuation = Math::is_finite(spot_attenuation) ? MAX(spot_attenuation, 0.0001f) : 1.0f;
				} else if (type == RSE::LIGHT_AREA) {
					const Vector2 size = light_storage->light_area_get_size(light).maxf(0.0f);
					punctual.area_u = light_transform.basis.xform(Vector3(size.x * 0.5f, 0.0f, 0.0f));
					punctual.area_v = light_transform.basis.xform(Vector3(0.0f, size.y * 0.5f, 0.0f));
					if (punctual.area_u.cross(punctual.area_v).length_squared() <= CMP_EPSILON2) {
						request.unsupported_punctual_lights++;
						continue;
					}
				}
				punctual.cull_mask = light_storage->light_instance_get_cull_mask(light_instance);
				punctual.shadow_caster_mask = light_storage->light_get_shadow_caster_mask(light);
				punctual.stable_id = light_instance.get_id();
				punctual.shadow_enabled = light_storage->light_has_shadow(light);
				punctual.shadow_opacity = CLAMP(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SHADOW_OPACITY), 0.0f, 1.0f);
				punctual.specular_amount = MAX(0.0f, light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPECULAR));
				const float raw_indirect_energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
				punctual.indirect_energy = Math::is_finite(raw_indirect_energy) ? MAX(raw_indirect_energy, 0.0f) : 0.0f;
				punctual.negative = light_storage->light_is_negative(light);
				float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
				energy *= punctual.negative ? -1.0f : 1.0f;
				if (type != RSE::LIGHT_DIRECTIONAL && light_storage->light_is_distance_fade_enabled(light)) {
					const float fade_length = light_storage->light_get_distance_fade_length(light);
					const float camera_distance = p_render_data->scene_data->cam_transform.origin.distance_to(punctual.position);
					if (fade_length <= 0.0f) {
						energy *= camera_distance <= light_storage->light_get_distance_fade_begin(light) ? 1.0f : 0.0f;
					} else if (camera_distance > light_storage->light_get_distance_fade_begin(light)) {
						energy *= Math::smoothstep(0.0f, 1.0f, 1.0f - (camera_distance - light_storage->light_get_distance_fade_begin(light)) / fade_length);
					}
					float shadow_fade = 1.0f;
					if (fade_length <= 0.0f) {
						shadow_fade = camera_distance <= light_storage->light_get_distance_fade_shadow(light) ? 1.0f : 0.0f;
					} else if (camera_distance > light_storage->light_get_distance_fade_shadow(light)) {
						shadow_fade = Math::smoothstep(0.0f, 1.0f, 1.0f - (camera_distance - light_storage->light_get_distance_fade_shadow(light)) / fade_length);
					}
					punctual.shadow_opacity *= shadow_fade;
				}
				if (RendererSceneRenderRD::get_singleton()->is_using_physical_light_units()) {
					const float intensity = light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY);
					energy *= type == RSE::LIGHT_DIRECTIONAL ? intensity : intensity / (type == RSE::LIGHT_OMNI ? Math::PI * 4.0f : (type == RSE::LIGHT_AREA ? Math::PI * 2.0f : Math::PI));
				} else {
					energy *= Math::PI;
				}
				if (p_render_data->camera_attributes.is_valid()) {
					energy *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
				}
				if (!Math::is_finite(energy)) {
					energy = 0.0f;
				}
				const Color radiance = light_storage->light_get_color(light).srgb_to_linear() * energy;
				punctual.radiance = Color(Math::is_finite(radiance.r) ? radiance.r : 0.0f, Math::is_finite(radiance.g) ? radiance.g : 0.0f, Math::is_finite(radiance.b) ? radiance.b : 0.0f) * stage_probe_light_scale;
				const float camera_distance = p_render_data->scene_data->cam_transform.origin.distance_to(punctual.position);
				const float finite_distance = Math::is_finite(camera_distance) ? MAX(0.0f, camera_distance) : 0.0f;
				const float max_radiance = MAX(punctual.radiance.r, MAX(punctual.radiance.g, punctual.radiance.b));
				const float distance_from_range = MAX(finite_distance - punctual.range, 0.0f);
				HybridPunctualCandidate candidate;
				candidate.stable_id = light_instance.get_id();
				candidate.light = punctual;
				// The local proposal score only controls upload ordering; the active
				// direct estimator uses each light's real type/PDF and visibility.
				// Stable RID tie-breaks below keep equal-score packets deterministic.
				const double extent = type == RSE::LIGHT_DIRECTIONAL ? 1.0 : (type == RSE::LIGHT_AREA ? MAX(double(punctual.area_u.length() + punctual.area_v.length()), 0.001) : double(punctual.range));
				const double numerator = double(max_radiance) * extent * extent;
				const double denominator = MAX(double(distance_from_range) * double(distance_from_range), 0.01);
				candidate.score = numerator / denominator;
				if (!Math::is_finite(candidate.score) || candidate.score < 0.0) {
					candidate.score = candidate.score < 0.0 ? 0.0 : 1.0e300;
				}
				punctual_candidates.push_back(candidate);
			}
		}
	}
	for (int i = 1; i < punctual_candidates.size(); i++) {
		HybridPunctualCandidate value = punctual_candidates[i];
		int insertion = i;
		while (insertion > 0 && (value.score > punctual_candidates[insertion - 1].score || (value.score == punctual_candidates[insertion - 1].score && value.stable_id < punctual_candidates[insertion - 1].stable_id))) {
			punctual_candidates.write[insertion] = punctual_candidates[insertion - 1];
			insertion--;
		}
		punctual_candidates.write[insertion] = value;
	}
	for (uint32_t i = 0; i < uint32_t(punctual_candidates.size()); i++) {
		request.punctual_lights.push_back(punctual_candidates[i].light);
	}
	request.punctual_light_overflow = 0;
	if (p_shadow_only && (!directional_shadow_replacement_valid || !shadow_scene_complete || request.directional_light_direction.is_zero_approx())) {
		return false;
	}
	MetalFluxTimingCaptureState *timing_capture_state = collect_gpu_timings ? timing_capture_state_for_owner(request.diagnostics_owner_id) : nullptr;
	request.collect_gpu_timings = collect_gpu_timings && metal_flux_rendered_frames >= 8 && timing_capture_state && (p_shadow_only ? !timing_capture_state->shadow_capture_submitted : !timing_capture_state->effect_capture_submitted);
	request.reflection_strength = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/reflection_strength");
	request.reflection_roughness_cutoff = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/reflection_roughness_cutoff");
	request.ambient_occlusion_strength = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/ambient_occlusion_strength");
	request.ambient_occlusion_distance = GLOBAL_GET_CACHED(float, "rendering/flux/ray_tracing/ambient_occlusion_distance");
	if (request.surfaces.is_empty() || request.instances.is_empty()) {
		if (collect_gpu_timings && request.unsupported_materials > 0 && !metal_flux_unsupported_materials_reported) {
			print_line(vformat("Flux: skipped %d unsupported ShaderMaterial surface(s); no canonical ray surfaces were submitted.", request.unsupported_materials));
			metal_flux_unsupported_materials_reported = true;
		}
		if (!p_shadow_only) {
			// Do not let the scaler consume guides from a previous, now-empty scene.
			p_render_buffer_data->set_hybrid_mfx_denoised_active(false, true);
		}
		return false;
	}
	RendererRD::MetalFluxEffect::FrameResult result;
	String error;
	bool collect_metalfx_temporal_detail = false;
	for (const String &argument : OS::get_singleton()->get_cmdline_user_args()) {
		if (argument == "--validate-hybrid-temporal-detail") {
			collect_metalfx_temporal_detail = true;
			break;
		}
	}
	request.collect_metalfx_reactive_telemetry = !p_shadow_only && request.use_metalfx_denoiser && collect_metalfx_temporal_detail;
	// One fixed-point readback every thirty submitted transport frames keeps the
	// diagnostic bounded while providing the 30/60/180 convergence checkpoints.
	request.collect_stage_probe = !p_shadow_only && collect_stage_probe && ((metal_flux_rendered_frames + 1u) % 30u) == 0u;
	if (request.collect_stage_probe) {
		request.metalfx_diagnostic_submission_index = metal_flux_rendered_frames + 1u;
	}
	if (request.collect_metalfx_reactive_telemetry) {
		request.metalfx_diagnostic_submission_index = metal_flux_mfx_denoised_submissions + 1;
		request.report_metalfx_reactive_coverage = (request.metalfx_diagnostic_submission_index % 60u) == 0u;
	}
	if (metal_flux_effect->render(request, result, &error) != OK) {
		WARN_PRINT_ONCE("Flux Metal ray effects disabled for this frame: " + error);
		if (!p_shadow_only) {
			p_render_buffer_data->set_hybrid_mfx_denoised_active(false, true);
		}
		return false;
	}
	if (request.collect_gpu_timings && result.gpu_timing_capture_submitted && timing_capture_state) {
		if (p_shadow_only) {
			timing_capture_state->shadow_capture_submitted = true;
		} else {
			timing_capture_state->effect_capture_submitted = true;
		}
	}
	if (p_shadow_only && (request.unsupported_materials > 0 || result.blas_builds_deferred > 0 || result.blas_build_triangles_deferred > 0)) {
		// The legacy visibility-only entry point cannot claim ownership while its
		// transport scene is incomplete. Full ray-on shading uses fail-open
		// visibility in the complete transport dispatch instead.
		return false;
	}
	if (!p_shadow_only && p_render_data->render_info) {
		RenderingServerTypes::FluxDiagnostics diagnostics;
		diagnostics.reset_for_frame(request.diagnostics_frame, 2);
		// This snapshot represents the completed full ray-effects pass. Mode 1
		// is reserved for the directional ray-visibility-only pass; the complete
		// Flux transport pass is mode 2 even though scheduling uses a boolean gate.
		diagnostics.ray_effects_active = true;
		diagnostics.denoiser = result.metalfx_denoiser ? "metalfx" : (result.flux_image_reconstruction ? "flux" : "none");
		diagnostics.flux_image_reconstruction = result.flux_image_reconstruction;
		const Size2i internal_size = render_buffers->get_internal_size();
		const Size2i target_size = render_buffers->get_target_size();
		diagnostics.viewport_internal_width = MAX(internal_size.x, 0);
		diagnostics.viewport_internal_height = MAX(internal_size.y, 0);
		diagnostics.viewport_target_width = MAX(target_size.x, 0);
		diagnostics.viewport_target_height = MAX(target_size.y, 0);
		diagnostics.viewport_scaling_3d_mode = int32_t(render_buffers->get_scaling_3d_mode());
		diagnostics.viewport_scaling_3d_scale = target_size.x > 0 ? float(internal_size.x) / float(target_size.x) : 1.0f;
		diagnostics.preview_admission_active = request.preview_admission_active;
		diagnostics.preview_admission_blas_build_limit = request.preview_admission_active ? request.maximum_blas_builds_per_frame : 0;
		diagnostics.preview_admission_blas_triangle_limit = request.preview_admission_active ? request.maximum_blas_build_triangles_per_frame : 0;
		diagnostics.stbn_sampling_enabled = request.sampling_sequence_mode == RendererPathTracing::SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE;
		diagnostics.restir_di_enabled = request.restir_di_reuse;
		diagnostics.regir_reuse_enabled = request.regir_direct_reuse;
		diagnostics.reusable_path_reuse_enabled = request.reusable_path_reuse;
		diagnostics.unified_finite_light_reuse_enabled = request.unified_finite_light_reservoir;
		diagnostics.environment_active = request.environment.active;
		diagnostics.environment_status = environment_requested ? RendererPathTracing::environment_importance_status_name(request.environment.active ? result.environment.status : environment_status) : "disabled";
		diagnostics.environment_importance_cache = RendererPathTracing::environment_importance_cache_name(result.environment.cache_decision);
		diagnostics.environment_radiance_width = result.environment.radiance_width;
		diagnostics.environment_radiance_height = result.environment.radiance_height;
		diagnostics.environment_proposal_width = result.environment.proposal_width;
		diagnostics.environment_proposal_height = result.environment.proposal_height;
			diagnostics.primary_surface_version = 1;
			diagnostics.trace_compaction_active = result.trace_compaction_active;
			diagnostics.trace_compaction_fallback = result.trace_compaction_fallback;
		diagnostics.ray_owned_shading = result.raster_primary_surface_views == request.views.size();
		diagnostics.primary_surface_view_count = result.raster_primary_surface_views;
		diagnostics.primary_unsupported_surface_count = request.unsupported_materials;
		diagnostics.invalid_pdf_sample_count = result.invalid_pdf_samples;
		diagnostics.nonfinite_lobe_sample_count = result.nonfinite_lobe_samples;
		diagnostics.rejected_energy_sample_count = result.rejected_energy_samples;
		diagnostics.primary_valid_pixel_count = result.primary_valid_pixels;
		diagnostics.primary_invalid_pixel_count = result.primary_invalid_pixels;
		diagnostics.primary_lit_pixel_count = result.primary_lit_pixels;
		diagnostics.acceleration_structure_deferred_build_count = result.blas_builds_deferred;
		diagnostics.acceleration_structure_deferred_triangle_count = result.blas_build_triangles_deferred;
		diagnostics.acceleration_structure_tlas_rebuild_count = result.tlas_rebuilds;
		diagnostics.acceleration_structure_tlas_refit_count = result.tlas_refits;
		diagnostics.acceleration_structure_tlas_reuse_count = result.tlas_reuses;
		diagnostics.alpha_mask_instance_count = result.alpha_mask_instances;
		diagnostics.alpha_traversal_fallback_count = result.alpha_traversal_fallbacks;
		diagnostics.emissive_triangle_count = result.emissive_triangle_count;
		diagnostics.emissive_triangle_capacity = result.emissive_triangle_capacity;
		diagnostics.emissive_triangle_overflow = result.emissive_triangle_overflow;
		diagnostics.diffuse_cache_hit_count = result.diffuse_cache_hits;
		diagnostics.diffuse_cache_update_count = result.diffuse_cache_updates;
		diagnostics.diffuse_cache_bytes = result.diffuse_cache_bytes;
		diagnostics.regir_enabled = result.regir_enabled;
		diagnostics.regir_valid = result.regir_valid;
		diagnostics.regir_complete = result.regir_complete;
		diagnostics.regir_cell_count = result.regir_cells;
		diagnostics.regir_bytes = result.regir_bytes;
		diagnostics.reusable_path_cache_enabled = result.reusable_path_cache_enabled;
		diagnostics.reusable_path_cache_valid = result.reusable_path_cache_valid;
		diagnostics.reusable_path_cache_complete = result.reusable_path_cache_complete;
		diagnostics.reusable_path_cache_cell_count = result.reusable_path_cache_cells;
		diagnostics.reusable_path_cache_occupied_cell_count = result.reusable_path_cache_occupied_cells;
		diagnostics.reusable_path_cache_bytes = result.reusable_path_cache_bytes;
		diagnostics.restir_gi_valid = result.restir_gi_valid;
		diagnostics.restir_gi_complete = result.restir_gi_complete;
		diagnostics.restir_gi_backend_prototype = result.restir_gi_complete;
		diagnostics.bidirectional_caustic_enabled = result.bidirectional_caustic_enabled;
		diagnostics.bidirectional_caustic_active = result.bidirectional_caustic_active;
		diagnostics.bidirectional_caustic_complete = result.bidirectional_caustic_complete;
		diagnostics.bidirectional_caustic_backend_prototype = result.bidirectional_caustic_backend_prototype;
		diagnostics.bidirectional_caustic_mirror_triangle_count = result.bidirectional_caustic_mirror_triangle_count;
		diagnostics.bidirectional_caustic_mirror_triangle_capacity = result.bidirectional_caustic_mirror_triangle_capacity;
		diagnostics.bidirectional_caustic_mirror_triangle_overflow = result.bidirectional_caustic_mirror_triangle_overflow;
		diagnostics.bidirectional_caustic_source_triangle_count = result.bidirectional_caustic_source_triangle_count;
		diagnostics.punctual_light_count = result.punctual_lights;
		diagnostics.punctual_light_overflow_count = result.punctual_light_overflow;
		diagnostics.unsupported_punctual_light_count = result.unsupported_punctual_lights;
		diagnostics.light_distribution_identity = result.light_distribution_identity;
		diagnostics.light_revision_requested = result.light_distribution_generation;
		diagnostics.light_revision_submitted = result.light_distribution_generation;
		diagnostics.admitted_geometry_generation = result.admitted_geometry_generation;
		diagnostics.visibility_residency_generation = result.visibility_residency_generation;
		diagnostics.residency_completion_token = result.residency_completion_token;
		diagnostics.residency_complete = result.residency_complete;
		diagnostics.light_distribution_generation = result.light_distribution_generation;
		diagnostics.environment_generation = result.environment_generation;
		diagnostics.transport_revisions_valid = result.transport_revisions_valid;
		diagnostics.transport_history_invalid_reasons = result.transport_history_invalid_reasons;
		diagnostics.cpu_scene_preparation_milliseconds = result.cpu_scene_preparation_milliseconds;
		diagnostics.cpu_residency_planning_milliseconds = result.cpu_residency_planning_milliseconds;
		diagnostics.cpu_residency_plan_cached_milliseconds = result.cpu_residency_plan_cached_milliseconds;
		diagnostics.cpu_residency_plan_full_milliseconds = result.cpu_residency_plan_full_milliseconds;
		diagnostics.residency_plan_cache_hit_count = result.residency_plan_cache_hit_count;
		diagnostics.residency_plan_cache_miss_count = result.residency_plan_cache_miss_count;
		diagnostics.residency_plan_cache_rebuild_count = result.residency_plan_cache_rebuild_count;
		diagnostics.cpu_metal_preparation_milliseconds = result.cpu_metal_preparation_milliseconds;
		diagnostics.cpu_submission_milliseconds = result.cpu_submission_milliseconds;
		diagnostics.admitted_geometry_count = request.instances.size();
		diagnostics.admitted_surface_count = request.surfaces.size();
		diagnostics.admitted_base_triangle_count = result.ray_geometry_base_triangles;
		diagnostics.admitted_selected_triangle_count = result.ray_geometry_selected_triangles;
		// material_templates is keyed by the canonical material RID, while
		// result.textured_materials is per TLAS instance and may contain duplicates.
		diagnostics.admitted_canonical_material_count = material_templates.size();
		diagnostics.transport_state = RendererPathTracing::transport_culling_state_name(request.transport_state);
		diagnostics.transport_reason = RendererPathTracing::transport_culling_reason_name(request.transport_reason);
		diagnostics.transport_max_distance = request.transport_max_distance;
		diagnostics.set_transport_counts(request.transport_primary_geometry_count, request.transport_selected_geometry_count, request.transport_eligible_geometry_count);
		diagnostics.transport_selected_light_count = request.transport_selected_light_count;
		diagnostics.transport_eligible_light_count = request.transport_eligible_light_count;
		diagnostics.ray_proxy_source_count = request.ray_proxy_source_count;
		diagnostics.ray_proxy_substituted_count = request.ray_proxy_substituted_count;
		diagnostics.ray_proxy_fail_open_count = request.ray_proxy_fail_open_count;
		diagnostics.ray_proxy_duplicate_count = request.ray_proxy_duplicate_count;
		for (uint32_t reason = 0; reason < RendererPathTracing::RAY_PROXY_RELATION_MAX; reason++) {
			diagnostics.ray_proxy_rejection_counts[reason] = request.ray_proxy_rejection_counts[reason];
		}
		diagnostics.material_tier2 = result.material_texture_tier2;
		diagnostics.material_capacity = result.material_texture_capacity;
		auto copy_texture_diagnostics = [&result](RendererPathTracing::HybridResidencyTextureChannel p_channel, RenderingServerTypes::FluxTextureDiagnostics &r_channel) {
			const uint32_t channel = static_cast<uint32_t>(p_channel);
			r_channel.requested = result.material_texture_requested[channel];
			r_channel.resident = result.material_texture_resident[channel];
			r_channel.misses = result.material_texture_misses[channel];
		};
		copy_texture_diagnostics(RendererPathTracing::HybridResidencyTextureChannel::ALBEDO, diagnostics.material_albedo);
		copy_texture_diagnostics(RendererPathTracing::HybridResidencyTextureChannel::NORMAL, diagnostics.material_normal);
		copy_texture_diagnostics(RendererPathTracing::HybridResidencyTextureChannel::ORM, diagnostics.material_orm);
		copy_texture_diagnostics(RendererPathTracing::HybridResidencyTextureChannel::EMISSIVE, diagnostics.material_emissive);
		copy_texture_diagnostics(RendererPathTracing::HybridResidencyTextureChannel::OPACITY, diagnostics.material_opacity);
		copy_texture_diagnostics(RendererPathTracing::HybridResidencyTextureChannel::ALPHA_OCCUPANCY, diagnostics.material_alpha_occupancy);
		diagnostics.supported_thin_transmission_material_count = result.supported_thin_transmission_materials;
		diagnostics.unsupported_transmission_texture_material_count = result.unsupported_transmission_texture_materials;
		diagnostics.unsupported_transmission_volume_material_count = result.unsupported_transmission_volume_materials;
		const uint32_t texture_misses = diagnostics.material_albedo.misses + diagnostics.material_normal.misses + diagnostics.material_orm.misses + diagnostics.material_emissive.misses + diagnostics.material_opacity.misses + diagnostics.material_alpha_occupancy.misses;
		diagnostics.transport_complete = diagnostics.ray_owned_shading && request.unsupported_materials == 0 && result.blas_builds_deferred == 0 && result.blas_build_triangles_deferred == 0 && texture_misses == 0 && result.invalid_pdf_samples == 0 && result.nonfinite_lobe_samples == 0;
		if (diagnostics.transport_complete) {
			diagnostics.transport_incomplete_reason = "complete";
		} else if (!diagnostics.ray_owned_shading) {
			diagnostics.transport_incomplete_reason = "PrimarySurfaceV1 unavailable";
		} else if (request.unsupported_materials > 0) {
			diagnostics.transport_incomplete_reason = "unsupported secondary material closure";
		} else if (result.blas_builds_deferred > 0 || result.blas_build_triangles_deferred > 0) {
			diagnostics.transport_incomplete_reason = "acceleration-structure admission deferred; visibility fails open";
		} else if (result.invalid_pdf_samples > 0 || result.nonfinite_lobe_samples > 0) {
			diagnostics.transport_incomplete_reason = "invalid transport sample rejected";
		} else {
			diagnostics.transport_incomplete_reason = "material texture residency incomplete";
		}
		diagnostics.refresh_shadow_residency_completeness();
		const uint64_t owner = request.diagnostics_owner_id;
		if (owner != 0) {
			Vector<RenderingServerTypes::FluxDiagnosticsPendingState> *pending_frames = metal_flux_pending_diagnostics.getptr(owner);
			if (!pending_frames) {
				metal_flux_pending_diagnostics.insert(owner, Vector<RenderingServerTypes::FluxDiagnosticsPendingState>());
				pending_frames = metal_flux_pending_diagnostics.getptr(owner);
			}
			bool already_pending = false;
			for (const RenderingServerTypes::FluxDiagnosticsPendingState &pending : *pending_frames) {
				already_pending |= pending.diagnostics.frame == diagnostics.frame;
			}
			if (!already_pending) {
				if (pending_frames->size() == 64) {
					pending_frames->remove_at(0);
				}
				RenderingServerTypes::FluxDiagnosticsPendingState pending;
				pending.diagnostics = diagnostics;
				pending.timing_expected = collect_gpu_timings && result.gpu_timing_capture_submitted;
				pending.work_attribution_expected = true;
				pending_frames->push_back(pending);
			}
			diagnostics.pending_capture_count = pending_frames->size();
		}
		if (!publish_completed_for_current_owner()) {
			p_render_data->render_info->flux = diagnostics;
		}
	}
	if (!p_shadow_only && request.use_metalfx_denoiser) {
		metal_flux_mfx_denoised_submissions++;
		if (collect_metalfx_temporal_detail && (metal_flux_mfx_denoised_submissions % 60u) == 0u) {
			print_line(vformat("Flux MetalFX actual denoised submissions=%d history=%s.", metal_flux_mfx_denoised_submissions, request.history_valid ? "valid" : "invalid"));
		}
	}
	metal_flux_material_residency_diagnostics_observed |= result.full_material_diagnostics_observed;
	if (!p_shadow_only && request.use_metalfx_denoiser && result.scene.blas_built > 0 && !request.history_valid) {
		// First admission already implies a fresh effect/camera history. Do not
		// reset the whole MetalFX image for a later BLAS admission: primary depth,
		// normal, motion, and the reactive mask isolate that newly visible surface,
		// while a scene-wide reset would prevent a stable room from converging.
		p_render_buffer_data->set_hybrid_mfx_denoised_active(true, true);
	}
	if (!p_shadow_only) {
		// This ping-pong owns primary identity/depth/shading guide history as well
		// as the non-MetalFX reconstruction records. Advancing it while MetalFX is
		// active keeps the reactive/disocclusion guides coherent from the next
		// frame; it does not run or blend Flux image reconstruction.
		p_render_buffer_data->advance_hybrid_history();
	}
	if (collect_gpu_timings && !p_shadow_only && request.environment.active) {
		const uint64_t key = request.environment.metadata.distribution_key();
		if (result.environment.cache_decision == RendererPathTracing::ENVIRONMENT_IMPORTANCE_CACHE_REBUILT || metal_flux_environment_reported_key != key || !metal_flux_environment_reuse_reported) {
			print_line(vformat("Flux environment: status=%s cache=%s source_id=%d generation=%d checksum=%d weights=%s reason=%s ownership=%s solar=%s irradiance=(%.3f, %.3f, %.3f); environment_sampling is included in ray_effects.",
					RendererPathTracing::environment_importance_status_name(result.environment.status),
					RendererPathTracing::environment_importance_cache_name(result.environment.cache_decision),
					result.environment.source_id,
					result.environment.generation,
					result.environment.checksum,
					result.environment.weight_state,
					result.environment.cache_reason,
					environment_reason,
					request.environment.solar_lobe.active ? "active" : "inactive",
					request.environment.solar_lobe.perpendicular_irradiance.r,
					request.environment.solar_lobe.perpendicular_irradiance.g,
					request.environment.solar_lobe.perpendicular_irradiance.b));
			metal_flux_environment_reported_key = key;
			metal_flux_environment_reuse_reported = result.environment.cache_decision == RendererPathTracing::ENVIRONMENT_IMPORTANCE_CACHE_REUSED;
		}
	}
	if (collect_gpu_timings && !p_shadow_only && !metal_flux_material_residency_diagnostic_reported && metal_flux_material_residency_diagnostics_observed) {
		const uint32_t albedo_channel = static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::ALBEDO);
		const uint32_t normal_channel = static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::NORMAL);
		const uint32_t orm_channel = static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::ORM);
		const uint32_t emissive_channel = static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::EMISSIVE);
		const uint32_t opacity_channel = static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::OPACITY);
		const uint32_t occupancy_channel = static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::ALPHA_OCCUPANCY);
		print_line(vformat("Flux material residency: %s capacity=%d; albedo req/res/miss=%d/%d/%d normal=%d/%d/%d ORM=%d/%d/%d emissive=%d/%d/%d opacity=%d/%d/%d occupancy=%d/%d/%d; unsupported=%d feature_ignored clearcoat/anisotropy/transmission/refraction=%d/%d/%d/%d; thin_transmission supported/texture_unsupported/volume_unsupported=%d/%d/%d; reflection_environment_miss/contrib=%d/%d; alpha instances/candidates/rejections/exhaustions=%d/%d/%d/%d occupancy empty/opaque/mixed=%d/%d/%d fail_open_fallbacks=%d max_per_ray=%d classes primary=%d/%d visibility=%d/%d reflection=%d/%d indirect=%d/%d generation_rejects=%d table_update=%.3f ms.",
				result.material_texture_tier2 ? "Tier-2" : "fallback",
				result.material_texture_capacity,
				result.material_texture_requested[albedo_channel], result.material_texture_resident[albedo_channel], result.material_texture_misses[albedo_channel],
				result.material_texture_requested[normal_channel], result.material_texture_resident[normal_channel], result.material_texture_misses[normal_channel],
				result.material_texture_requested[orm_channel], result.material_texture_resident[orm_channel], result.material_texture_misses[orm_channel],
				result.material_texture_requested[emissive_channel], result.material_texture_resident[emissive_channel], result.material_texture_misses[emissive_channel],
				result.material_texture_requested[opacity_channel], result.material_texture_resident[opacity_channel], result.material_texture_misses[opacity_channel],
				result.material_texture_requested[occupancy_channel], result.material_texture_resident[occupancy_channel], result.material_texture_misses[occupancy_channel],
				result.unsupported_materials,
				request.unsupported_clearcoat_materials,
				request.unsupported_anisotropy_materials,
				request.unsupported_transmission_materials,
				request.unsupported_refraction_materials,
				result.supported_thin_transmission_materials,
				result.unsupported_transmission_texture_materials,
				result.unsupported_transmission_volume_materials,
				result.reflection_environment_misses,
				result.reflection_environment_contributions,
				result.alpha_mask_instances, result.alpha_candidates, result.alpha_rejections, result.alpha_candidate_exhaustions,
				result.alpha_occupancy_empty_rejections, result.alpha_occupancy_opaque_accepts, result.alpha_occupancy_mixed_samples,
				result.alpha_traversal_fallbacks,
				result.alpha_max_candidates_per_ray,
				result.alpha_primary_candidates, result.alpha_primary_rejections,
				result.alpha_visibility_candidates, result.alpha_visibility_rejections,
				result.alpha_reflection_candidates, result.alpha_reflection_rejections,
				result.alpha_indirect_candidates, result.alpha_indirect_rejections,
				result.material_generation_rejects,
				result.material_table_update_milliseconds));
		metal_flux_material_residency_diagnostic_reported = true;
	}
	if (collect_gpu_timings && !p_shadow_only && !metal_flux_diagnostic_reported && metal_flux_rendered_frames >= 8) {
		print_line(vformat("Flux transport culling: state=%s reason=%s distance=%.2f m primary=%d geometry=%d/%d lights=%d/%d.",
				RendererPathTracing::transport_culling_state_name(request.transport_state),
				RendererPathTracing::transport_culling_reason_name(request.transport_reason),
				request.transport_max_distance,
				request.transport_primary_geometry_count,
				request.transport_selected_geometry_count,
				request.transport_eligible_geometry_count,
				request.transport_selected_light_count,
				request.transport_eligible_light_count));
		const bool using_metalfx_temporal = render_buffers->get_scaling_3d_mode() == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL;
		const String hybrid_features = String(" + raster authored emission + ray-owned analytic/emissive direct shadows + one-bounce diffuse transport + GGX reflections") + (result.world_space_diffuse_contact_visibility_views > 0 ? vformat(" + world-space diffuse contact visibility (%d ray(s), %.2f m, %d view(s))", request.contact_visibility_samples, request.contact_visibility_distance, result.world_space_diffuse_contact_visibility_views) : "");
		Color analytic_radiance_sum;
		uint32_t analytic_directional_count = 0;
		for (const RendererRD::MetalFluxEffect::PunctualLight &light : request.punctual_lights) {
			analytic_radiance_sum += light.radiance;
			analytic_directional_count += light.type == RendererRD::MetalFluxEffect::PunctualLight::TYPE_DIRECTIONAL;
		}
		print_line(vformat("Flux: PrimarySurfaceV1 material/visibility + Metal ray-owned direct shadows/GI/reflections%s; primary surface views=%d; transport-history-valid views=%d; triangle normals + %d canonical StandardMaterial3D/ORM material(s) with albedo, normal, ORM, emissive, and strict-opacity semantics, %d texture fallback(s); ray geometry triangles base/selected=%d/%d across %d LOD-selected instance surface(s), base retained dynamic/alpha-mask/near-field=%d/%d/%d; analytic lights serialized/secondary-consumed=%d/%d (%d directional), radiance sum=(%.3f, %.3f, %.3f), ray primary selected/contributed/visibility=%d/%d/%d, %d overflow, %d explicitly unsupported light(s); restored variance-aware spatial/motion-valid temporal + %s; %d view(s); BLAS build/refit/reuse %d/%d/%d, deferred %d build(s)/%d triangle(s); invalid PDF/nonfinite/rejected energy=%d/%d/%d.",
				hybrid_features,
				result.raster_primary_surface_views,
				result.transport_history_valid_views,
				result.textured_materials,
				result.texture_fallbacks,
				result.ray_geometry_base_triangles,
				result.ray_geometry_selected_triangles,
				result.ray_lod_instance_surfaces,
				result.ray_lod_base_dynamic_surfaces,
				result.ray_lod_base_alpha_mask_surfaces,
				result.ray_lod_base_near_field_surfaces,
				result.punctual_lights,
				result.punctual_lights,
				analytic_directional_count,
				analytic_radiance_sum.r,
				analytic_radiance_sum.g,
				analytic_radiance_sum.b,
				result.primary_analytic_selected,
				result.primary_analytic_contributed,
				result.primary_analytic_visibility_tests,
				result.punctual_light_overflow,
				result.unsupported_punctual_lights,
				request.use_metalfx_denoiser ? "MetalFX temporal denoised" : (using_metalfx_temporal ? "MetalFX temporal" : "native-resolution raster"),
				result.rendered_views,
				result.scene.blas_built,
				result.scene.blas_refit,
				result.scene.blas_reused,
				result.blas_builds_deferred,
				result.blas_build_triangles_deferred,
				result.invalid_pdf_samples,
				result.nonfinite_lobe_samples,
				result.rejected_energy_samples));
		print_line(vformat("Flux virtual ray structures: path=%s tiers near/middle/far=%d/%d/%d pending=%d coarse_fallbacks=%d offscreen_retained=%d; BLAS compact query/copy/swap/retire=%d/%d/%d/%d; TLAS rebuild/refit/reuse=%d/%d/%d; compact bytes=%d/%d.",
				result.ray_structure_path,
				result.ray_group_near,
				result.ray_group_middle,
				result.ray_group_far,
				result.ray_group_pending,
				result.ray_group_coarse_fallbacks,
				result.ray_group_off_screen_retained,
				result.blas_compaction_queries,
				result.blas_compactions,
				result.blas_compaction_swaps,
				result.blas_compaction_retirements,
				result.tlas_rebuilds,
				result.tlas_refits,
				result.tlas_reuses,
				result.ray_group_compacted_bytes,
				result.ray_group_uncompacted_bytes));
		metal_flux_diagnostic_reported = true;
	}
	metal_flux_rendered_frames++;
	if (collect_gpu_timings && result.scene.blas_refit > 0 && !metal_flux_refit_reported) {
		print_line(vformat("Flux: dynamic BLAS refit active (%d surface(s) this frame).", result.scene.blas_refit));
		metal_flux_refit_reported = true;
	}
	return true;
}
#endif

void RenderFlux::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	scene_state.used_uniform_buffer_count = 0;

	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	ERR_FAIL_NULL(p_render_data);
	virtual_geometry_render_data = p_render_data;

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RenderBufferDataFlux> rb_data;
	if (rb->has_custom_data(RB_SCOPE_FLUX)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FLUX);
	}
	bool is_reflection_probe = p_render_data->reflection_probe.is_valid();
	bool is_multiview = rb->get_view_count() > 1;

	static const int texture_multisamples[RSE::VIEWPORT_MSAA_MAX] = { 1, 2, 4, 8 };

	//first of all, make a new render pass
	//fill up ubo

	RENDER_TIMESTAMP("Prepare 3D Scene");

	// get info about our rendering effects
	bool ce_needs_motion_vectors = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_MOTION_VECTORS);
	bool ce_needs_normal_roughness = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_ROUGHNESS);
	bool ce_needs_separate_specular = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_SEPARATE_SPECULAR);
	int hybrid_mode = 0;
	const int project_hybrid_mode = GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/enabled") ? 1 : 0;
	const int viewport_hybrid_mode = p_render_data->hybrid_renderer_mode != 0 ? 1 : 0;
	const int requested_hybrid_mode = project_hybrid_mode == 1 && viewport_hybrid_mode == 1 ? 1 : 0;
	if (rb_data.is_valid()) {
		rb_data->set_hybrid_renderer_enabled(requested_hybrid_mode == 1);
	}
#ifdef METAL_ENABLED
	if (!is_reflection_probe && metal_flux_effect && metal_flux_effect->is_supported()) {
		hybrid_mode = requested_hybrid_mode;
		ce_needs_normal_roughness |= hybrid_mode == 1;
		ce_needs_motion_vectors |= hybrid_mode == 1;
	} else if (!is_reflection_probe && requested_hybrid_mode == 1) {
		WARN_PRINT_ONCE("Flux ray tracing requested, but the active Metal device does not support native ray tracing. Using Flux basic raster.");
	}
	if (rb_data.is_valid() && hybrid_mode != 1) {
		// Scaling may still run after Hybrid is disabled. Prevent stale guides/history
		// from selecting the denoised path for an ordinary Forward+ frame.
		rb_data->invalidate_hybrid_history();
		rb_data->set_hybrid_mfx_denoised_active(false, true);
	}
#else
	if (!is_reflection_probe && requested_hybrid_mode == 1) {
		WARN_PRINT_ONCE("Flux ray tracing requested, but this build has no certified native ray-effects backend. Using Flux basic raster; Windows Vulkan/RTX support remains pending.");
	}
#endif
	p_render_data->hybrid_renderer_mode = hybrid_mode;
	if (p_render_data->render_info) {
		if (p_render_data->render_info->flux_last_effective_mode != hybrid_mode) {
#ifdef METAL_ENABLED
			const uint64_t owner = p_render_data->render_info->flux_owner_id;
			metal_flux_pending_diagnostics.erase(owner);
			metal_flux_completed_diagnostics.erase(owner);
			metal_flux_completed_timing_diagnostics.erase(owner);
			metal_flux_timing_capture_states.erase(owner);
#endif
			p_render_data->render_info->flux_last_effective_mode = hybrid_mode;
		}
		p_render_data->render_info->flux.reset_for_frame(p_render_data->render_info->flux_current_frame, hybrid_mode);
	}
	// Flux raster owns deterministic primary ambient/reflection. Ray transport
	// samples a ready Sky for secondary bounce only, so unsupported, late, flat,
	// and no-Sky environments always fail open without stale suppression.
	p_render_data->scene_data->suppress_raster_environment_lighting = false;

	// sdfgi first
	_update_sdfgi(p_render_data);

	// assign render indices to voxel_gi_instances
	for (uint32_t i = 0; i < (uint32_t)p_render_data->voxel_gi_instances->size(); i++) {
		RID voxel_gi_instance = (*p_render_data->voxel_gi_instances)[i];
		gi.voxel_gi_instance_set_render_index(voxel_gi_instance, i);
	}

	// obtain cluster builder
	if (light_storage->owns_reflection_probe_instance(p_render_data->reflection_probe)) {
		current_cluster_builder = light_storage->reflection_probe_instance_get_cluster_builder(p_render_data->reflection_probe, &cluster_builder_shared);

		if (p_render_data->camera_attributes.is_valid()) {
			light_storage->reflection_probe_set_baked_exposure(light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe), RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes));
		}
	} else if (rb_data.is_valid()) {
		current_cluster_builder = rb_data->cluster_builder;

		p_render_data->voxel_gi_count = 0;

		if (rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			if (sdfgi.is_valid()) {
				sdfgi->update_cascades();
				sdfgi->pre_process_gi(p_render_data->scene_data->cam_transform, p_render_data);
				sdfgi->update_light();
			}
		}

		gi.setup_voxel_gi_instances(p_render_data, p_render_data->render_buffers, p_render_data->scene_data->cam_transform, *p_render_data->voxel_gi_instances, p_render_data->voxel_gi_count);
	} else {
		ERR_PRINT("No render buffer nor reflection atlas, bug"); // Should never happen!
		current_cluster_builder = nullptr;
		return; // No point in continuing, we'll just crash.
	}

	ERR_FAIL_NULL(current_cluster_builder);

	p_render_data->cluster_buffer = current_cluster_builder->get_cluster_buffer();
	p_render_data->cluster_size = current_cluster_builder->get_cluster_size();
	p_render_data->cluster_max_elements = current_cluster_builder->get_max_cluster_elements();

	_update_vrs(rb);

	RENDER_TIMESTAMP("Setup 3D Scene");

	bool using_debug_mvs = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS;
	bool using_taa = rb->get_use_taa();

	enum {
		SCALE_NONE,
		SCALE_FSR2,
		SCALE_MFX,
	} scale_type = SCALE_NONE;

	switch (rb->get_scaling_3d_mode()) {
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR2:
			scale_type = SCALE_FSR2;
			break;
		case RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL:
#ifdef METAL_MFXTEMPORAL_ENABLED
			scale_type = SCALE_MFX;
#else
			scale_type = SCALE_NONE;
#endif
			break;
		default:
			break;
	}
	bool using_upscaling = scale_type != SCALE_NONE;

	// check if we need motion vectors
	bool motion_vectors_required;
	if (using_debug_mvs) {
		motion_vectors_required = true;
	} else if (ce_needs_motion_vectors) {
		motion_vectors_required = true;
	} else if (!is_reflection_probe && using_taa) {
		motion_vectors_required = true;
	} else if (!is_reflection_probe && using_upscaling) {
		motion_vectors_required = true;
	} else {
		motion_vectors_required = false;
	}

	//p_render_data->scene_data->subsurface_scatter_width = subsurface_scatter_size;
	p_render_data->scene_data->calculate_motion_vectors = motion_vectors_required;
	p_render_data->scene_data->directional_light_count = 0;
	p_render_data->scene_data->opaque_prepass_threshold = 0.99f;

	Size2i screen_size;
	RID color_framebuffer;
	RID color_only_framebuffer;
	RID depth_framebuffer;
	RendererRD::MaterialStorage::Samplers samplers;

	PassMode depth_pass_mode = PASS_MODE_DEPTH;
	uint32_t color_pass_flags = 0;
	Vector<Color> depth_pass_clear;
	bool using_separate_specular = false;
	bool using_ssr = false;
	bool using_sdfgi = false;
	bool using_voxelgi = false;
	bool reverse_cull = p_render_data->scene_data->cam_transform.basis.determinant() < 0;
	bool using_ssil = hybrid_mode != 1 && !is_reflection_probe && p_render_data->environment.is_valid() && environment_get_ssil_enabled(p_render_data->environment);
	bool using_motion_pass = rb_data.is_valid() && using_upscaling;

	if (is_reflection_probe) {
		uint32_t resolution = light_storage->reflection_probe_instance_get_resolution(p_render_data->reflection_probe);
		screen_size.x = resolution;
		screen_size.y = resolution;

		color_framebuffer = light_storage->reflection_probe_instance_get_framebuffer(p_render_data->reflection_probe, p_render_data->reflection_probe_pass);
		color_only_framebuffer = color_framebuffer;
		depth_framebuffer = light_storage->reflection_probe_instance_get_depth_framebuffer(p_render_data->reflection_probe, p_render_data->reflection_probe_pass);

		if (light_storage->reflection_probe_is_interior(light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe))) {
			p_render_data->environment = RID(); //no environment on interiors
		}

		reverse_cull = true; // for some reason our views are inverted
		samplers = RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default();

		// Indicate pipelines for reflection probes are required.
		global_pipeline_data_required.use_reflection_probes = true;
	} else {
		screen_size = rb->get_internal_size();

		if (p_render_data->scene_data->calculate_motion_vectors) {
			color_pass_flags |= COLOR_PASS_FLAG_MOTION_VECTORS;
			scene_shader.enable_advanced_shader_group();

			// Indicate pipelines for motion vectors are required.
			global_pipeline_data_required.use_motion_vectors = true;
		}

		if (hybrid_mode == 1) {
			color_pass_flags |= COLOR_PASS_FLAG_PRIMARY_SURFACE;
			scene_shader.enable_advanced_shader_group();
		}

		if (p_render_data->voxel_gi_instances->size() > 0) {
			using_voxelgi = true;
		}

		if (p_render_data->environment.is_valid()) {
			if (hybrid_mode != 1 && environment_get_sdfgi_enabled(p_render_data->environment) && get_debug_draw_mode() != RSE::VIEWPORT_DEBUG_DRAW_UNSHADED) {
				using_sdfgi = true;
			}
			if (hybrid_mode != 1 && environment_get_ssr_enabled(p_render_data->environment)) {
				if (!p_render_data->transparent_bg) {
					using_ssr = true;
				} else {
					WARN_PRINT_ONCE("Screen-space reflections are not supported in viewports with a transparent background. Disabling SSR in transparent viewport.");
				}
			}
		}

		if (p_render_data->scene_data->view_count > 1) {
			color_pass_flags |= COLOR_PASS_FLAG_MULTIVIEW;
			// Try enabling here in case is_xr_enabled() returns false.
			scene_shader.shader.enable_group(SceneShaderFlux::SHADER_GROUP_MULTIVIEW);

			// Indicate pipelines for multiview are required.
			global_pipeline_data_required.use_multiview = true;
		}

		color_framebuffer = rb_data->get_color_pass_fb(color_pass_flags);
		color_only_framebuffer = rb_data->get_color_only_fb();
		samplers = rb->get_samplers();
	}

	p_render_data->scene_data->emissive_exposure_normalization = -1.0;

	RD::get_singleton()->draw_command_begin_label("Render Setup");

	_setup_lightmaps(p_render_data, *p_render_data->lightmaps, p_render_data->scene_data->cam_transform);
	_setup_voxelgis(*p_render_data->voxel_gi_instances);
	uint32_t depth_prepass_uniform_buffer_index = _setup_environment(p_render_data, is_reflection_probe, screen_size, screen_size, p_default_bg_color, false);

	// May have changed due to the above (light buffer enlarged, as an example).
	_update_render_base_uniform_set();

	_fill_render_list(RENDER_LIST_OPAQUE, p_render_data, PASS_MODE_COLOR, using_sdfgi, using_sdfgi || using_voxelgi, using_motion_pass);
	render_list[RENDER_LIST_OPAQUE].sort_by_key();
	render_list[RENDER_LIST_MOTION].sort_by_key();
	render_list[RENDER_LIST_ALPHA].sort_by_reverse_depth_and_priority();

	int *render_info = p_render_data->render_info ? p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE] : (int *)nullptr;
	_fill_instance_data(RENDER_LIST_OPAQUE, render_info);
	_fill_instance_data(RENDER_LIST_MOTION, render_info);
	_fill_instance_data(RENDER_LIST_ALPHA, render_info);
	_append_virtual_geometry_instance_data(p_render_data);

	RD::get_singleton()->draw_command_end_label();

	if (!is_reflection_probe) {
		// PrimarySurfaceV1 is rasterized so primary visibility and material
		// reconstruction do not depend on acceleration-structure admission.
		const bool raster_hybrid_primary = hybrid_mode == 1;
		if (raster_hybrid_primary) {
			scene_shader.enable_advanced_shader_group();
			depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL;
		} else if (using_voxelgi) {
			depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI;
		} else if (p_render_data->environment.is_valid()) {
			if (using_ssr ||
					using_sdfgi ||
					environment_get_ssao_enabled(p_render_data->environment) ||
					using_ssil ||
					ce_needs_normal_roughness ||
					get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER ||
					scene_state.used_normal_texture) {
				depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS;
			}
		} else if (ce_needs_normal_roughness || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER || scene_state.used_normal_texture) {
			depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS;
		}

		switch (depth_pass_mode) {
			case PASS_MODE_DEPTH: {
				depth_framebuffer = rb_data->get_depth_fb();
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
				depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataFlux::DEPTH_FB_ROUGHNESS);
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
				depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataFlux::DEPTH_FB_ROUGHNESS_VOXELGI);
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL: {
				depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataFlux::DEPTH_FB_ROUGHNESS_HYBRID_MATERIAL);
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
				depth_pass_clear.push_back(Color(0, 0, 0, 0));
			} break;
			default: {
			};
		}
	}

	bool using_sss = rb_data.is_valid() && !is_reflection_probe && scene_state.used_sss && ss_effects->sss_get_quality() != RSE::SUB_SURFACE_SCATTERING_QUALITY_DISABLED;
	// SSS and separate-specular closures are outside PrimarySurfaceV1. Opaque
	// pixels use the deterministic canonical approximation; transparent
	// overlays retain the ordinary raster path below.
	if (hybrid_mode == 1) {
		using_sss = false;
		ce_needs_separate_specular = false;
	}

	if (using_sss && p_render_data->transparent_bg) {
		WARN_PRINT_ONCE("Sub-surface scattering is not supported in viewports with a transparent background. Disabling SSS in transparent viewport.");
		using_sss = false;
	}

	if ((using_sss || ce_needs_separate_specular) && !using_separate_specular) {
		using_separate_specular = true;
		color_pass_flags |= COLOR_PASS_FLAG_SEPARATE_SPECULAR;
		color_framebuffer = rb_data->get_color_pass_fb(color_pass_flags);
	}

	// Ensure this is allocated so we don't get a stutter the first time an object with SSS appears on screen.
	if (global_surface_data.sss_used && !is_reflection_probe) {
		rb_data->ensure_specular();
	}

	if (global_surface_data.normal_texture_used && !is_reflection_probe) {
		rb_data->ensure_normal_roughness_texture();
	}

	if (using_sss || using_separate_specular || scene_state.used_lightmap || using_voxelgi || global_surface_data.sss_used) {
		scene_shader.enable_advanced_shader_group(p_render_data->scene_data->view_count > 1);
	}

	// Update the global pipeline requirements with all the features found to be in use in this scene.
	if (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL || global_surface_data.normal_texture_used) {
		global_pipeline_data_required.use_normal_and_roughness = true;
	}

	if (scene_state.used_lightmap || scene_state.lightmaps_used > 0) {
		global_pipeline_data_required.use_lightmaps = true;
	}

	if (using_voxelgi) {
		global_pipeline_data_required.use_voxelgi = true;
	}

	if (using_separate_specular || global_surface_data.sss_used) {
		global_pipeline_data_required.use_separate_specular = true;
	}

	// Update the compiled pipelines if any of the requirements have changed.
	_update_dirty_geometry_pipelines();

	RID radiance_texture;
	bool draw_sky = false;
	bool draw_sky_fog_only = false;
	// We invert luminance_multiplier for sky so that we can combine it with exposure value.
	float sky_luminance_multiplier = 1.0 / rb->get_luminance_multiplier();
	float sky_brightness_multiplier = 1.0;

	Color clear_color;
	bool load_color = false;

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		clear_color = Color(0, 0, 0, 1); //in overdraw mode, BG should always be black
	} else if (is_environment(p_render_data->environment)) {
		RSE::EnvironmentBG bg_mode = environment_get_background(p_render_data->environment);
		float bg_energy_multiplier = environment_get_bg_energy_multiplier(p_render_data->environment);
		bg_energy_multiplier *= environment_get_bg_intensity(p_render_data->environment);
		RSE::EnvironmentReflectionSource reflection_source = environment_get_reflection_source(p_render_data->environment);

		if (p_render_data->camera_attributes.is_valid()) {
			bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}

		switch (bg_mode) {
			case RSE::ENV_BG_CLEAR_COLOR:
			case RSE::ENV_BG_COLOR: {
				clear_color = bg_mode == RSE::ENV_BG_CLEAR_COLOR ? p_default_bg_color : environment_get_bg_color(p_render_data->environment);

				if (!p_render_data->transparent_bg && (rb->has_custom_data(RB_SCOPE_FOG) || environment_get_fog_enabled(p_render_data->environment))) {
					draw_sky_fog_only = true;
					RendererRD::MaterialStorage::get_singleton()->material_set_param(sky.sky_scene_state.fog_material, "clear_color", Variant(clear_color));
				}

				clear_color = clear_color.srgb_to_linear();
				clear_color.r *= bg_energy_multiplier;
				clear_color.g *= bg_energy_multiplier;
				clear_color.b *= bg_energy_multiplier;
			} break;
			case RSE::ENV_BG_SKY: {
				draw_sky = !p_render_data->transparent_bg;
			} break;
			case RSE::ENV_BG_CANVAS: {
				if (!is_reflection_probe) {
					RID texture = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_texture(rb->get_render_target());
					bool convert_to_linear = !RendererRD::TextureStorage::get_singleton()->render_target_is_using_hdr(rb->get_render_target());
					copy_effects->copy_to_fb_rect(texture, color_only_framebuffer, Rect2i(), false, false, false, false, RID(), false, false, convert_to_linear);
				}
				load_color = true;
			} break;
			case RSE::ENV_BG_KEEP: {
				load_color = true;
			} break;
			case RSE::ENV_BG_CAMERA_FEED: {
			} break;
			default: {
			}
		}

		// setup sky if used for ambient, reflections, or background
		const bool hybrid_environment_requested = hybrid_mode == 1 && GLOBAL_GET_CACHED(bool, "rendering/flux/ray_tracing/environment_lighting/enabled");
		if (draw_sky || draw_sky_fog_only || (reflection_source == RSE::ENV_REFLECTION_SOURCE_BG && bg_mode == RSE::ENV_BG_SKY) || reflection_source == RSE::ENV_REFLECTION_SOURCE_SKY || environment_get_ambient_source(p_render_data->environment) == RSE::ENV_AMBIENT_SOURCE_SKY || hybrid_environment_requested) {
			RENDER_TIMESTAMP("Setup Sky");
			RD::get_singleton()->draw_command_begin_label("Setup Sky");

			// Setup our sky render information for this frame/viewport
			sky.setup_sky(p_render_data, screen_size);

			sky_brightness_multiplier *= bg_energy_multiplier;

			RID sky_rid = environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				sky.update_radiance_buffers(rb, p_render_data->environment, p_render_data->scene_data->cam_transform.origin, time, sky_luminance_multiplier, sky_brightness_multiplier);
				radiance_texture = sky.sky_get_radiance_texture_rd(sky_rid);
			} else {
				// do not try to draw sky if invalid
				draw_sky = false;
			}

			if (draw_sky || draw_sky_fog_only) {
				// update sky half/quarter res buffers (if required)
				sky.update_res_buffers(rb, p_render_data->environment, time, sky_luminance_multiplier, sky_brightness_multiplier);
			}

			RD::get_singleton()->draw_command_end_label();
		}

		if (bg_mode != RSE::ENV_BG_CLEAR_COLOR && bg_mode != RSE::ENV_BG_COLOR) {
			clear_color = clear_color.srgb_to_linear();
		}
	} else {
		clear_color = p_default_bg_color.srgb_to_linear();
	}

	// After this point clear_color has linear encoding.

	RSE::ViewportMSAA msaa = rb->get_msaa_3d();
	bool use_msaa = msaa != RSE::VIEWPORT_MSAA_DISABLED && hybrid_mode != 1;

	bool ce_pre_opaque_resolved_color = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	bool ce_post_opaque_resolved_color = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	bool ce_pre_transparent_resolved_color = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);

	bool ce_pre_opaque_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	bool ce_post_opaque_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	bool ce_pre_transparent_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);

	bool debug_voxelgis = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_ALBEDO || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_LIGHTING || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_EMISSION;
	bool debug_sdfgi_probes = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SDFGI_PROBES;
	bool force_depth_pre_pass = scene_state.used_opaque_stencil || hybrid_mode == 1;
	if (rb_data.is_valid()) {
		rb_data->set_primary_surface_v1_valid(false);
	}
	bool depth_pre_pass = (force_depth_pre_pass || bool(GLOBAL_GET_CACHED(bool, "rendering/driver/depth_prepass/enable"))) && depth_framebuffer.is_valid();

	SceneShaderFlux::ShaderSpecialization base_specialization = scene_shader.default_specialization;
	base_specialization.use_depth_fog = p_render_data->environment.is_valid() && environment_get_fog_mode(p_render_data->environment) == RSE::EnvironmentFogMode::ENV_FOG_MODE_DEPTH;

	bool using_ssao = hybrid_mode != 1 && depth_pre_pass && !is_reflection_probe && p_render_data->environment.is_valid() && environment_get_ssao_enabled(p_render_data->environment);

	if (depth_pre_pass) { //depth pre pass
		bool needs_pre_resolve = _needs_post_prepass_render(p_render_data, using_sdfgi || using_voxelgi);
		if (needs_pre_resolve) {
			RENDER_TIMESTAMP("GI + Render Depth Pre-Pass (Parallel)");
		} else {
			RENDER_TIMESTAMP("Render Depth Pre-Pass");
		}
		if (needs_pre_resolve) {
			//pre clear the depth framebuffer, as AMD (and maybe others?) use compute for it, and barrier other compute shaders.
			RD::get_singleton()->draw_list_begin(depth_framebuffer, RD::DRAW_CLEAR_ALL, depth_pass_clear, 0.0f);
			RD::get_singleton()->draw_list_end();
			//start compute processes here, so they run at the same time as depth pre-pass
			_post_prepass_render(p_render_data, using_sdfgi || using_voxelgi);
		}

		RD::get_singleton()->draw_command_begin_label("Render Depth Pre-Pass");

		RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, nullptr, is_multiview, RID(), samplers, depth_prepass_uniform_buffer_index);

		bool finish_depth = using_ssao || using_ssil || using_sdfgi || using_voxelgi || ce_pre_opaque_resolved_depth || ce_post_opaque_resolved_depth;
		RenderListParameters render_list_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), reverse_cull, depth_pass_mode, 0, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
		_render_list_with_draw_list(&render_list_params, depth_framebuffer, RD::DrawFlags(needs_pre_resolve ? RD::DRAW_DEFAULT_ALL : RD::DRAW_CLEAR_ALL), depth_pass_clear, 0.0f, 0u, p_render_data->render_region);
		if (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_HYBRID_MATERIAL) {
			rb_data->set_primary_surface_v1_valid(true);
		}

		RD::get_singleton()->draw_command_end_label();

		if (use_msaa) {
			RENDER_TIMESTAMP("Resolve Depth Pre-Pass (MSAA)");
			RD::get_singleton()->draw_command_begin_label("Resolve Depth Pre-Pass (MSAA)");
			if (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					resolve_effects->resolve_gi(rb->get_depth_msaa(v), rb_data->get_normal_roughness_msaa(v), using_voxelgi ? rb_data->get_voxelgi_msaa(v) : RID(), rb->get_depth_texture(v), rb_data->get_normal_roughness(v), using_voxelgi ? rb_data->get_voxelgi(v) : RID(), rb->get_internal_size(), texture_multisamples[msaa]);
				}
			} else if (finish_depth) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
				}
			}
			RD::get_singleton()->draw_command_end_label();
		}
	}

#ifdef METAL_ENABLED
	p_render_data->scene_data->hybrid_raytraced_directional_shadow = false;
	// Ray-on opaque shading is owned by the full Flux transport dispatch below.
	// Do not build a second raster-light shadow replacement or bind its result to
	// the material-only PrimarySurfaceV1 pass.
#endif

	{
		if (ce_pre_opaque_resolved_color) {
			// We haven't rendered color data yet so...
			WARN_PRINT_ONCE("Pre opaque rendering effects can't access resolved color buffers.");
		}

		if (ce_pre_opaque_resolved_depth && !depth_pre_pass) {
			// We haven't rendered depth data yet so...
			WARN_PRINT_ONCE("Pre opaque rendering effects can't access resolved depth buffers.");
		}

		RENDER_TIMESTAMP("Process Pre Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE, p_render_data);
	}

	RID normal_roughness_views[RendererSceneRender::MAX_RENDER_VIEWS];
	if (rb_data.is_valid() && rb_data->has_normal_roughness()) {
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			normal_roughness_views[v] = rb_data->get_normal_roughness(v);
		}
	}
	_pre_opaque_render(p_render_data, using_ssao, using_ssil, using_ssr, using_sdfgi || using_voxelgi, hybrid_mode != 1, normal_roughness_views, rb_data.is_valid() && rb_data->has_voxelgi() ? rb_data->get_voxelgi() : RID());

	if (current_cluster_builder) {
		base_specialization.cluster_has_area_light = current_cluster_builder->get_cluster_count_by_type(ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT) != 0;
	}

	RENDER_TIMESTAMP("Render Opaque Pass");

	RD::get_singleton()->draw_command_begin_label("Render Opaque Pass");

	p_render_data->scene_data->directional_light_count = p_render_data->directional_light_count;
	p_render_data->scene_data->opaque_prepass_threshold = 0.0f;

	// Shadow pass can change the base uniform set samplers.
	_update_render_base_uniform_set();

	uint32_t opaque_pass_uniform_buffer_index = _setup_environment(p_render_data, is_reflection_probe, screen_size, screen_size, p_default_bg_color, true, using_motion_pass);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, p_render_data, is_multiview, hybrid_mode == 1 ? RID() : radiance_texture, samplers, opaque_pass_uniform_buffer_index, true);

	{
		bool render_motion_pass = !render_list[RENDER_LIST_MOTION].elements.is_empty();

		{
			Vector<Color> c;
			if (!load_color) {
				if (using_separate_specular || rb_data.is_valid()) {
					// Effects that rely on separate specular, like subsurface scattering, must clear the alpha to zero.
					clear_color.a = 0;
				}
				c.push_back(clear_color);

				if (rb_data.is_valid()) {
					c.push_back(Color(0, 0, 0, 0)); // Separate specular.
					c.push_back(Color(0, 0, 0, 0)); // Motion vector. Pushed to the clear color vector even if the framebuffer isn't bound.
				}
			}

			uint32_t opaque_color_pass_flags = using_motion_pass ? (color_pass_flags & ~uint32_t(COLOR_PASS_FLAG_MOTION_VECTORS)) : color_pass_flags;
			RID opaque_framebuffer = using_motion_pass ? rb_data->get_color_pass_fb(opaque_color_pass_flags) : color_framebuffer;
			RenderListParameters render_list_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), reverse_cull, PASS_MODE_COLOR, opaque_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
			_render_list_with_draw_list(&render_list_params, opaque_framebuffer, RD::DrawFlags(load_color ? RD::DRAW_DEFAULT_ALL : RD::DRAW_CLEAR_COLOR_ALL) | (depth_pre_pass ? RD::DRAW_DEFAULT_ALL : RD::DRAW_CLEAR_DEPTH), c, 0.0f, 0u, p_render_data->render_region);
		}

		RD::get_singleton()->draw_command_end_label();

		if (using_motion_pass) {
			if (scale_type == SCALE_MFX) {
				motion_vectors_store->process(rb,
						p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform,
						p_render_data->scene_data->prev_cam_projection, p_render_data->scene_data->prev_cam_transform);
			} else {
				Vector<Color> motion_vector_clear_colors;
				motion_vector_clear_colors.push_back(Color(-1, -1, 0, 0));
				RD::get_singleton()->draw_list_begin(rb_data->get_velocity_only_fb(), RD::DRAW_CLEAR_ALL, motion_vector_clear_colors);
				RD::get_singleton()->draw_list_end();
			}
		}

		if (render_motion_pass) {
			RD::get_singleton()->draw_command_begin_label("Render Motion Pass");

			RENDER_TIMESTAMP("Render Motion Pass");

			rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_MOTION, p_render_data, is_multiview, hybrid_mode == 1 ? RID() : radiance_texture, samplers, opaque_pass_uniform_buffer_index, true);

			RenderListParameters render_list_params(render_list[RENDER_LIST_MOTION].elements.ptr(), render_list[RENDER_LIST_MOTION].element_info.ptr(), render_list[RENDER_LIST_MOTION].elements.size(), reverse_cull, PASS_MODE_COLOR, color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
			_render_list_with_draw_list(&render_list_params, color_framebuffer);

			RD::get_singleton()->draw_command_end_label();
		}
	}

	{
		if (ce_post_opaque_resolved_color) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
			}
		}

		if (ce_post_opaque_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}

		RENDER_TIMESTAMP("Process Post Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, p_render_data);
	}

	if (debug_voxelgis) {
		Projection dc;
		dc.set_depth_correction(true);
		Projection cm = (dc * p_render_data->scene_data->cam_projection) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_only_framebuffer);
		RD::get_singleton()->draw_command_begin_label("Debug VoxelGIs");
		for (int i = 0; i < (int)p_render_data->voxel_gi_instances->size(); i++) {
			gi.debug_voxel_gi((*p_render_data->voxel_gi_instances)[i], draw_list, color_only_framebuffer, cm, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_LIGHTING, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_EMISSION, 1.0);
		}
		RD::get_singleton()->draw_command_end_label();
		RD::get_singleton()->draw_list_end();
	}

	if (debug_sdfgi_probes) {
		Projection dc;
		dc.set_depth_correction(true);
		Projection cms[RendererSceneRender::MAX_RENDER_VIEWS];
		for (uint32_t v = 0; v < p_render_data->scene_data->view_count; v++) {
			cms[v] = (dc * p_render_data->scene_data->view_projection[v]) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
		}
		_debug_sdfgi_probes(rb, color_only_framebuffer, p_render_data->scene_data->view_count, cms);
	}

	if (draw_sky || draw_sky_fog_only) {
		RENDER_TIMESTAMP("Render Sky");

		RD::get_singleton()->draw_command_begin_label("Draw Sky");
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_only_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);

		sky.draw_sky(draw_list, rb, p_render_data->environment, color_only_framebuffer, time, sky_luminance_multiplier, sky_brightness_multiplier);

		RD::get_singleton()->draw_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	if (use_msaa) {
		RENDER_TIMESTAMP("Resolve MSAA");

		if (scene_state.used_screen_texture || using_separate_specular || ce_pre_transparent_resolved_color || hybrid_mode == 1) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
			}
			if (using_separate_specular) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					RD::get_singleton()->texture_resolve_multisample(rb_data->get_specular_msaa(v), rb_data->get_specular(v));
				}
			}
		}

		if (scene_state.used_depth_texture || scene_state.used_normal_texture || using_separate_specular || ce_needs_normal_roughness || ce_pre_transparent_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}
		if (hybrid_mode == 1 && rb->has_velocity_buffer(true)) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_velocity_buffer(true, v), rb->get_velocity_buffer(false, v));
			}
		}
	}

#ifdef METAL_ENABLED
	if (hybrid_mode == 1) {
		RENDER_TIMESTAMP("Metal Hybrid Ray Effects");
		RD::get_singleton()->draw_command_begin_label("Metal Hybrid Ray Effects");
		_process_metal_flux(p_render_data, rb_data, false);
		RD::get_singleton()->draw_command_end_label();
	}
#endif

	{
		RENDER_TIMESTAMP("Process Post Sky Compositor Effects");
		// Don't need to check for depth or color resolve here, we've already triggered it.
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY, p_render_data);
	}

	if (using_separate_specular) {
		if (using_sss) {
			RENDER_TIMESTAMP("Sub-Surface Scattering");
			RD::get_singleton()->draw_command_begin_label("Process Sub-Surface Scattering");
			_process_sss(rb, p_render_data->scene_data->cam_projection);
			RD::get_singleton()->draw_command_end_label();
		}

		{
			//just mix specular back
			RENDER_TIMESTAMP("Merge Specular");
			copy_effects->merge_specular(color_only_framebuffer, rb_data->get_specular(), !use_msaa ? RID() : rb->get_internal_texture(), RID(), p_render_data->scene_data->view_count);
		}
	}

	if (using_separate_specular && is_environment(p_render_data->environment) && (environment_get_background(p_render_data->environment) == RSE::ENV_BG_CANVAS)) {
		// Canvas background mode does not clear the color buffer, but copies over it. If screen-space specular effects are enabled and the background is blank,
		// this results in ghosting due to the separate specular buffer copy. Need to explicitly clear the specular buffer once we're done with it to fix it.
		RENDER_TIMESTAMP("Clear Separate Specular (Canvas Background Mode)");
		Vector<Color> blank_clear_color;
		blank_clear_color.push_back(Color(0.0, 0.0, 0.0));
		RD::get_singleton()->draw_list_begin(rb_data->get_specular_only_fb(), RD::DRAW_CLEAR_ALL, blank_clear_color);
		RD::get_singleton()->draw_list_end();
	}

	if (rb_data.is_valid() && using_upscaling) {
		// Make sure the upscaled texture is initialized, but not necessarily filled, before running screen copies
		// so it properly detect if a dedicated copy texture should be used.
		rb->ensure_upscaled();
	}

	if (scene_state.used_screen_texture || global_surface_data.screen_texture_used) {
		RENDER_TIMESTAMP("Copy Screen Texture");

		_render_buffers_ensure_screen_texture(p_render_data);

		if (scene_state.used_screen_texture) {
			// Copy screen texture to backbuffer so we can read from it
			_render_buffers_copy_screen_texture(p_render_data);
		}
	}

	if (scene_state.used_depth_texture || global_surface_data.depth_texture_used) {
		RENDER_TIMESTAMP("Copy Depth Texture");

		_render_buffers_ensure_depth_texture(p_render_data);

		if (scene_state.used_depth_texture) {
			// Copy depth texture to backbuffer so we can read from it
			_render_buffers_copy_depth_texture(p_render_data);
		}
	}

	{
		if (using_separate_specular) {
			// Our specular will be combined back in (and effects, subsurface scattering and/or ssr applied),
			// so if we've requested this, we need another copy.
			// Fairly unlikely scenario though.

			if (ce_pre_transparent_resolved_color) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
				}
			}

			if (ce_pre_transparent_resolved_depth) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
				}
			}
		}

		RENDER_TIMESTAMP("Process Pre Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT, p_render_data);
	}

	RENDER_TIMESTAMP("Render 3D Transparent Pass");

	RD::get_singleton()->draw_command_begin_label("Render 3D Transparent Pass");

	uint32_t transparent_pass_uniform_buffer_index = _setup_environment(p_render_data, is_reflection_probe, screen_size, screen_size, p_default_bg_color, false);

	rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, is_multiview, radiance_texture, samplers, transparent_pass_uniform_buffer_index, true);
	bool hybrid_transparency_overlay_rendered = false;
	uint32_t hybrid_transparency_color_pass_flags = 0;

	{
		uint32_t transparent_color_pass_flags = (color_pass_flags | uint32_t(COLOR_PASS_FLAG_TRANSPARENT)) & ~(uint32_t(COLOR_PASS_FLAG_SEPARATE_SPECULAR) | uint32_t(COLOR_PASS_FLAG_PRIMARY_SURFACE));
		// Motion vectors should not be overwritten by transparent objects.
		transparent_color_pass_flags &= ~uint32_t(COLOR_PASS_FLAG_MOTION_VECTORS);

		// Hybrid reserves this target for linear premultiplied alpha. The
		// opaque base has already been traced into the MetalFX color input, so
		// composing alpha there would denoise it twice. MetalFX consumes this
		// overlay exactly once after reconstruction.
		const bool use_hybrid_transparency_overlay = hybrid_mode == 1 && rb_data.is_valid() && rb_data->is_hybrid_mfx_denoised_active();
		hybrid_transparency_overlay_rendered = use_hybrid_transparency_overlay;
		hybrid_transparency_color_pass_flags = transparent_color_pass_flags;
		RID alpha_framebuffer = use_hybrid_transparency_overlay ? rb_data->get_hybrid_transparency_fb() : (rb_data.is_valid() ? rb_data->get_color_pass_fb(transparent_color_pass_flags) : color_only_framebuffer);
		Vector<Color> alpha_clear;
		if (use_hybrid_transparency_overlay) {
			alpha_clear.push_back(Color(0.0, 0.0, 0.0, 0.0));
		}
		RenderListParameters render_list_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), reverse_cull, PASS_MODE_COLOR, transparent_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
		_render_list_with_draw_list(&render_list_params, alpha_framebuffer, use_hybrid_transparency_overlay ? RD::DRAW_CLEAR_COLOR_ALL : RD::DRAW_DEFAULT_ALL, alpha_clear, 0.0f, 0u, p_render_data->render_region);
	}

	RD::get_singleton()->draw_command_end_label();

	RENDER_TIMESTAMP("Resolve");

	RD::get_singleton()->draw_command_begin_label("Resolve");

	if (rb_data.is_valid() && use_msaa) {
		bool resolve_velocity_buffer = (using_taa || using_upscaling || ce_needs_motion_vectors) && rb->has_velocity_buffer(true);
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
			resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);

			if (resolve_velocity_buffer) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_velocity_buffer(true, v), rb->get_velocity_buffer(false, v));
			}
		}
	}

	RD::get_singleton()->draw_command_end_label();

	RD::get_singleton()->draw_command_begin_label("Copy Framebuffer for SSIL/SSR");
	if (using_ssil || using_ssr) {
		RENDER_TIMESTAMP("Copy Final Framebuffer (SSIL/SSR)");
		_copy_framebuffer_to_ss_effects(rb, using_ssil, using_ssr);
	}
	RD::get_singleton()->draw_command_end_label();

	{
		RENDER_TIMESTAMP("Process Post Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, p_render_data);
	}

	if (rb_data.is_valid() && (using_upscaling || using_taa)) {
		if (scale_type == SCALE_FSR2) {
			rb_data->ensure_fsr2(fsr2_effect);

			RID exposure;
			if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
				exposure = luminance->get_current_luminance_buffer(rb);
			}

			RD::get_singleton()->draw_command_begin_label("FSR2");
			RENDER_TIMESTAMP("FSR2");

			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				real_t fov = p_render_data->scene_data->cam_projection.get_fov();
				real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
				real_t fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);
				Vector2 jitter = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f;
				RendererRD::FSR2Effect::Parameters params;
				params.context = rb_data->get_fsr2_context();
				params.internal_size = rb->get_internal_size();
				params.sharpness = CLAMP(1.0f - (rb->get_fsr_sharpness() / 2.0f), 0.0f, 1.0f);
				params.color = rb->get_internal_texture(v);
				params.depth = rb->get_depth_texture(v);
				params.velocity = rb->get_velocity_buffer(false, v);
				params.reactive = rb->get_internal_texture_reactive(v);
				params.exposure = exposure;
				params.output = rb->get_upscaled_texture(v);
				params.z_near = p_render_data->scene_data->z_near;
				params.z_far = p_render_data->scene_data->z_far;
				params.fovy = fovy;
				params.jitter = jitter;
				params.delta_time = float(time_step);
				params.reset_accumulation = false; // FIXME: The engine does not provide a way to reset the accumulation.

				Projection correction;
				correction.set_depth_correction(true, true, false);

				const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;
				const Projection &cur_proj = p_render_data->scene_data->cam_projection;
				const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;
				const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;
				params.reprojection = (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform * (correction * cur_proj).inverse();

				fsr2_effect->upscale(params);
			}

			RD::get_singleton()->draw_command_end_label();
		} else if (scale_type == SCALE_MFX) {
#ifdef METAL_MFXTEMPORAL_ENABLED
			// Scale to ±0.5. MetalFX uses a bottom-left origin for jitter.
			Vector2 jitter = p_render_data->scene_data->taa_jitter * Vector2(0.5f, -0.5f);
			bool denoised_scheduled = rb_data->is_hybrid_mfx_denoised_active();
			if (denoised_scheduled) {
				RD::get_singleton()->draw_command_begin_label("MetalFX Temporal Denoised");
				// MetalFX submission is an external scaler call rather than a Flux
				// counter-sampled encoder. Keep an exact submission bracket for the
				// owner/frame diagnostics; GPU duration remains explicitly unavailable.
				RENDER_TIMESTAMP("MetalFX Temporal Denoised Submission Begin");
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					const Transform3D world_from_view = p_render_data->scene_data->cam_transform * Transform3D(Basis(), p_render_data->scene_data->view_eye_offset[v]);
					Projection metalfx_projection_correction;
					metalfx_projection_correction.set_depth_correction(p_render_data->scene_data->flip_y);
					RendererRD::MFXDenoisedEffect::Params params;
					params.color = rb->get_internal_texture(v);
					params.depth = rb->get_depth_texture(v);
					params.motion = rb->get_velocity_buffer(false, v);
					params.normal = rb_data->get_hybrid_guide_normal(v);
					params.diffuse = rb_data->get_hybrid_guide_diffuse(v);
					params.specular = rb_data->get_hybrid_guide_specular(v);
					params.roughness = rb_data->get_hybrid_guide_roughness(v);
					params.denoise_strength = rb_data->get_hybrid_guide_denoise_strength(v);
					params.reactive = rb_data->get_hybrid_guide_reactive(v);
					params.specular_distance = rb_data->get_hybrid_guide_specular_distance(v);
					params.transparency = rb_data->get_hybrid_guide_transparency(v);
					params.output = rb->get_upscaled_texture(v);
					params.view_from_world = _mfx_matrix_from_transform(world_from_view.affine_inverse());
					// MetalFX receives jitter separately, but its projection must still use
					// the same Y/depth device convention as the supplied depth texture.
					params.clip_from_view = _mfx_matrix_from_projection(metalfx_projection_correction * p_render_data->scene_data->view_projection[v]);
					params.jitter_offset = jitter;
					params.motion_vector_scale = rb->get_internal_size();
					// MFXDenoisedEffect enables MetalFX auto exposure. Its explicit
					// pre-exposure input therefore remains neutral while the raw HDR
					// Flux color is denoised/scaled by MetalFX itself.
					params.pre_exposure = 1.0f;
					params.reset = rb_data->should_reset_hybrid_mfx_denoised();
					String error;
					if (mfx_denoised_effect->process(rb_data->get_mfx_denoised_context(v), params, &error) != OK) {
						WARN_PRINT_ONCE("Flux: MetalFX temporal denoising could not be scheduled; using ordinary MetalFX temporal scaling. " + error);
						denoised_scheduled = false;
						break;
					}
				}
				RENDER_TIMESTAMP("MetalFX Temporal Denoised Submission End");
				RD::get_singleton()->draw_command_end_label();
				if (denoised_scheduled) {
					rb_data->clear_hybrid_mfx_denoised_reset();
				}
			}

			if (!denoised_scheduled) {
				// Alpha was intentionally rendered only into MetalFX's separate
				// overlay. If that scheduling path failed, replay the alpha list once
				// into the ordinary scaler input; otherwise transparency would vanish.
				if (hybrid_transparency_overlay_rendered) {
					RD::get_singleton()->draw_command_begin_label("Hybrid Transparency Fallback Composite");
					RID fallback_alpha_framebuffer = rb_data->get_color_pass_fb(hybrid_transparency_color_pass_flags);
					RenderListParameters fallback_alpha_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), reverse_cull, PASS_MODE_COLOR, hybrid_transparency_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
					_render_list_with_draw_list(&fallback_alpha_params, fallback_alpha_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);
					RD::get_singleton()->draw_command_end_label();
					hybrid_transparency_overlay_rendered = false; // Exactly-once fallback composition.
				}
				const bool reset = rb_data->ensure_mfx_temporal(mfx_temporal_effect);
				RID exposure;
				if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
					exposure = luminance->get_current_luminance_buffer(rb);
				}
				RD::get_singleton()->draw_command_begin_label("MetalFX Temporal");
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					RendererRD::MFXTemporalEffect::Params params;
					params.src = rb->get_internal_texture(v);
					params.depth = rb->get_depth_texture(v);
					params.motion = rb->get_velocity_buffer(false, v);
					params.exposure = exposure;
					params.dst = rb->get_upscaled_texture(v);
					params.jitter_offset = jitter;
					params.reset = reset;
					mfx_temporal_effect->process(rb_data->get_mfx_temporal_context(), params);
				}
				RD::get_singleton()->draw_command_end_label();
			}
#endif
		} else if (using_taa) {
			RD::get_singleton()->draw_command_begin_label("TAA");
			RENDER_TIMESTAMP("TAA");
			taa->process(rb, rb->get_base_data_format(), p_render_data->scene_data->z_near, p_render_data->scene_data->z_far);
			RD::get_singleton()->draw_command_end_label();
		}
	}

	if (rb_data.is_valid()) {
		_debug_draw_cluster(rb);

		RENDER_TIMESTAMP("Tonemap");

		_render_buffers_post_process_and_tonemap(p_render_data);
	}

	if (rb_data.is_valid()) {
		_render_buffers_debug_draw(p_render_data);

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SDFGI && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			Vector<RID> view_rids;

			// SDFGI renders at internal resolution, need to check if our debug correctly supports outputting upscaled.
			Size2i size = rb->get_internal_size();
			RID source_texture = rb->get_internal_texture();
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				view_rids.push_back(rb->get_internal_texture(v));
			}

			sdfgi->debug_draw(p_render_data->scene_data->view_count, p_render_data->scene_data->view_projection, p_render_data->scene_data->cam_transform, size.x, size.y, rb->get_render_target(), source_texture, view_rids);
		}
	}
}

void RenderFlux::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	Ref<RenderBufferDataFlux> rb_data = rb->get_custom_data(RB_SCOPE_FLUX);
	ERR_FAIL_COND(rb_data.is_null());

	RendererSceneRenderRD::_render_buffers_debug_draw(p_render_data);

	RID render_target = rb->get_render_target();

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SSAO && rb->has_texture(RB_SCOPE_SSAO, RB_FINAL)) {
		RID final = rb->get_texture_slice(RB_SCOPE_SSAO, RB_FINAL, 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(final, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, true);
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SSIL && rb->has_texture(RB_SCOPE_SSIL, RB_FINAL)) {
		RID final = rb->get_texture_slice(RB_SCOPE_SSIL, RB_FINAL, 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(final, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false);
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_GI_BUFFER && rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID ambient_texture = rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
		RID reflection_texture = rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION);
		copy_effects->copy_to_fb_rect(ambient_texture, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false, false, true, reflection_texture, rb->get_view_count() > 1);
	}
}

void RenderFlux::_render_shadow_pass(RID p_light, RID p_shadow_atlas, int p_pass, const PagedArray<RenderGeometryInstance *> &p_instances, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, bool p_open_pass, bool p_close_pass, bool p_clear_region, RenderingServerTypes::RenderInfo *p_render_info, const Size2i &p_viewport_size, const Transform3D &p_main_cam_transform) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	ERR_FAIL_COND(!light_storage->owns_light_instance(p_light));

	RID base = light_storage->light_instance_get_base_light(p_light);

	Rect2i atlas_rect;
	uint32_t atlas_size = 1;
	RID atlas_fb;

	bool reverse_cull_face = light_storage->light_get_reverse_cull_face_mode(base);
	bool using_dual_paraboloid = false;
	bool using_dual_paraboloid_flip = false;
	Vector2i dual_paraboloid_offset;
	RID render_fb;
	RID render_texture;
	float zfar;

	bool use_pancake = false;
	bool render_cubemap = false;
	bool finalize_cubemap = false;

	bool flip_y = false;

	Projection light_projection;
	Transform3D light_transform;

	if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
		//set pssm stuff
		uint64_t last_scene_shadow_pass = light_storage->light_instance_get_shadow_pass(p_light);
		if (last_scene_shadow_pass != get_scene_pass()) {
			light_storage->light_instance_set_directional_rect(p_light, light_storage->get_directional_shadow_rect());
			light_storage->directional_shadow_increase_current_light();
			light_storage->light_instance_set_shadow_pass(p_light, get_scene_pass());
		}

		use_pancake = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE) > 0;
		light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
		light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);

		atlas_rect = light_storage->light_instance_get_directional_rect(p_light);

		if (light_storage->light_directional_get_shadow_mode(base) == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS) {
			atlas_rect.size.width /= 2;
			atlas_rect.size.height /= 2;

			if (p_pass == 1) {
				atlas_rect.position.x += atlas_rect.size.width;
			} else if (p_pass == 2) {
				atlas_rect.position.y += atlas_rect.size.height;
			} else if (p_pass == 3) {
				atlas_rect.position += atlas_rect.size;
			}
		} else if (light_storage->light_directional_get_shadow_mode(base) == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
			atlas_rect.size.height /= 2;

			if (p_pass == 0) {
			} else {
				atlas_rect.position.y += atlas_rect.size.height;
			}
		}

		float directional_shadow_size = light_storage->directional_shadow_get_size();
		Rect2 atlas_rect_norm = atlas_rect;
		atlas_rect_norm.position /= directional_shadow_size;
		atlas_rect_norm.size /= directional_shadow_size;
		light_storage->light_instance_set_directional_shadow_atlas_rect(p_light, p_pass, atlas_rect_norm);

		zfar = RSG::light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

		render_fb = light_storage->direction_shadow_get_fb();
		render_texture = RID();
		flip_y = true;

	} else {
		//set from shadow atlas

		ERR_FAIL_COND(!light_storage->owns_shadow_atlas(p_shadow_atlas));
		ERR_FAIL_COND(!light_storage->shadow_atlas_owns_light_instance(p_shadow_atlas, p_light));

		RSG::light_storage->shadow_atlas_update(p_shadow_atlas);

		uint32_t key = light_storage->shadow_atlas_get_light_instance_key(p_shadow_atlas, p_light);

		uint32_t quadrant = (key >> RendererRD::LightStorage::QUADRANT_SHIFT) & 0x3;
		uint32_t shadow = key & RendererRD::LightStorage::SHADOW_INDEX_MASK;
		uint32_t subdivision = light_storage->shadow_atlas_get_quadrant_subdivision(p_shadow_atlas, quadrant);

		ERR_FAIL_INDEX((int)shadow, light_storage->shadow_atlas_get_quadrant_shadow_size(p_shadow_atlas, quadrant));

		uint32_t shadow_atlas_size = light_storage->shadow_atlas_get_size(p_shadow_atlas);
		uint32_t quadrant_size = shadow_atlas_size >> 1;

		atlas_rect.position.x = (quadrant & 1) * quadrant_size;
		atlas_rect.position.y = (quadrant >> 1) * quadrant_size;

		uint32_t shadow_size = (quadrant_size / subdivision);
		atlas_rect.position.x += (shadow % subdivision) * shadow_size;
		atlas_rect.position.y += (shadow / subdivision) * shadow_size;

		atlas_rect.size.width = shadow_size;
		atlas_rect.size.height = shadow_size;

		zfar = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

		if (light_storage->light_get_type(base) == RSE::LIGHT_OMNI) {
			bool wrap = (shadow + 1) % subdivision == 0;
			dual_paraboloid_offset = wrap ? Vector2i(1 - subdivision, 1) : Vector2i(1, 0);

			if (light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
				render_texture = light_storage->get_cubemap(shadow_size / 2);
				render_fb = light_storage->get_cubemap_fb(shadow_size / 2, p_pass);

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);
				render_cubemap = true;
				finalize_cubemap = p_pass == 5;
				atlas_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

				atlas_size = shadow_atlas_size;

				if (p_pass == 0) {
					_render_shadow_begin();
				}

			} else {
				atlas_rect.position.x += 1;
				atlas_rect.position.y += 1;
				atlas_rect.size.x -= 2;
				atlas_rect.size.y -= 2;

				atlas_rect.position += p_pass * atlas_rect.size * dual_paraboloid_offset;

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

				using_dual_paraboloid = true;
				using_dual_paraboloid_flip = p_pass == 1;
				render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);
				flip_y = true;
			}

		} else if (light_storage->light_get_type(base) == RSE::LIGHT_SPOT) {
			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;
		} else if (light_storage->light_get_type(base) == RSE::LIGHT_AREA) {
			Vector2 area_size = light_storage->light_area_get_size(base);

			zfar = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE) + area_size.length() / 2.0;

			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;

			using_dual_paraboloid = true;
		}
	}

	if (render_cubemap) {
		//rendering to cubemap
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, false, false, use_pancake, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, Rect2(), false, true, true, true, p_render_info, p_viewport_size, p_main_cam_transform);
		if (finalize_cubemap) {
			_render_shadow_process();
			_render_shadow_end();
			//reblit
			Rect2 atlas_rect_norm = atlas_rect;
			atlas_rect_norm.position /= float(atlas_size);
			atlas_rect_norm.size /= float(atlas_size);
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), zfar, false);
			atlas_rect_norm.position += Vector2(dual_paraboloid_offset) * atlas_rect_norm.size;
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), zfar, true);

			//restore transform so it can be properly used
			light_storage->light_instance_set_shadow_transform(p_light, Projection(), light_storage->light_instance_get_base_transform(p_light), zfar, 0, 0, 0);
		}

	} else {
		//render shadow
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, using_dual_paraboloid, using_dual_paraboloid_flip, use_pancake, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, atlas_rect, flip_y, p_clear_region, p_open_pass, p_close_pass, p_render_info, p_viewport_size, p_main_cam_transform);
	}
}

void RenderFlux::_render_shadow_begin() {
	scene_state.shadow_passes.clear();
	RD::get_singleton()->draw_command_begin_label("Shadow Setup");
	_update_render_base_uniform_set();

	render_list[RENDER_LIST_SECONDARY].clear();
	// No need to reset scene_state.curr_gpu_ptr or scene_state.instance_buffer[RENDER_LIST_SECONDARY]
	// because _fill_instance_data will do that if it detects p_offset == 0u.
}

void RenderFlux::_render_shadow_append(RID p_framebuffer, const PagedArray<RenderGeometryInstance *> &p_instances, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, float p_bias, float p_normal_bias, bool p_reverse_cull_face, bool p_use_dp, bool p_use_dp_flip, bool p_use_pancake, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, const Rect2i &p_rect, bool p_flip_y, bool p_clear_region, bool p_begin, bool p_end, RenderingServerTypes::RenderInfo *p_render_info, const Size2i &p_viewport_size, const Transform3D &p_main_cam_transform) {
	SceneState::ShadowPass shadow_pass;

	RenderSceneDataRD scene_data;
	scene_data.flip_y = !p_flip_y; // Q: Why is this inverted? Do we assume flip in shadow logic?
	scene_data.cam_projection = p_projection;
	scene_data.cam_transform = p_transform;
	scene_data.view_projection[0] = p_projection;
	scene_data.z_far = p_zfar;
	scene_data.z_near = 0.0;
	scene_data.lod_distance_multiplier = p_lod_distance_multiplier;
	scene_data.dual_paraboloid_side = p_use_dp_flip ? -1 : 1;
	scene_data.opaque_prepass_threshold = 0.1f;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_main_cam_transform;
	scene_data.shadow_pass = true;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;
	render_data.render_info = p_render_info;

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_rect.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color(), false, false, p_use_pancake);

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DISABLE_LOD) {
		scene_data.screen_mesh_lod_threshold = 0.0;
	} else {
		scene_data.screen_mesh_lod_threshold = p_screen_mesh_lod_threshold;
	}

	PassMode pass_mode = p_use_dp ? PASS_MODE_SHADOW_DP : PASS_MODE_SHADOW;

	uint32_t render_list_from = render_list[RENDER_LIST_SECONDARY].elements.size();
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode, false, false, false, true);
	uint32_t render_list_size = render_list[RENDER_LIST_SECONDARY].elements.size() - render_list_from;
	render_list[RENDER_LIST_SECONDARY].sort_by_key_range(render_list_from, render_list_size);
	_fill_instance_data(RENDER_LIST_SECONDARY, p_render_info ? p_render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW] : (int *)nullptr, render_list_from, render_list_size, false);

	{
		//regular forward for now
		bool flip_cull = p_use_dp_flip;
		if (p_flip_y) {
			flip_cull = !flip_cull;
		}

		if (p_reverse_cull_face) {
			flip_cull = !flip_cull;
		}

		shadow_pass.element_from = render_list_from;
		shadow_pass.element_count = render_list_size;
		shadow_pass.flip_cull = flip_cull;
		shadow_pass.pass_mode = pass_mode;

		shadow_pass.rp_uniform_set = RID(); //will be filled later when instance buffer is complete
		shadow_pass.screen_mesh_lod_threshold = scene_data.screen_mesh_lod_threshold;
		shadow_pass.lod_distance_multiplier = scene_data.lod_distance_multiplier;

		shadow_pass.framebuffer = p_framebuffer;
		shadow_pass.clear_depth = p_begin || p_clear_region;
		shadow_pass.rect = p_rect;

		shadow_pass.uniform_buffer_index = uniform_buffer_index;

		scene_state.shadow_passes.push_back(shadow_pass);
	}
}

void RenderFlux::_render_shadow_process() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) > 0u) {
		rd->buffer_flush(scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u));
	}

	//render shadows one after the other, so this can be done un-barriered and the driver can optimize (as well as allow us to run compute at the same time)

	for (uint32_t i = 0; i < scene_state.shadow_passes.size(); i++) {
		//render passes need to be configured after instance buffer is done, since they need the latest version
		SceneState::ShadowPass &shadow_pass = scene_state.shadow_passes[i];
		shadow_pass.rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), shadow_pass.uniform_buffer_index, false);
	}

	RD::get_singleton()->draw_command_end_label();
}
void RenderFlux::_render_shadow_end() {
	RD::get_singleton()->draw_command_begin_label("Shadow Render");

	for (SceneState::ShadowPass &shadow_pass : scene_state.shadow_passes) {
		RenderListParameters render_list_parameters(render_list[RENDER_LIST_SECONDARY].elements.ptr() + shadow_pass.element_from, render_list[RENDER_LIST_SECONDARY].element_info.ptr() + shadow_pass.element_from, shadow_pass.element_count, shadow_pass.flip_cull, shadow_pass.pass_mode, 0, true, false, shadow_pass.rp_uniform_set, false, Vector2(), shadow_pass.lod_distance_multiplier, shadow_pass.screen_mesh_lod_threshold, 1, shadow_pass.element_from);
		_render_list_with_draw_list(&render_list_parameters, shadow_pass.framebuffer, shadow_pass.clear_depth ? RD::DRAW_CLEAR_DEPTH : RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0, shadow_pass.rect);
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderFlux::_render_particle_collider_heightfield(RID p_fb, const Transform3D &p_cam_transform, const Projection &p_cam_projection, const PagedArray<RenderGeometryInstance *> &p_instances) {
	RENDER_TIMESTAMP("Setup GPUParticlesCollisionHeightField3D");

	RD::get_singleton()->draw_command_begin_label("Render Collider Heightfield");

	RenderSceneDataRD scene_data;
	scene_data.flip_y = true;
	scene_data.cam_projection = p_cam_projection;
	scene_data.cam_transform = p_cam_transform;
	scene_data.view_projection[0] = p_cam_projection;
	scene_data.z_near = 0.0;
	scene_data.z_far = p_cam_projection.get_z_far();
	scene_data.dual_paraboloid_side = 0;
	scene_data.opaque_prepass_threshold = 0.0;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_cam_transform;
	scene_data.shadow_pass = true; // Not a shadow pass, but should be treated like one.

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_fb);
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, screen_size, Color(), false, false, false);

	PassMode pass_mode = PASS_MODE_SHADOW;

	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render Collider Heightfield");

	{
		//regular forward for now
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), false, pass_mode, 0, true, false, rp_uniform_set);
		_render_list_with_draw_list(&render_list_params, p_fb, RD::DRAW_CLEAR_ALL);
	}
	RD::get_singleton()->draw_command_end_label();
}

void RenderFlux::_render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region, float p_exposure_normalization) {
	RENDER_TIMESTAMP("Setup Rendering 3D Material");

	RD::get_singleton()->draw_command_begin_label("Render 3D Material");

	RenderSceneDataRD scene_data;
	scene_data.cam_projection = p_cam_projection;
	scene_data.cam_transform = p_cam_transform;
	scene_data.view_projection[0] = p_cam_projection;
	scene_data.dual_paraboloid_side = 0;
	scene_data.material_uv2_mode = false;
	scene_data.opaque_prepass_threshold = 0.0f;
	scene_data.emissive_exposure_normalization = p_exposure_normalization;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_cam_transform;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	scene_shader.enable_advanced_shader_group();

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_region.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());

	PassMode pass_mode = PASS_MODE_DEPTH_MATERIAL;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render 3D Material");

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, 0, true, false, rp_uniform_set);
		//regular forward for now
		Vector<Color> clear = {
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)
		};

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear, 0.0f, 0, p_region);
		_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count);
		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderFlux::_render_uv2(const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) {
	RENDER_TIMESTAMP("Setup Rendering UV2");

	RD::get_singleton()->draw_command_begin_label("Render UV2");

	RenderSceneDataRD scene_data;
	scene_data.dual_paraboloid_side = 0;
	scene_data.material_uv2_mode = true;
	scene_data.opaque_prepass_threshold = 0.0;
	scene_data.emissive_exposure_normalization = -1.0;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	scene_shader.enable_advanced_shader_group();

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_region.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());

	PassMode pass_mode = PASS_MODE_DEPTH_MATERIAL;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render 3D Material");

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, 0, true, false, rp_uniform_set, true);
		//regular forward for now
		Vector<Color> clear = {
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)
		};
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear, 0.0f, 0, p_region);

		const int uv_offset_count = 9;
		static const Vector2 uv_offsets[uv_offset_count] = {
			Vector2(-1, 1),
			Vector2(1, 1),
			Vector2(1, -1),
			Vector2(-1, -1),
			Vector2(-1, 0),
			Vector2(1, 0),
			Vector2(0, -1),
			Vector2(0, 1),
			Vector2(0, 0),

		};

		for (int i = 0; i < uv_offset_count; i++) {
			Vector2 ofs = uv_offsets[i];
			ofs.x /= p_region.size.width;
			ofs.y /= p_region.size.height;
			render_list_params.uv_offset = ofs;
			_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count); //first wireframe, for pseudo conservative
		}
		render_list_params.uv_offset = Vector2();
		render_list_params.force_wireframe = false;
		_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count); //second regular triangles

		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderFlux::_render_sdfgi(Ref<RenderSceneBuffersRD> p_render_buffers, const Vector3i &p_from, const Vector3i &p_size, const AABB &p_bounds, const PagedArray<RenderGeometryInstance *> &p_instances, const RID &p_albedo_texture, const RID &p_emission_texture, const RID &p_emission_aniso_texture, const RID &p_geom_facing_texture, float p_exposure_normalization) {
	RENDER_TIMESTAMP("Render SDFGI");

	RD::get_singleton()->draw_command_begin_label("Render SDFGI Voxel");

	RenderSceneDataRD scene_data;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	_update_render_base_uniform_set();

	// Indicate pipelines for SDFGI are required.
	global_pipeline_data_required.use_sdfgi = true;

	PassMode pass_mode = PASS_MODE_SDF;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	Vector3 half_size = p_bounds.size * 0.5;
	Vector3 center = p_bounds.position + half_size;

	//print_line("re-render " + p_from + " - " + p_size + " bounds " + p_bounds);
	for (int i = 0; i < 3; i++) {
		scene_state.ubo.sdf_offset[i] = p_from[i];
		scene_state.ubo.sdf_size[i] = p_size[i];
	}

	for (int i = 0; i < 3; i++) {
		Vector3 axis;
		axis[i] = 1.0;
		Vector3 up, right;
		int right_axis = (i + 1) % 3;
		int up_axis = (i + 2) % 3;
		up[up_axis] = 1.0;
		right[right_axis] = 1.0;

		Size2i fb_size;
		fb_size.x = p_size[right_axis];
		fb_size.y = p_size[up_axis];

		scene_data.cam_transform.origin = center + axis * half_size;
		scene_data.cam_transform.basis.set_column(0, right);
		scene_data.cam_transform.basis.set_column(1, up);
		scene_data.cam_transform.basis.set_column(2, axis);

		//print_line("pass: " + itos(i) + " xform " + scene_data.cam_transform);

		float h_size = half_size[right_axis];
		float v_size = half_size[up_axis];
		float d_size = half_size[i] * 2.0;
		scene_data.cam_projection.set_orthogonal(-h_size, h_size, -v_size, v_size, 0, d_size);
		//print_line("pass: " + itos(i) + " cam hsize: " + rtos(h_size) + " vsize: " + rtos(v_size) + " dsize " + rtos(d_size));

		Transform3D to_bounds;
		to_bounds.origin = p_bounds.position;
		to_bounds.basis.scale(p_bounds.size);

		RendererRD::MaterialStorage::store_transform(to_bounds.affine_inverse() * scene_data.cam_transform, scene_state.ubo.sdf_to_bounds);

		scene_data.emissive_exposure_normalization = p_exposure_normalization;
		uint32_t uniform_buffer_index = _setup_environment(&render_data, true, fb_size, fb_size, Color());

		RID rp_uniform_set = _setup_sdfgi_render_pass_uniform_set(p_albedo_texture, p_emission_texture, p_emission_aniso_texture, p_geom_facing_texture, RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

		HashMap<Size2i, RID>::Iterator E = sdfgi_framebuffer_size_cache.find(fb_size);
		if (!E) {
			RID fb = RD::get_singleton()->framebuffer_create_empty(fb_size);
			E = sdfgi_framebuffer_size_cache.insert(fb_size, fb);
		}

		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, 0, true, false, rp_uniform_set, false);
		_render_list_with_draw_list(&render_list_params, E->value);
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderFlux::base_uniforms_changed() {
	if (!render_base_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
		RD::get_singleton()->free_rid(render_base_uniform_set);
	}
	render_base_uniform_set = RID();
}

void RenderFlux::_update_render_base_uniform_set() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	if (render_base_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set) || (lightmap_texture_array_version != light_storage->lightmap_array_get_version())) {
		if (render_base_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
			RD::get_singleton()->free_rid(render_base_uniform_set);
		}

		lightmap_texture_array_version = light_storage->lightmap_array_get_version();

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 2;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(scene_shader.shadow_sampler);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 3;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_omni_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 4;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_spot_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 5;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_area_light_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 6;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_reflection_probe_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 7;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_directional_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 8;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_capture_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::TextureStorage::get_singleton()->get_decal_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 13;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.binding = 14;
			u.append_id(sdfgi_get_ubo());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 15;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CanvasItemTextureFilter::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CanvasItemTextureRepeat::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 16;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(best_fit_normal.texture);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 17;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(dfg_lut.texture);
			uniforms.push_back(u);
		}

		{ // Lookup-table for Area Lights - Linearly transformed cosines (LTC)
			if (ltc.lut1_texture.is_null() || ltc.lut2_texture.is_null()) {
				Ref<Image> lut1_image;
				int dimensions = LTC_LUT_DIMENSIONS;
				int lut1_bytes = 4 * dimensions * dimensions;
				size_t lut1_size = lut1_bytes * 4; // float

				Vector<uint8_t> lut1_data;
				lut1_data.resize(lut1_size);

				memcpy(lut1_data.ptrw(), LTC_LUT1, lut1_size);
				lut1_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut1_data);

				ltc.lut1_texture = RS::get_singleton()->texture_2d_create(lut1_image);

				int lut2_bytes = 4 * dimensions * dimensions;
				size_t lut2_size = lut2_bytes * 4;

				Ref<Image> lut2_image;
				Vector<uint8_t> lut2_data;
				lut2_data.resize(lut2_size);

				memcpy(lut2_data.ptrw(), LTC_LUT2, lut2_size);
				lut2_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut2_data);

				ltc.lut2_texture = RS::get_singleton()->texture_2d_create(lut2_image);
			}
		}

		{
			RD::Uniform u;
			u.binding = 18;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc.lut1_texture));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 19;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc.lut2_texture));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 20;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
			u.append_id(area_light_atlas);
			uniforms.push_back(u);
		}

		render_base_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, scene_shader.default_shader_rd, SCENE_UNIFORM_SET);
	}
}

RID RenderFlux::_setup_render_pass_uniform_set(RenderListType p_render_list, const RenderDataRD *p_render_data, bool p_is_multiview, RID p_radiance_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index, bool p_use_directional_shadow_atlas) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	bool is_multiview = p_is_multiview;

	Ref<RenderSceneBuffersRD> rb; // handy for not having to fully type out p_render_data->render_buffers all the time...
	Ref<RenderBufferDataFlux> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		rb = p_render_data->render_buffers;
		if (rb->has_custom_data(RB_SCOPE_FLUX)) {
			// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
			// This will not be available when rendering reflection probes.
			rb_data = rb->get_custom_data(RB_SCOPE_FLUX);
		}
	}

	//default render buffer and scene state uniform set

	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[p_render_list].get_size(0u) == 0u) {
			// Any buffer will do since it's not used, so just create one.
			// We can't use scene_shader.default_vec4_xform_buffer because it's not dynamic.
			scene_state.instance_buffer[p_render_list].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[p_render_list].prepare_for_upload();
		}
		RID instance_buffer = scene_state.instance_buffer[p_render_list]._get(0u);
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		RID radiance_texture;
		if (p_radiance_texture.is_valid()) {
			radiance_texture = p_radiance_texture;
		} else {
			radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}
	{
		RID ref_texture = (p_render_data && p_render_data->reflection_atlas.is_valid()) ? light_storage->reflection_atlas_get_texture(p_render_data->reflection_atlas) : RID();
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		if (ref_texture.is_valid()) {
			u.append_id(ref_texture);
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture;
		if (p_render_data && p_render_data->shadow_atlas.is_valid()) {
			texture = RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_render_data->shadow_atlas);
		}
		if (!texture.is_valid()) {
			texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		if (p_use_directional_shadow_atlas && RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture().is_valid()) {
			u.append_id(RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture());
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
		}
		uniforms.push_back(u);
	}
	{
		Vector<RID> textures;
		textures.resize(scene_state.max_lightmaps * 2);

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			uint32_t current_lightmap_index = i < scene_state.max_lightmaps ? i : i - scene_state.max_lightmaps;

			if (p_render_data && current_lightmap_index < p_render_data->lightmaps->size()) {
				RID base = light_storage->lightmap_instance_get_lightmap((*p_render_data->lightmaps)[current_lightmap_index]);
				RID texture;

				if (i < scene_state.max_lightmaps) {
					// Lightmap
					texture = light_storage->lightmap_get_texture(base);
				} else {
					// Shadowmask
					texture = light_storage->shadowmask_get_texture(base);
				}

				if (texture.is_valid()) {
					RID rd_texture = texture_storage->texture_get_rd_texture(texture);
					textures.write[i] = rd_texture;
					continue;
				}
			}

			textures.write[i] = default_tex;
		}
		RD::Uniform u(RD::UNIFORM_TYPE_TEXTURE, 7, textures);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			if (p_render_data && i < (int)p_render_data->voxel_gi_instances->size()) {
				RID tex = gi.voxel_gi_instance_get_texture((*p_render_data->voxel_gi_instances)[i]);
				if (!tex.is_valid()) {
					tex = default_tex;
				}
				u.append_id(tex);
			} else {
				u.append_id(default_tex);
			}
		}

		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID cb = (p_render_data && p_render_data->cluster_buffer.is_valid()) ? p_render_data->cluster_buffer : scene_shader.default_vec4_xform_buffer;
		u.append_id(cb);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	{
		RD::Uniform u;
		u.binding = 24;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture;
		if (rb.is_valid() && rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH)) {
			texture = rb->get_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH);
		} else {
			texture = texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_DEPTH : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 25;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID bbt = rb_data.is_valid() ? rb->get_back_buffer_texture() : RID();
		RID texture = bbt.is_valid() ? bbt : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 26;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb_data->has_normal_roughness() ? rb_data->get_normal_roughness() : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_NORMAL : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 27;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID aot = rb.is_valid() && rb->has_texture(RB_SCOPE_SSAO, RB_FINAL) ? rb->get_texture(RB_SCOPE_SSAO, RB_FINAL) : RID();
		RID texture = aot.is_valid() ? aot : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 29;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 30;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID t;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			t = sdfgi->lightprobe_texture;
		}
		if (t.is_null()) {
			t = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		}
		u.append_id(t);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 31;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID t;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			t = sdfgi->occlusion_texture;
		}
		if (t.is_null()) {
			t = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		}
		u.append_id(t);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		RID voxel_gi;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_GI)) {
			Ref<RendererRD::GI::RenderBuffersGI> rbgi = rb->get_custom_data(RB_SCOPE_GI);
			voxel_gi = rbgi->get_voxel_gi_buffer();
		}
		u.append_id(voxel_gi.is_valid() ? voxel_gi : render_buffers_get_default_voxel_gi_buffer());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 33;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID vfog;
		if (rb_data.is_valid() && rb->has_custom_data(RB_SCOPE_FOG)) {
			Ref<RendererRD::Fog::VolumetricFog> fog = rb->get_custom_data(RB_SCOPE_FOG);
			vfog = fog->fog_map;
			if (vfog.is_null()) {
				vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
			}
		} else {
			vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		}
		u.append_id(vfog);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 34;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID ssil = rb.is_valid() && rb->has_texture(RB_SCOPE_SSIL, RB_FINAL) ? rb->get_texture(RB_SCOPE_SSIL, RB_FINAL) : RID();
		RID texture = ssil.is_valid() ? ssil : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 35;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID ssr;
		if (rb_data.is_valid()) {
			if (rb_data->ss_effects_data.ssr.half_size) {
				if (rb->has_texture(RB_SCOPE_SSR, RB_FINAL)) {
					ssr = rb->get_texture(RB_SCOPE_SSR, RB_FINAL);
				}
			} else {
				if (rb->has_texture(RB_SCOPE_SSR, RB_SSR)) {
					ssr = rb->get_texture(RB_SCOPE_SSR, RB_SSR);
				}
			}
		}

		RID texture = ssr.is_valid() ? ssr : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 36;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID ssr_mip_level = (rb_data.is_valid() && !rb_data->ss_effects_data.ssr.half_size && rb->has_texture(RB_SCOPE_SSR, RB_MIP_LEVEL)) ? rb->get_texture(RB_SCOPE_SSR, RB_MIP_LEVEL) : RID();
		RID texture = ssr_mip_level.is_valid() ? ssr_mip_level : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 37;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_EFFECT) ? rb->get_texture(RB_SCOPE_FLUX, RB_TEX_HYBRID_EFFECT) : RID();
		if (!texture.is_valid()) {
			texture = texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.get_default_shader_rd(is_multiview), RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderFlux::_setup_sdfgi_render_pass_uniform_set(RID p_albedo_texture, RID p_emission_texture, RID p_emission_aniso_texture, RID p_geom_facing_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) == 0u) {
			// Any buffer will do since it's not used, so just create one.
			// We can't use scene_shader.default_vec4_xform_buffer because it's not dynamic.
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].prepare_for_upload();
		}
		RID instance_buffer = scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u);
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		// No radiance texture.
		RID radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}

	{
		// No reflection atlas.
		RID ref_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(ref_texture);
		uniforms.push_back(u);
	}

	{
		// No shadow atlas.
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		// No directional shadow atlas.
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		// No Lightmaps
		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			u.append_id(default_tex);
		}

		uniforms.push_back(u);
	}

	{
		// No VoxelGIs
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			u.append_id(default_tex);
		}

		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID cb = scene_shader.default_vec4_xform_buffer;
		u.append_id(cb);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	// actual sdfgi stuff

	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 24;
		u.append_id(p_albedo_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 25;
		u.append_id(p_emission_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 26;
		u.append_id(p_emission_aniso_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 27;
		u.append_id(p_geom_facing_texture);
		uniforms.push_back(u);
	}

	if (scene_shader.default_shader_sdfgi_rd.is_null()) {
		// The variant for SDF from the default material should only be retrieved when SDFGI is required.
		ERR_FAIL_NULL_V(scene_shader.default_material_shader_ptr, RID());
		scene_shader.enable_advanced_shader_group();
		scene_shader.default_shader_sdfgi_rd = scene_shader.default_material_shader_ptr->get_shader_variant(SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF, 0, true);
		ERR_FAIL_COND_V(scene_shader.default_shader_sdfgi_rd.is_null(), RID());
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.default_shader_sdfgi_rd, RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderFlux::_render_buffers_get_normal_texture(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataFlux> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FLUX);

	return rb_data->get_normal_roughness();
}

RID RenderFlux::_render_buffers_get_velocity_texture(Ref<RenderSceneBuffersRD> p_render_buffers) {
	return p_render_buffers->get_velocity_buffer(false);
}

void RenderFlux::environment_set_ssao_quality(RSE::EnvironmentSSAOQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::EnvironmentSSAOQuality::ENV_SSAO_QUALITY_VERY_LOW || p_quality > RSE::EnvironmentSSAOQuality::ENV_SSAO_QUALITY_ULTRA);
	ss_effects->ssao_set_quality(p_quality, p_half_size, p_adaptive_target, p_blur_passes, p_fadeout_from, p_fadeout_to);
}

void RenderFlux::environment_set_ssil_quality(RSE::EnvironmentSSILQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::EnvironmentSSILQuality::ENV_SSIL_QUALITY_VERY_LOW || p_quality > RSE::EnvironmentSSILQuality::ENV_SSIL_QUALITY_ULTRA);
	ss_effects->ssil_set_quality(p_quality, p_half_size, p_adaptive_target, p_blur_passes, p_fadeout_from, p_fadeout_to);
}

void RenderFlux::environment_set_ssr_half_size(bool p_half_size) {
	ERR_FAIL_NULL(ss_effects);
	ss_effects->ssr_set_half_size(p_half_size);
}

void RenderFlux::environment_set_ssr_roughness_quality(RSE::EnvironmentSSRRoughnessQuality p_quality) {
	WARN_PRINT_ONCE("environment_set_ssr_roughness_quality has been deprecated and no longer does anything.");
}

void RenderFlux::sub_surface_scattering_set_quality(RSE::SubSurfaceScatteringQuality p_quality) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::SubSurfaceScatteringQuality::SUB_SURFACE_SCATTERING_QUALITY_DISABLED || p_quality > RSE::SubSurfaceScatteringQuality::SUB_SURFACE_SCATTERING_QUALITY_HIGH);
	ss_effects->sss_set_quality(p_quality);
}

void RenderFlux::sub_surface_scattering_set_scale(float p_scale, float p_depth_scale) {
	ERR_FAIL_NULL(ss_effects);
	ss_effects->sss_set_scale(p_scale, p_depth_scale);
}

RenderFlux *RenderFlux::singleton = nullptr;

void RenderFlux::sdfgi_update(const Ref<RenderSceneBuffers> &p_render_buffers, RID p_environment, const Vector3 &p_world_position) {
	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RendererRD::GI::SDFGI> sdfgi;
	if (rb->has_custom_data(RB_SCOPE_SDFGI)) {
		sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	}

	bool needs_sdfgi = p_environment.is_valid() && environment_get_sdfgi_enabled(p_environment);
	bool needs_reset = sdfgi.is_valid() ? sdfgi->version != gi.sdfgi_current_version : false;

	if (!needs_sdfgi || needs_reset) {
		if (sdfgi.is_valid()) {
			// delete it
			sdfgi.unref();
			rb->set_custom_data(RB_SCOPE_SDFGI, sdfgi);
		}

		if (!needs_sdfgi) {
			return;
		}
	}

	// Ensure advanced shaders are available if SDFGI is used.
	// Call here as this is the first entry point for SDFGI.
	scene_shader.enable_advanced_shader_group();

	static const uint32_t history_frames_to_converge[RSE::ENV_SDFGI_CONVERGE_MAX] = { 5, 10, 15, 20, 25, 30 };
	uint32_t requested_history_size = history_frames_to_converge[gi.sdfgi_frames_to_converge];

	if (sdfgi.is_valid() && (sdfgi->num_cascades != environment_get_sdfgi_cascades(p_environment) || sdfgi->min_cell_size != environment_get_sdfgi_min_cell_size(p_environment) || requested_history_size != sdfgi->history_size || sdfgi->uses_occlusion != environment_get_sdfgi_use_occlusion(p_environment) || sdfgi->y_scale_mode != environment_get_sdfgi_y_scale(p_environment))) {
		//configuration changed, erase
		sdfgi.unref();
		rb->set_custom_data(RB_SCOPE_SDFGI, sdfgi);
	}

	if (sdfgi.is_null()) {
		// re-create
		sdfgi = gi.create_sdfgi(p_environment, p_world_position, requested_history_size);
		rb->set_custom_data(RB_SCOPE_SDFGI, sdfgi);
	} else {
		//check for updates
		sdfgi->update(p_environment, p_world_position);
	}
}

int RenderFlux::sdfgi_get_pending_region_count(const Ref<RenderSceneBuffers> &p_render_buffers) const {
	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), 0);

	if (!rb->has_custom_data(RB_SCOPE_SDFGI)) {
		return 0;
	}
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);

	int dirty_count = 0;
	for (const RendererRD::GI::SDFGI::Cascade &c : sdfgi->cascades) {
		if (c.dirty_regions == RendererRD::GI::SDFGI::Cascade::DIRTY_ALL) {
			dirty_count++;
		} else {
			for (int j = 0; j < 3; j++) {
				if (c.dirty_regions[j] != 0) {
					dirty_count++;
				}
			}
		}
	}

	return dirty_count;
}

AABB RenderFlux::sdfgi_get_pending_region_bounds(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const {
	AABB bounds;
	Vector3i from;
	Vector3i size;

	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), AABB());
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	ERR_FAIL_COND_V(sdfgi.is_null(), AABB());

	int c = sdfgi->get_pending_region_data(p_region, from, size, bounds);
	ERR_FAIL_COND_V(c == -1, AABB());
	return bounds;
}

uint32_t RenderFlux::sdfgi_get_pending_region_cascade(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const {
	AABB bounds;
	Vector3i from;
	Vector3i size;

	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), -1);
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	ERR_FAIL_COND_V(sdfgi.is_null(), -1);

	return sdfgi->get_pending_region_data(p_region, from, size, bounds);
}

void RenderFlux::GeometryInstanceFlux::_mark_dirty() {
	if (dirty_list_element.in_list()) {
		return;
	}

	//clear surface caches
	GeometryInstanceSurfaceDataCache *surf = surface_caches;

	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		RenderFlux::get_singleton()->geometry_instance_surface_alloc.free(surf);
		surf = next;
	}

	surface_caches = nullptr;

	RenderFlux::get_singleton()->geometry_instance_dirty_list.add(&dirty_list_element);
}

void RenderFlux::_update_global_pipeline_data_requirements_from_project() {
	const int msaa_3d_mode = GLOBAL_GET_CACHED(int, "rendering/anti_aliasing/quality/msaa_3d");
	const bool directional_shadow_16_bits = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/directional_shadow/16_bits");
	const bool positional_shadow_16_bits = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/positional_shadow/atlas_16_bits");
	global_pipeline_data_required.use_16_bit_shadows = directional_shadow_16_bits || positional_shadow_16_bits;
	global_pipeline_data_required.use_32_bit_shadows = !directional_shadow_16_bits || !positional_shadow_16_bits;
	global_pipeline_data_required.texture_samples = RenderSceneBuffersRD::msaa_to_samples(RSE::ViewportMSAA(msaa_3d_mode));
}

void RenderFlux::_update_global_pipeline_data_requirements_from_light_storage() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	global_pipeline_data_required.use_shadow_cubemaps = light_storage->get_shadow_cubemaps_used();
	global_pipeline_data_required.use_shadow_dual_paraboloid = light_storage->get_shadow_dual_paraboloid_used();
}

void RenderFlux::_geometry_instance_add_surface_with_material(GeometryInstanceFlux *ginstance, uint32_t p_surface, SceneShaderFlux::MaterialData *p_material, RID p_material_rid, uint32_t p_shader_id, RID p_mesh) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint32_t flags = 0;

	if (p_material->shader_data->uses_sss) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SUBSURFACE_SCATTERING;
		global_surface_data.sss_used = true;
	}

	if (p_material->shader_data->uses_screen_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SCREEN_TEXTURE;
		global_surface_data.screen_texture_used = true;
	}

	if (p_material->shader_data->uses_depth_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_DEPTH_TEXTURE;
		global_surface_data.depth_texture_used = true;
	}

	if (p_material->shader_data->uses_normal_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_NORMAL_TEXTURE;
		global_surface_data.normal_texture_used = true;
	}

	if (ginstance->data->cast_double_sided_shadows) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS;
	}

	if (p_material->shader_data->stencil_enabled) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_STENCIL;
	}

	if (p_material->shader_data->uses_alpha_pass()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA;
		if (p_material->shader_data->uses_depth_in_alpha_pass()) {
			flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
			flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
		}
	} else {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE;
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
	}

	if (p_material->shader_data->uses_particle_trails) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS;
	}

	if (p_material->shader_data->is_animated()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR;
	}

	if (p_material->shader_data->stencil_enabled) {
		if (p_material->shader_data->stencil_flags & SceneShaderFlux::ShaderData::STENCIL_FLAG_READ) {
			// Stencil materials which read from the stencil buffer must be in the alpha pass.
			// This is critical to preserve compatibility once we'll have the compositor.
			if (!(flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
				String shader_path = p_material->shader_data->path.is_empty() ? "" : "(" + p_material->shader_data->path + ")";
				ERR_PRINT_ED(vformat("Attempting to use a shader %s that reads stencil but is not in the alpha queue. Ensure the material uses alpha blending or has depth_draw disabled or depth_test disabled.", shader_path));
			}
		}
	}

	SceneShaderFlux::MaterialData *material_shadow = nullptr;
	void *surface_shadow = nullptr;
	if (p_material->shader_data->uses_shared_shadow_material()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SHARED_SHADOW_MATERIAL;
		material_shadow = static_cast<SceneShaderFlux::MaterialData *>(RendererRD::MaterialStorage::get_singleton()->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));

		RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);
		if (shadow_mesh.is_valid()) {
			surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, p_surface);
		}
	} else {
		material_shadow = p_material;
	}

	GeometryInstanceSurfaceDataCache *sdcache = geometry_instance_surface_alloc.alloc();

	sdcache->flags = flags;

	sdcache->shader = p_material->shader_data;
	sdcache->material = p_material;
	sdcache->material_rid = p_material_rid;
	sdcache->material_uniform_set = p_material->uniform_set;
	sdcache->surface = mesh_storage->mesh_get_surface(p_mesh, p_surface);
	sdcache->primitive = mesh_storage->mesh_surface_get_primitive(sdcache->surface);
	sdcache->surface_index = p_surface;

	if (ginstance->data->dirty_dependencies) {
		RSG::utilities->base_update_dependency(p_mesh, &ginstance->data->dependency_tracker);
	}

	//shadow
	sdcache->shader_shadow = material_shadow->shader_data;
	sdcache->material_uniform_set_shadow = material_shadow->uniform_set;

	sdcache->surface_shadow = surface_shadow ? surface_shadow : sdcache->surface;

	sdcache->owner = ginstance;

	sdcache->next = ginstance->surface_caches;
	ginstance->surface_caches = sdcache;

	//sortkey

	sdcache->sort.sort_key1 = 0;
	sdcache->sort.sort_key2 = 0;

	sdcache->sort.surface_index = p_surface;
	const uint32_t material_id = p_material_rid.get_local_index();
	sdcache->sort.material_id_hi = (material_id & 0xFF000000) >> 24;
	sdcache->sort.material_id_lo = (material_id & 0x00FFFFFF);
	sdcache->sort.shader_id = p_shader_id;
	sdcache->sort.geometry_id = p_mesh.get_local_index(); //only meshes can repeat anyway
	sdcache->sort.uses_forward_gi = ginstance->can_sdfgi;
	sdcache->sort.priority = p_material->priority;
	sdcache->sort.uses_projector = ginstance->using_projectors;
	sdcache->sort.uses_softshadow = ginstance->using_softshadows;

	uint64_t format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(sdcache->surface);
	if (p_material->shader_data->uses_tangent && !p_material->shader_data->writes_tangent && !(format & RSE::ARRAY_FORMAT_TANGENT)) {
		String shader_path = p_material->shader_data->path.is_empty() ? "" : "(" + p_material->shader_data->path + ")";
		String mesh_path = mesh_storage->mesh_get_path(p_mesh).is_empty() ? "" : "(" + mesh_storage->mesh_get_path(p_mesh) + ")";
		WARN_PRINT_ED(vformat("Attempting to use a shader %s that requires tangents with a mesh %s that doesn't contain tangents. Ensure that meshes are imported with the 'ensure_tangents' option. If creating your own meshes, add an `ARRAY_TANGENT` array (when using ArrayMesh) or call `generate_tangents()` (when using SurfaceTool).", shader_path, mesh_path));
	}

#if PRELOAD_PIPELINES_ON_SURFACE_CACHE_CONSTRUCTION
	if (!sdcache->compilation_dirty_element.in_list()) {
		geometry_surface_compilation_dirty_list.add(&sdcache->compilation_dirty_element);
	}

	if (!sdcache->compilation_all_element.in_list()) {
		geometry_surface_compilation_all_list.add(&sdcache->compilation_all_element);
	}
#endif
}

void RenderFlux::_geometry_instance_add_surface_with_material_chain(GeometryInstanceFlux *ginstance, uint32_t p_surface, SceneShaderFlux::MaterialData *p_material, RID p_mat_src, RID p_mesh) {
	SceneShaderFlux::MaterialData *material = p_material;
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	_geometry_instance_add_surface_with_material(ginstance, p_surface, material, p_mat_src, material_storage->material_get_shader_id(p_mat_src), p_mesh);

	while (material->next_pass.is_valid()) {
		RID next_pass = material->next_pass;
		material = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(next_pass, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (!material || !material->shader_data->is_valid()) {
			break;
		}
		if (ginstance->data->dirty_dependencies) {
			material_storage->material_update_dependency(next_pass, &ginstance->data->dependency_tracker);
		}
		_geometry_instance_add_surface_with_material(ginstance, p_surface, material, next_pass, material_storage->material_get_shader_id(next_pass), p_mesh);
	}
}

void RenderFlux::_geometry_instance_add_surface(GeometryInstanceFlux *ginstance, uint32_t p_surface, RID p_material, RID p_mesh) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RID m_src;

	m_src = ginstance->data->material_override.is_valid() ? ginstance->data->material_override : p_material;

	SceneShaderFlux::MaterialData *material = nullptr;

	if (m_src.is_valid()) {
		material = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(m_src, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (!material || !material->shader_data->is_valid()) {
			material = nullptr;
		}
	}

	if (material) {
		if (ginstance->data->dirty_dependencies) {
			material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
		}
	} else {
		material = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		m_src = scene_shader.default_material;
	}

	ERR_FAIL_NULL(material);

	_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material, m_src, p_mesh);

	if (ginstance->data->material_overlay.is_valid()) {
		m_src = ginstance->data->material_overlay;

		material = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(m_src, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (material && material->shader_data->is_valid()) {
			if (ginstance->data->dirty_dependencies) {
				material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
			}

			_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material, m_src, p_mesh);
		}
	}
}

void RenderFlux::_geometry_instance_update(RenderGeometryInstance *p_geometry_instance) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	GeometryInstanceFlux *ginstance = static_cast<GeometryInstanceFlux *>(p_geometry_instance);

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_begin();
	}

	//add geometry for drawing
	switch (ginstance->data->base_type) {
		case RSE::INSTANCE_MESH: {
			const RID *materials = nullptr;
			uint32_t surface_count;
			RID mesh = ginstance->data->base;

			materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
			if (materials) {
				//if no materials, no surfaces.
				const RID *inst_materials = ginstance->data->surface_materials.ptr();
				uint32_t surf_mat_count = ginstance->data->surface_materials.size();

				for (uint32_t j = 0; j < surface_count; j++) {
					RID material = (j < surf_mat_count && inst_materials[j].is_valid()) ? inst_materials[j] : materials[j];
					_geometry_instance_add_surface(ginstance, j, material, mesh);
				}
			}

			ginstance->instance_count = 1;

		} break;

		case RSE::INSTANCE_MULTIMESH: {
			RID mesh = mesh_storage->multimesh_get_mesh(ginstance->data->base);
			if (mesh.is_valid()) {
				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t j = 0; j < surface_count; j++) {
						_geometry_instance_add_surface(ginstance, j, materials[j], mesh);
					}
				}

				ginstance->instance_count = mesh_storage->multimesh_get_instances_to_draw(ginstance->data->base);
			}

		} break;
#if 0
		case RSE::INSTANCE_IMMEDIATE: {
			RasterizerStorageGLES3::Immediate *immediate = storage->immediate_owner.get_or_null(inst->base);
			ERR_CONTINUE(!immediate);

			_add_geometry(immediate, inst, nullptr, -1, p_depth_pass, p_shadow_pass);

		} break;
#endif
		case RSE::INSTANCE_PARTICLES: {
			int draw_passes = particles_storage->particles_get_draw_passes(ginstance->data->base);

			for (int j = 0; j < draw_passes; j++) {
				RID mesh = particles_storage->particles_get_draw_pass_mesh(ginstance->data->base, j);
				if (!mesh.is_valid()) {
					continue;
				}

				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t k = 0; k < surface_count; k++) {
						_geometry_instance_add_surface(ginstance, k, materials[k], mesh);
					}
				}
			}

			ginstance->instance_count = particles_storage->particles_get_amount(ginstance->data->base, ginstance->trail_steps);

		} break;

		default: {
		}
	}

	//Fill push constant

	ginstance->base_flags = 0;

	bool store_transform = true;
	if (ginstance->data->base_type == RSE::INSTANCE_MULTIMESH) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		if (mesh_storage->multimesh_get_transform_format(ginstance->data->base) == RSE::MULTIMESH_TRANSFORM_2D) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D;
		}
		if (mesh_storage->multimesh_uses_colors(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		}
		if (mesh_storage->multimesh_uses_custom_data(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;
		}
		if (mesh_storage->multimesh_uses_indirect(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT;
		}

		ginstance->transforms_uniform_set = mesh_storage->multimesh_get_3d_uniform_set(ginstance->data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

	} else if (ginstance->data->base_type == RSE::INSTANCE_PARTICLES) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_PARTICLES;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;

		//for particles, stride is the trail size
		ginstance->base_flags |= (ginstance->trail_steps << INSTANCE_DATA_FLAGS_PARTICLE_TRAIL_SHIFT);

		if (!particles_storage->particles_is_using_local_coords(ginstance->data->base)) {
			store_transform = false;
		}
		if (particles_storage->particles_get_frame_counter(ginstance->data->base) == 0) {
			// Particles haven't been cleared or updated, update once now to ensure they are ready to render.
			particles_storage->update_particles();
		}

		ginstance->transforms_uniform_set = particles_storage->particles_get_instance_buffer_uniform_set(ginstance->data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

		if (ginstance->data->dirty_dependencies) {
			particles_storage->particles_update_dependency(ginstance->data->base, &ginstance->data->dependency_tracker);
		}
	} else if (ginstance->data->base_type == RSE::INSTANCE_MESH) {
		if (mesh_storage->skeleton_is_valid(ginstance->data->skeleton)) {
			ginstance->transforms_uniform_set = mesh_storage->skeleton_get_3d_uniform_set(ginstance->data->skeleton, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);
			if (ginstance->data->dirty_dependencies) {
				mesh_storage->skeleton_update_dependency(ginstance->data->skeleton, &ginstance->data->dependency_tracker);
			}
		} else {
			ginstance->transforms_uniform_set = RID();
		}
	}

	ginstance->store_transform_cache = store_transform;
	ginstance->can_sdfgi = false;

	if (!RendererRD::LightStorage::get_singleton()->lightmap_instance_is_valid(ginstance->lightmap_instance)) {
		if (ginstance->voxel_gi_instances[0].is_null() && (ginstance->data->use_baked_light || ginstance->data->use_dynamic_gi)) {
			ginstance->can_sdfgi = true;
		}
	}

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_end();
		ginstance->data->dirty_dependencies = false;
	}

	ginstance->dirty_list_element.remove_from_list();
}

static RD::FramebufferFormatID _get_color_framebuffer_format_for_pipeline(RD::DataFormat p_color_format, bool p_can_be_storage, RD::TextureSamples p_samples, bool p_specular, bool p_velocity, uint32_t p_view_count) {
	const bool multisampling = p_samples > RD::TEXTURE_SAMPLES_1;
	RD::AttachmentFormat attachment;
	attachment.samples = p_samples;

	RD::AttachmentFormat unused_attachment;
	unused_attachment.usage_flags = RD::AttachmentFormat::UNUSED_ATTACHMENT;

	thread_local Vector<RD::AttachmentFormat> attachments;
	attachments.clear();

	// Color attachment.
	attachment.format = p_color_format;
	attachment.usage_flags = RenderSceneBuffersRD::get_color_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	if (p_specular) {
		attachment.format = RenderFlux::RenderBufferDataFlux::get_specular_format();
		attachment.usage_flags = RenderFlux::RenderBufferDataFlux::get_specular_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	} else {
		attachments.push_back(unused_attachment);
	}

	if (p_velocity) {
		attachment.format = RenderSceneBuffersRD::get_velocity_format();
		attachment.usage_flags = RenderSceneBuffersRD::get_velocity_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	} else {
		attachments.push_back(unused_attachment);
	}

	// Depth attachment.
	attachment.format = RenderSceneBuffersRD::get_depth_format(false, multisampling, p_can_be_storage);
	attachment.usage_flags = RenderSceneBuffersRD::get_depth_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	thread_local Vector<RD::FramebufferPass> passes;
	passes.resize(1);
	passes.ptrw()[0].color_attachments.resize(attachments.size() - 1);

	int *color_attachments = passes.ptrw()[0].color_attachments.ptrw();
	for (int64_t i = 0; i < attachments.size() - 1; i++) {
		color_attachments[i] = (attachments[i].usage_flags == RD::AttachmentFormat::UNUSED_ATTACHMENT) ? RD::ATTACHMENT_UNUSED : i;
	}

	passes.ptrw()[0].depth_attachment = attachments.size() - 1;

	return RD::get_singleton()->framebuffer_format_create_multipass(attachments, passes, p_view_count);
}

static RD::FramebufferFormatID _get_reflection_probe_color_framebuffer_format_for_pipeline(bool p_storage) {
	RD::AttachmentFormat attachment;
	thread_local Vector<RD::AttachmentFormat> attachments;
	attachments.clear();

	attachment.format = RendererRD::LightStorage::get_reflection_probe_color_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_color_usage_bits(p_storage);
	attachments.push_back(attachment);

	attachment.format = RendererRD::LightStorage::get_reflection_probe_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(attachments);
}

static RD::FramebufferFormatID _get_depth_framebuffer_format_for_pipeline(bool p_can_be_storage, RD::TextureSamples p_samples, bool p_normal_roughness, bool p_voxelgi) {
	const bool multisampling = p_samples > RD::TEXTURE_SAMPLES_1;
	RD::AttachmentFormat attachment;
	attachment.samples = p_samples;

	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	attachment.format = RenderSceneBuffersRD::get_depth_format(false, multisampling, p_can_be_storage);
	attachment.usage_flags = RenderSceneBuffersRD::get_depth_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	if (p_normal_roughness) {
		attachment.format = RenderFlux::RenderBufferDataFlux::get_normal_roughness_format();
		attachment.usage_flags = RenderFlux::RenderBufferDataFlux::get_normal_roughness_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	}

	if (p_voxelgi) {
		attachment.format = RenderFlux::RenderBufferDataFlux::get_voxelgi_format();
		attachment.usage_flags = RenderFlux::RenderBufferDataFlux::get_voxelgi_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	}

	thread_local Vector<RD::FramebufferPass> passes;
	passes.resize(1);
	passes.ptrw()[0].color_attachments.resize(attachments.size() - 1);

	int *color_attachments = passes.ptrw()[0].color_attachments.ptrw();
	for (int64_t i = 1; i < attachments.size(); i++) {
		color_attachments[i - 1] = (attachments[i].usage_flags == RD::AttachmentFormat::UNUSED_ATTACHMENT) ? RD::ATTACHMENT_UNUSED : i;
	}

	passes.ptrw()[0].depth_attachment = 0;

	return RD::get_singleton()->framebuffer_format_create_multipass(Vector<RD::AttachmentFormat>(attachments), passes);
}

static RD::FramebufferFormatID _get_shadow_cubemap_framebuffer_format_for_pipeline() {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_cubemap_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_cubemap_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

static RD::FramebufferFormatID _get_shadow_atlas_framebuffer_format_for_pipeline(bool p_use_16_bits) {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_shadow_atlas_depth_format(p_use_16_bits);
	attachment.usage_flags = RendererRD::LightStorage::get_shadow_atlas_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

static RD::FramebufferFormatID _get_reflection_probe_depth_framebuffer_format_for_pipeline() {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_reflection_probe_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

void RenderFlux::_mesh_compile_pipeline_for_surface(SceneShaderFlux::ShaderData *p_shader, void *p_mesh_surface, bool p_ubershader, bool p_instanced_surface, RSE::PipelineSource p_source, SceneShaderFlux::ShaderData::PipelineKey &r_pipeline_key, Vector<ShaderPipelinePair> *r_pipeline_pairs) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint64_t input_mask = p_shader->get_vertex_input_mask(r_pipeline_key.version, r_pipeline_key.color_pass_flags, p_ubershader);
	bool pipeline_motion_vectors = r_pipeline_key.color_pass_flags & SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
	bool emulate_point_size = p_shader->uses_point_size && scene_shader.emulate_point_size;
	r_pipeline_key.vertex_format_id = mesh_storage->mesh_surface_get_vertex_format(p_mesh_surface, input_mask, p_instanced_surface, pipeline_motion_vectors, emulate_point_size);
	r_pipeline_key.ubershader = p_ubershader;

	p_shader->pipeline_hash_map.compile_pipeline(r_pipeline_key, r_pipeline_key.hash(), p_source, p_ubershader);

	if (r_pipeline_pairs != nullptr) {
		r_pipeline_pairs->push_back({ p_shader, r_pipeline_key });
	}
}

void RenderFlux::_mesh_compile_pipelines_for_surface(const SurfacePipelineData &p_surface, const GlobalPipelineData &p_global, RSE::PipelineSource p_source, Vector<ShaderPipelinePair> *r_pipeline_pairs) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	bool octmap_use_storage = !copy_effects->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);

	// Retrieve from the scene shader which groups are currently enabled.
	const bool multiview_enabled = p_global.use_multiview && scene_shader.is_multiview_shader_group_enabled();
	const RD::DataFormat buffers_color_format = _render_buffers_get_preferred_color_format();
	const bool buffers_can_be_storage = _render_buffers_can_be_storage();

	// Set the attributes common to all pipelines.
	SceneShaderFlux::ShaderData::PipelineKey pipeline_key;
	pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
	pipeline_key.primitive_type = mesh_storage->mesh_surface_get_primitive(p_surface.mesh_surface);
	pipeline_key.wireframe = false;

	// Grab the shader and surface used for most passes.
	const uint32_t multiview_iterations = multiview_enabled ? 2 : 1;
	const uint32_t lightmap_iterations = p_global.use_lightmaps && p_surface.can_use_lightmap ? 2 : 1;
	const uint32_t alpha_iterations = p_surface.uses_transparent ? 2 : 1;
	for (uint32_t multiview = 0; multiview < multiview_iterations; multiview++) {
		for (uint32_t lightmap = 0; lightmap < lightmap_iterations; lightmap++) {
			for (uint32_t alpha = p_surface.uses_opaque ? 0 : 1; alpha < alpha_iterations; alpha++) {
				// Generate all the possible variants used during the color pass.
				pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_COLOR_PASS;
				pipeline_key.color_pass_flags = 0;

				if (lightmap) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_LIGHTMAP;
				}

				if (alpha) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_TRANSPARENT;
				}

				if (multiview) {
					pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MULTIVIEW;
				} else if (p_global.use_reflection_probes) {
					// Reflection probe can't be rendered in multiview.
					pipeline_key.framebuffer_format_id = _get_reflection_probe_color_framebuffer_format_for_pipeline(octmap_use_storage);
					_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
				}

				// View count is assumed to be 2 as the configuration is dependent on the viewport. It's likely a safe assumption for stereo rendering.
				uint32_t view_count = multiview ? 2 : 1;
				pipeline_key.framebuffer_format_id = _get_color_framebuffer_format_for_pipeline(buffers_color_format, buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false, view_count);
				_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

				// Generate all the possible variants used during the advanced color passes.
				const uint32_t separate_specular_iterations = p_global.use_separate_specular ? 2 : 1;
				const uint32_t motion_vectors_iterations = p_global.use_motion_vectors ? 2 : 1;
				uint32_t base_color_pass_flags = pipeline_key.color_pass_flags;
				for (uint32_t separate_specular = 0; separate_specular < separate_specular_iterations; separate_specular++) {
					for (uint32_t motion_vectors = 0; motion_vectors < motion_vectors_iterations; motion_vectors++) {
						if (!separate_specular && !motion_vectors) {
							// This case was already generated.
							continue;
						}

						pipeline_key.color_pass_flags = base_color_pass_flags;

						if (separate_specular) {
							pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR;
						}

						if (motion_vectors) {
							pipeline_key.color_pass_flags |= SceneShaderFlux::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
						}

						pipeline_key.framebuffer_format_id = _get_color_framebuffer_format_for_pipeline(buffers_color_format, buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), separate_specular, motion_vectors, view_count);
						_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
					}
				}
			}
		}
	}

	if (!p_surface.uses_depth) {
		return;
	}

	// Generate the depth pipelines if the material supports depth or it must be part of the shadow pass.
	pipeline_key.color_pass_flags = 0;

	if (p_global.use_normal_and_roughness) {
		// A lot of different effects rely on normal and roughness being written to during the depth pass.
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), true, false);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_voxelgi) {
		// Depth pass with VoxelGI support.
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), true, true);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_sdfgi) {
		// Depth pass with SDFGI support.
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

		// Depth pass with SDFGI support for an empty framebuffer.
		pipeline_key.framebuffer_format_id = RD::get_singleton()->framebuffer_format_create_empty();
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// The dedicated depth passes use a different version of the surface and the shader.
	pipeline_key.primitive_type = mesh_storage->mesh_surface_get_primitive(p_surface.mesh_surface_shadow);
	pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS;
	pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false);
	_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

	if (p_global.use_shadow_dual_paraboloid) {
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_DP;
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_shadow_cubemaps) {
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_shadow_cubemap_framebuffer_format_for_pipeline();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// Atlas shadowmaps (omni lights) can be in both 16-bit and 32-bit versions.
	const uint32_t use_16_bits_start = p_global.use_32_bit_shadows ? 0 : 1;
	const uint32_t use_16_bits_iterations = p_global.use_16_bit_shadows ? 2 : 1;
	for (uint32_t use_16_bits = use_16_bits_start; use_16_bits < use_16_bits_iterations; use_16_bits++) {
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_shadow_atlas_framebuffer_format_for_pipeline(use_16_bits);
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

		if (p_global.use_shadow_dual_paraboloid) {
			pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS_DP;
			_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
		}
	}

	if (p_global.use_reflection_probes) {
		// Depth pass for reflection probes. Normally this will be redundant as the format is the exact same as the shadow cubemap.
		pipeline_key.version = SceneShaderFlux::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_reflection_probe_depth_framebuffer_format_for_pipeline();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}
}

void RenderFlux::_mesh_generate_all_pipelines_for_surface_cache(GeometryInstanceSurfaceDataCache *p_surface_cache, const GlobalPipelineData &p_global) {
	bool uses_alpha_pass = (p_surface_cache->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) != 0;
	float multiplied_fade_alpha = p_surface_cache->owner->force_alpha * p_surface_cache->owner->parent_fade_alpha;
	bool uses_fade = (multiplied_fade_alpha < FADE_ALPHA_PASS_THRESHOLD) || p_surface_cache->owner->fade_near || p_surface_cache->owner->fade_far;
	SurfacePipelineData surface;
	surface.mesh_surface = p_surface_cache->surface;
	surface.mesh_surface_shadow = p_surface_cache->surface_shadow;
	surface.shader = p_surface_cache->shader;
	surface.shader_shadow = p_surface_cache->shader_shadow;
	surface.instanced = p_surface_cache->owner->mesh_instance.is_valid();
	surface.uses_opaque = !uses_alpha_pass;
	surface.uses_transparent = uses_alpha_pass || uses_fade;
	surface.uses_depth = (p_surface_cache->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE | GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW)) != 0;
	surface.can_use_lightmap = p_surface_cache->owner->lightmap_instance.is_valid() || p_surface_cache->owner->lightmap_sh;
	_mesh_compile_pipelines_for_surface(surface, p_global, RSE::PIPELINE_SOURCE_SURFACE);
}

void RenderFlux::_update_dirty_geometry_instances() {
	while (geometry_instance_dirty_list.first()) {
		_geometry_instance_update(geometry_instance_dirty_list.first()->self());
	}

	_update_dirty_geometry_pipelines();
}

void RenderFlux::_update_dirty_geometry_pipelines() {
	if (global_pipeline_data_required.key != global_pipeline_data_compiled.key) {
		// Go through the entire list of surfaces and compile pipelines for everything again.
		SelfList<GeometryInstanceSurfaceDataCache> *list = geometry_surface_compilation_all_list.first();
		while (list != nullptr) {
			GeometryInstanceSurfaceDataCache *surface_cache = list->self();
			_mesh_generate_all_pipelines_for_surface_cache(surface_cache, global_pipeline_data_required);

			if (surface_cache->compilation_dirty_element.in_list()) {
				// Remove any elements from the dirty list as they don't need to be processed again.
				geometry_surface_compilation_dirty_list.remove(&surface_cache->compilation_dirty_element);
			}

			list = list->next();
		}

		global_pipeline_data_compiled.key = global_pipeline_data_required.key;
	} else {
		// Compile pipelines only for the dirty list.
		if (!geometry_surface_compilation_dirty_list.first()) {
			return;
		}

		while (geometry_surface_compilation_dirty_list.first() != nullptr) {
			GeometryInstanceSurfaceDataCache *surface_cache = geometry_surface_compilation_dirty_list.first()->self();
			_mesh_generate_all_pipelines_for_surface_cache(surface_cache, global_pipeline_data_compiled);
			surface_cache->compilation_dirty_element.remove_from_list();
		}
	}
}

void RenderFlux::_geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker) {
	switch (p_notification) {
		case Dependency::DEPENDENCY_CHANGED_MATERIAL:
		case Dependency::DEPENDENCY_CHANGED_MESH:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES_INSTANCES:
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH:
		case Dependency::DEPENDENCY_CHANGED_SKELETON_DATA: {
			static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
			static_cast<GeometryInstanceFlux *>(p_tracker->userdata)->data->dirty_dependencies = true;
		} break;
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH_VISIBLE_INSTANCES: {
			GeometryInstanceFlux *ginstance = static_cast<GeometryInstanceFlux *>(p_tracker->userdata);
			if (ginstance->data->base_type == RSE::INSTANCE_MULTIMESH) {
				ginstance->instance_count = RendererRD::MeshStorage::get_singleton()->multimesh_get_instances_to_draw(ginstance->data->base);
			}
		} break;
		default: {
			//rest of notifications of no interest
		} break;
	}
}
void RenderFlux::_geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker) {
	static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
	static_cast<GeometryInstanceFlux *>(p_tracker->userdata)->data->dirty_dependencies = true;
}

RenderGeometryInstance *RenderFlux::geometry_instance_create(RID p_base) {
	RSE::InstanceType type = RSG::utilities->get_base_type(p_base);
	ERR_FAIL_COND_V(!((1 << type) & RSE::INSTANCE_GEOMETRY_MASK), nullptr);

	GeometryInstanceFlux *ginstance = geometry_instance_alloc.alloc();
	ginstance->data = memnew(GeometryInstanceFlux::Data);

	ginstance->data->base = p_base;
	ginstance->data->base_type = type;
	ginstance->data->dependency_tracker.userdata = ginstance;
	ginstance->data->dependency_tracker.changed_callback = _geometry_instance_dependency_changed;
	ginstance->data->dependency_tracker.deleted_callback = _geometry_instance_dependency_deleted;

	ginstance->_mark_dirty();

	return ginstance;
}

void RenderFlux::GeometryInstanceFlux::set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) {
	uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame != prev_transform_change_frame) {
		prev_transform = transform;
		prev_transform_change_frame = frame;
		transform_status = TransformStatus::MOVED;
	} else if (unlikely(transform_status == TransformStatus::TELEPORTED)) {
		prev_transform = transform;
	}

	RenderGeometryInstanceBase::set_transform(p_transform, p_aabb, p_transformed_aabb);
}

void RenderFlux::GeometryInstanceFlux::reset_motion_vectors() {
	prev_transform = transform;
	transform_status = TransformStatus::TELEPORTED;
}

void RenderFlux::GeometryInstanceFlux::set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) {
	lightmap_instance = p_lightmap_instance;
	lightmap_uv_scale = p_lightmap_uv_scale;
	lightmap_slice_index = p_lightmap_slice_index;

	_mark_dirty();
}

void RenderFlux::GeometryInstanceFlux::set_lightmap_capture(const Color *p_sh9) {
	if (p_sh9) {
		if (lightmap_sh == nullptr) {
			lightmap_sh = RenderFlux::get_singleton()->geometry_instance_lightmap_sh.alloc();
		}

		memcpy(lightmap_sh->sh, p_sh9, sizeof(Color) * 9);
	} else {
		if (lightmap_sh != nullptr) {
			RenderFlux::get_singleton()->geometry_instance_lightmap_sh.free(lightmap_sh);
			lightmap_sh = nullptr;
		}
	}
	_mark_dirty();
}

void RenderFlux::geometry_instance_free(RenderGeometryInstance *p_geometry_instance) {
	GeometryInstanceFlux *ginstance = static_cast<GeometryInstanceFlux *>(p_geometry_instance);
	ERR_FAIL_NULL(ginstance);
	if (ginstance->lightmap_sh != nullptr) {
		geometry_instance_lightmap_sh.free(ginstance->lightmap_sh);
	}
	GeometryInstanceSurfaceDataCache *surf = ginstance->surface_caches;
	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		geometry_instance_surface_alloc.free(surf);
		surf = next;
	}
	memdelete(ginstance->data);
	geometry_instance_alloc.free(ginstance);
}

uint32_t RenderFlux::geometry_instance_get_pair_mask() {
	return (1 << RSE::INSTANCE_VOXEL_GI);
}

void RenderFlux::mesh_generate_pipelines(RID p_mesh, bool p_background_compilation) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);
	uint32_t surface_count = 0;
	const RID *materials = mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);
	Vector<ShaderPipelinePair> pipeline_pairs;
	for (uint32_t i = 0; i < surface_count; i++) {
		if (materials[i].is_null()) {
			continue;
		}

		void *mesh_surface = mesh_storage->mesh_get_surface(p_mesh, i);
		void *mesh_surface_shadow = mesh_surface;
		SceneShaderFlux::MaterialData *material = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(materials[i], RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (material == nullptr || !material->shader_data->is_valid()) {
			continue;
		}

		SceneShaderFlux::ShaderData *shader = material->shader_data;
		SceneShaderFlux::ShaderData *shader_shadow = shader;
		if (material->shader_data->uses_shared_shadow_material()) {
			SceneShaderFlux::MaterialData *material_shadow = static_cast<SceneShaderFlux::MaterialData *>(material_storage->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
			if (material_shadow != nullptr) {
				shader_shadow = material_shadow->shader_data;
				if (shadow_mesh.is_valid()) {
					mesh_surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, i);
				}
			}
		}

		if (!shader->is_valid()) {
			continue;
		}

		SurfacePipelineData surface;
		surface.mesh_surface = mesh_surface;
		surface.mesh_surface_shadow = mesh_surface_shadow;
		surface.shader = shader;
		surface.shader_shadow = shader_shadow;
		surface.instanced = mesh_storage->mesh_needs_instance(p_mesh, true);
		surface.uses_opaque = !material->shader_data->uses_alpha_pass();
		surface.uses_transparent = material->shader_data->uses_alpha_pass();
		surface.uses_depth = surface.uses_opaque || (surface.uses_transparent && material->shader_data->uses_depth_in_alpha_pass());
		surface.can_use_lightmap = mesh_storage->mesh_surface_get_format(mesh_surface) & RSE::ARRAY_FORMAT_TEX_UV2;
		_mesh_compile_pipelines_for_surface(surface, global_pipeline_data_required, RSE::PIPELINE_SOURCE_MESH, &pipeline_pairs);
	}

	// Wait for all the pipelines that were compiled. This will force the loader to wait on all ubershader pipelines to be ready.
	if (!p_background_compilation && !pipeline_pairs.is_empty()) {
		for (ShaderPipelinePair pair : pipeline_pairs) {
			pair.first->pipeline_hash_map.wait_for_pipeline(pair.second.hash());
		}
	}
}

uint32_t RenderFlux::get_pipeline_compilations(RSE::PipelineSource p_source) {
	return scene_shader.get_pipeline_compilations(p_source);
}

void RenderFlux::enable_features(BitField<FeatureBits> p_feature_bits) {
	if (p_feature_bits.has_flag(FEATURE_MULTIVIEW_BIT)) {
		scene_shader.enable_multiview_shader_group();
	}

	if (p_feature_bits.has_flag(FEATURE_ADVANCED_BIT)) {
		scene_shader.enable_advanced_shader_group(p_feature_bits.has_flag(FEATURE_MULTIVIEW_BIT));
	}

	if (p_feature_bits.has_flag(FEATURE_VRS_BIT)) {
		gi.enable_vrs_shader_group();
	}
}

String RenderFlux::get_name() const {
	return "flux";
}

void RenderFlux::GeometryInstanceFlux::pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) {
	if (p_voxel_gi_instance_count > 0) {
		voxel_gi_instances[0] = p_voxel_gi_instances[0];
	} else {
		voxel_gi_instances[0] = RID();
	}

	if (p_voxel_gi_instance_count > 1) {
		voxel_gi_instances[1] = p_voxel_gi_instances[1];
	} else {
		voxel_gi_instances[1] = RID();
	}
}

void RenderFlux::GeometryInstanceFlux::set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) {
	using_projectors = p_projector;
	using_softshadows = p_softshadow;
	_mark_dirty();
}

void RenderFlux::_update_shader_quality_settings() {
	SceneShaderFlux::ShaderSpecialization specialization = {};
	specialization.decal_use_mipmaps = decals_get_filter() == RSE::DECAL_FILTER_NEAREST_MIPMAPS ||
			decals_get_filter() == RSE::DECAL_FILTER_LINEAR_MIPMAPS ||
			decals_get_filter() == RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			decals_get_filter() == RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;
	;
	specialization.projector_use_mipmaps = light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	specialization.soft_shadow_samples = soft_shadow_samples_get();
	specialization.penumbra_shadow_samples = penumbra_shadow_samples_get();
	specialization.directional_soft_shadow_samples = directional_soft_shadow_samples_get();
	specialization.directional_penumbra_shadow_samples = directional_penumbra_shadow_samples_get();
	specialization.use_lightmap_bicubic_filter = lightmap_filter_bicubic_get();
	specialization.fog_use_legacy_blending = fog_use_legacy_blending_get();
	scene_shader.set_default_specialization(specialization);

	base_uniforms_changed(); //also need this
}

RenderFlux::RenderFlux() {
	singleton = this;

	/* SCENE SHADER */

	{
		String defines;
		defines += "\n#define MAX_ROUGHNESS_LOD " + itos(get_roughness_layers() - 1) + ".0\n";
		if (is_using_radiance_octmap_array()) {
			defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY \n";
		}
		defines += "\n#define SDFGI_OCT_SIZE " + itos(gi.sdfgi_get_lightprobe_octahedron_size()) + "\n";
		defines += "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(MAX_DIRECTIONAL_LIGHTS) + "\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (force_vertex_shading) {
			defines += "\n#define USE_VERTEX_LIGHTING\n";
		}

		bool specular_occlusion = GLOBAL_GET("rendering/reflections/specular_occlusion/enabled");
		if (!specular_occlusion) {
			defines += "\n#define SPECULAR_OCCLUSION_DISABLED\n";
		}

		{
			//lightmaps
			scene_state.max_lightmaps = MAX_LIGHTMAPS;
			defines += "\n#define MAX_LIGHTMAP_TEXTURES " + itos(scene_state.max_lightmaps) + "\n";
			defines += "\n#define MAX_LIGHTMAPS " + itos(scene_state.max_lightmaps) + "\n";

			scene_state.lightmap_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapData) * scene_state.max_lightmaps);
		}
		{
			//captures
			scene_state.max_lightmap_captures = 2048;
			scene_state.lightmap_captures = memnew_arr(LightmapCaptureData, scene_state.max_lightmap_captures);
			scene_state.lightmap_capture_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapCaptureData) * scene_state.max_lightmap_captures);
		}
		{
			defines += "\n#define MATERIAL_UNIFORM_SET " + itos(MATERIAL_UNIFORM_SET) + "\n";
		}
#ifdef REAL_T_IS_DOUBLE
		{
			defines += "\n#define USE_DOUBLE_PRECISION \n";
		}
#endif

		scene_shader.init(defines);
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		virtual_geometry_raster.shader.initialize(modes);
		virtual_geometry_raster.shader_version = virtual_geometry_raster.shader.version_create();
		virtual_geometry_raster.pipeline = RD::get_singleton()->compute_pipeline_create(virtual_geometry_raster.shader.version_get_shader(virtual_geometry_raster.shader_version, 0));

		Vector<RD::VertexAttribute> attributes;
		auto add_attribute = [&attributes](uint32_t p_binding, uint32_t p_location, uint32_t p_offset, RD::DataFormat p_format, uint32_t p_stride) {
			RD::VertexAttribute attribute;
			attribute.binding = p_binding;
			attribute.location = p_location;
			attribute.offset = p_offset;
			attribute.format = p_format;
			attribute.stride = p_stride;
			attributes.push_back(attribute);
		};
		add_attribute(0, 0, 0, RD::DATA_FORMAT_R32G32B32_SFLOAT, 12);
		add_attribute(1, 1, 0, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		add_attribute(1, 3, 32, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		add_attribute(1, 4, 16, RD::DATA_FORMAT_R32G32_SFLOAT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		add_attribute(1, 5, 24, RD::DATA_FORMAT_R32G32_SFLOAT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		add_attribute(1, 10, 48, RD::DATA_FORMAT_R32G32B32A32_UINT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		add_attribute(1, 11, 64, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		add_attribute(0, 12, 0, RD::DATA_FORMAT_R32G32B32_SFLOAT, 12);
		add_attribute(1, 13, 0, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, sizeof(RendererVirtualGeometry::VirtualGeometryGPUVertexAttributes));
		virtual_geometry_raster.vertex_format = RD::get_singleton()->vertex_format_create(attributes);
	}

	/* shadow sampler */
	{
		RD::SamplerState sampler;
		sampler.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_GREATER;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		best_fit_normal.shader.initialize(modes);
		best_fit_normal.shader_version = best_fit_normal.shader.version_create();
		best_fit_normal.pipeline = RD::get_singleton()->compute_pipeline_create(best_fit_normal.shader.version_get_shader(best_fit_normal.shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R8_UNORM;
		tformat.width = 1024;
		tformat.height = 1024;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		best_fit_normal.texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = best_fit_normal.shader.version_get_shader(best_fit_normal.shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(best_fit_normal.texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, best_fit_normal.pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();
	}

	/* DFG LUT */
	{
		Vector<String> modes;
		modes.push_back("\n");
		dfg_lut.shader.initialize(modes);
		dfg_lut.shader_version = dfg_lut.shader.version_create();
		dfg_lut.pipeline = RD::get_singleton()->compute_pipeline_create(dfg_lut.shader.version_get_shader(dfg_lut.shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R16G16_SFLOAT;
		tformat.width = 128;
		tformat.height = 128;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		dfg_lut.texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = dfg_lut.shader.version_get_shader(dfg_lut.shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dfg_lut.texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, dfg_lut.pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();
	}

	_update_shader_quality_settings();
	_update_global_pipeline_data_requirements_from_project();

	taa = memnew(RendererRD::TAA);
	fsr2_effect = memnew(RendererRD::FSR2Effect);
	ss_effects = memnew(RendererRD::SSEffects);
#ifdef METAL_ENABLED
	metal_flux_effect = memnew(RendererRD::MetalFluxEffect);
#endif
#ifdef METAL_MFXTEMPORAL_ENABLED
	motion_vectors_store = memnew(RendererRD::MotionVectorsStore);
	mfx_temporal_effect = memnew(RendererRD::MFXTemporalEffect);
	mfx_denoised_effect = memnew(RendererRD::MFXDenoisedEffect);
#endif
}

RenderFlux::~RenderFlux() {
	for (const VirtualGeometryRasterState::Buffers &buffers : virtual_geometry_raster.buffers) {
		if (buffers.candidates.is_valid()) RD::get_singleton()->free_rid(buffers.candidates);
		if (buffers.commands.is_valid()) RD::get_singleton()->free_rid(buffers.commands);
		if (buffers.counters.is_valid()) RD::get_singleton()->free_rid(buffers.counters);
	}
	if (virtual_geometry_raster.pipeline.is_valid()) RD::get_singleton()->free_rid(virtual_geometry_raster.pipeline);
	if (virtual_geometry_raster.shader_version.is_valid()) virtual_geometry_raster.shader.version_free(virtual_geometry_raster.shader_version);
#ifdef METAL_ENABLED
	if (metal_flux_effect) {
		memdelete(metal_flux_effect);
		metal_flux_effect = nullptr;
	}
#endif

	if (ss_effects != nullptr) {
		memdelete(ss_effects);
		ss_effects = nullptr;
	}

	if (taa != nullptr) {
		memdelete(taa);
		taa = nullptr;
	}

	if (fsr2_effect) {
		memdelete(fsr2_effect);
		fsr2_effect = nullptr;
	}

#ifdef METAL_MFXTEMPORAL_ENABLED
	if (mfx_temporal_effect) {
		memdelete(mfx_temporal_effect);
		mfx_temporal_effect = nullptr;
	}
	if (mfx_denoised_effect) {
		memdelete(mfx_denoised_effect);
		mfx_denoised_effect = nullptr;
	}

	if (motion_vectors_store) {
		memdelete(motion_vectors_store);
		motion_vectors_store = nullptr;
	}
#endif

	RD::get_singleton()->free_rid(shadow_sampler);
	RSG::light_storage->directional_shadow_atlas_set_size(0);

	RD::get_singleton()->free_rid(best_fit_normal.pipeline);
	RD::get_singleton()->free_rid(best_fit_normal.texture);
	best_fit_normal.shader.version_free(best_fit_normal.shader_version);

	RD::get_singleton()->free_rid(dfg_lut.pipeline);
	RD::get_singleton()->free_rid(dfg_lut.texture);
	dfg_lut.shader.version_free(dfg_lut.shader_version);

	if (ltc.lut1_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc.lut1_texture);
	}
	if (ltc.lut2_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc.lut2_texture);
	}

	{
		for (const RID &rid : scene_state.uniform_buffers) {
			RD::get_singleton()->free_rid(rid);
		}
		for (const RID &rid : scene_state.implementation_uniform_buffers) {
			RD::get_singleton()->free_rid(rid);
		}
		RD::get_singleton()->free_rid(scene_state.lightmap_buffer);
		RD::get_singleton()->free_rid(scene_state.lightmap_capture_buffer);
		for (uint32_t i = 0; i < RENDER_LIST_MAX; i++) {
			scene_state.instance_buffer[i].uninit();
		}
		memdelete_arr(scene_state.lightmap_captures);
	}

	while (sdfgi_framebuffer_size_cache.begin()) {
		RD::get_singleton()->free_rid(sdfgi_framebuffer_size_cache.begin()->value);
		sdfgi_framebuffer_size_cache.remove(sdfgi_framebuffer_size_cache.begin());
	}
}
