/**************************************************************************/
/*  metal_fx.h                                                            */
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

#if defined(METAL_ENABLED) && !defined(VISIONOS_ENABLED)
#define METAL_MFXTEMPORAL_ENABLED
#endif

#ifdef METAL_ENABLED

#include "core/templates/paged_allocator.h"
#include "servers/rendering/path_tracing/hybrid_reconstruction.h"
#include "servers/rendering/path_tracing/path_tracing_scene_packet.h"
#include "servers/rendering/renderer_rd/effects/spatial_upscaler.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_device_driver.h"


namespace MTLFX {
class SpatialScalerBase;
class TemporalScalerBase;
class TemporalDenoisedScalerBase;
} //namespace MTLFX

namespace RendererRD {

struct MFXSpatialContext {
	MTLFX::SpatialScalerBase *scaler = nullptr;
	MFXSpatialContext() = default;
	~MFXSpatialContext();
};

class MFXSpatialEffect : public SpatialUpscaler {
	struct CallbackArgs {
		MFXSpatialEffect *owner = nullptr;
		MTLFX::SpatialScalerBase *scaler = nullptr;
		RDD::TextureID src;
		RDD::TextureID dst;

		CallbackArgs(MFXSpatialEffect *p_owner, RDD::TextureID p_src, RDD::TextureID p_dst, const MFXSpatialContext &p_ctx) :
				owner(p_owner), scaler(p_ctx.scaler), src(p_src), dst(p_dst) {}

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;
	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	virtual const Span<char> get_label() const final { return "MetalFX Spatial Upscale"; }
	virtual void ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) final;
	virtual void process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_src, RID p_dst) final;

	struct CreateParams {
		Vector2i input_size;
		Vector2i output_size;
		RDD::DataFormat input_format;
		RDD::DataFormat output_format;
	};

	MFXSpatialContext *create_context(CreateParams p_params) const;

	MFXSpatialEffect();
	~MFXSpatialEffect();
};

#ifdef METAL_MFXTEMPORAL_ENABLED

struct MFXTemporalContext {
	MTLFX::TemporalScalerBase *scaler = nullptr;
	MFXTemporalContext() = default;
	~MFXTemporalContext();
};

class MFXTemporalEffect {
	struct CallbackArgs {
		MFXTemporalEffect *owner = nullptr;
		MTLFX::TemporalScalerBase *scaler = nullptr;
		RDD::TextureID src;
		RDD::TextureID depth;
		RDD::TextureID motion;
		RDD::TextureID exposure;
		Vector2 jitter_offset;
		RDD::TextureID dst;
		bool reset = false;

		CallbackArgs(
				MFXTemporalEffect *p_owner,
				RDD::TextureID p_src,
				RDD::TextureID p_depth,
				RDD::TextureID p_motion,
				RDD::TextureID p_exposure,
				Vector2 p_jitter_offset,
				RDD::TextureID p_dst,
				const MFXTemporalContext &p_ctx,
				bool p_reset) :
				owner(p_owner),
				scaler(p_ctx.scaler),
				src(p_src),
				depth(p_depth),
				motion(p_motion),
				exposure(p_exposure),
				jitter_offset(p_jitter_offset),
				dst(p_dst),
				reset(p_reset) {}

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	MFXTemporalEffect();
	~MFXTemporalEffect();

	struct CreateParams {
		Vector2i input_size;
		Vector2i output_size;
		RDD::DataFormat input_format;
		RDD::DataFormat depth_format;
		RDD::DataFormat motion_format;
		RDD::DataFormat reactive_format;
		RDD::DataFormat output_format;
		Vector2 motion_vector_scale;
	};

	MFXTemporalContext *create_context(CreateParams p_params) const;

	struct Params {
		RID src;
		RID depth;
		RID motion;
		RID exposure;
		RID dst;
		Vector2 jitter_offset;
		bool reset = false;
	};

	void process(MFXTemporalContext *p_ctx, Params p_params);
};

struct MFXDenoisedContext {
	MTLFX::TemporalDenoisedScalerBase *scaler = nullptr;
	MFXDenoisedContext() = default;
	~MFXDenoisedContext();
};

class MFXDenoisedEffect {
	struct CallbackArgs {
		MFXDenoisedEffect *owner = nullptr;
		MTLFX::TemporalDenoisedScalerBase *scaler = nullptr;
		RDD::TextureID color;
		RDD::TextureID depth;
		RDD::TextureID motion;
		RDD::TextureID normal;
		RDD::TextureID diffuse;
		RDD::TextureID specular;
		RDD::TextureID roughness;
		RDD::TextureID denoise_strength;
		RDD::TextureID reactive;
		RDD::TextureID specular_distance;
		RDD::TextureID transparency;
		RDD::TextureID output;
		RendererPathTracing::Matrix4 view_from_world;
		RendererPathTracing::Matrix4 clip_from_view;
		Vector2 jitter_offset;
		Vector2 motion_vector_scale;
		float pre_exposure = 1.0f;
		bool reset = false;

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;
	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	struct CreateParams {
		Vector2i input_size;
		Vector2i output_size;
		RDD::DataFormat color_format;
		RDD::DataFormat depth_format;
		RDD::DataFormat motion_format;
		RDD::DataFormat normal_format;
		RDD::DataFormat diffuse_format;
		RDD::DataFormat specular_format;
		RDD::DataFormat roughness_format;
		RDD::DataFormat denoise_strength_format;
		RDD::DataFormat reactive_format;
		RDD::DataFormat specular_distance_format;
		RDD::DataFormat transparency_format;
		RDD::DataFormat output_format;
	};

	struct Params {
		RID color;
		RID depth;
		RID motion;
		RID normal;
		RID diffuse;
		RID specular;
		RID roughness;
		RID denoise_strength;
		RID reactive;
		RID specular_distance;
		RID transparency;
		RID output;
		RendererPathTracing::Matrix4 view_from_world;
		RendererPathTracing::Matrix4 clip_from_view;
		Vector2 jitter_offset;
		Vector2 motion_vector_scale;
		float pre_exposure = 1.0f;
		bool reset = false;
	};

	bool is_supported() const;
	MFXDenoisedContext *create_context(const CreateParams &p_params, String *r_error = nullptr) const;
	Error process(MFXDenoisedContext *p_context, const Params &p_params, String *r_error = nullptr);
};

class MFXHybridReconstructionContext : public RendererPathTracing::HybridReconstructionContext {
public:
	RendererPathTracing::HybridReconstructionConfig config;
	Vector<MFXDenoisedContext *> views;

	~MFXHybridReconstructionContext() override;
};

class MFXHybridReconstructionAdapter : public RendererPathTracing::HybridReconstructionAdapter {
	MFXDenoisedEffect effect;

public:
	RendererPathTracing::ReconstructionCapabilities get_capabilities() const override;
	RendererPathTracing::HybridReconstructionContext *create_context(const RendererPathTracing::HybridReconstructionConfig &p_config, String *r_error = nullptr) override;
	Error process(RendererPathTracing::HybridReconstructionContext *p_context, const RendererPathTracing::HybridReconstructionFrame &p_frame, String *r_error = nullptr) override;
};

#endif

} //namespace RendererRD

#endif // METAL_ENABLED
