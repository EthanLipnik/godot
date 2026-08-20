/**************************************************************************/
/*  metal_fx.cpp                                                          */
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

#ifdef METAL_ENABLED

#include "metal_fx.h"

#include "drivers/metal/pixel_formats.h"
#include "drivers/metal/rendering_device_driver_metal3.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#include <MetalFX/MetalFX.hpp>

using namespace RendererRD;

#pragma mark - Spatial Scaler

MFXSpatialContext::~MFXSpatialContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXSpatialEffect::MFXSpatialEffect() {
}

MFXSpatialEffect::~MFXSpatialEffect() {
}

void MFXSpatialEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::SpatialScalerBase *scaler = p_userdata->scaler;
	scaler->setColorTexture(src_texture);
	scaler->setOutputTexture(dst_texture);
	MTLFX::SpatialScaler *s = static_cast<MTLFX::SpatialScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

void MFXSpatialEffect::ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) {
	p_render_buffers->ensure_mfx(this);
}

void MFXSpatialEffect::process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_src, RID p_dst) {
	MFXSpatialContext *ctx = p_render_buffers->get_mfx_spatial_context();
	DEV_ASSERT(ctx); // this should have been done by the caller via ensure_context

	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_dst)),
			*ctx);
	RD::CallbackResource res[2] = {
		{ .rid = p_src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE }
	};
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXSpatialEffect::callback, userdata, VectorView<RD::CallbackResource>(res, 2));
}

MFXSpatialContext *MFXSpatialEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_SPATIAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::SpatialScalerDescriptor> desc = NS::TransferPtr(MTLFX::SpatialScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));
	desc->setColorProcessingMode(MTLFX::SpatialScalerColorProcessingModeLinear);

	MFXSpatialContext *context = memnew(MFXSpatialContext);
	context->scaler = desc->newSpatialScaler(dev);

	return context;
}

#ifdef METAL_MFXTEMPORAL_ENABLED

#pragma mark - Temporal Scaler

MFXTemporalContext::~MFXTemporalContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXTemporalEffect::MFXTemporalEffect() {}
MFXTemporalEffect::~MFXTemporalEffect() {}

MFXTemporalContext *MFXTemporalEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_TEMPORAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::TemporalScalerDescriptor> desc = NS::TransferPtr(MTLFX::TemporalScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setDepthTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.depth_format));
	desc->setMotionTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.motion_format));
	desc->setAutoExposureEnabled(false);

	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));

	MFXTemporalContext *context = memnew(MFXTemporalContext);
	context->scaler = desc->newTemporalScaler(dev);
	context->scaler->setMotionVectorScaleX(p_params.motion_vector_scale.x);
	context->scaler->setMotionVectorScaleY(p_params.motion_vector_scale.y);
	context->scaler->setDepthReversed(true); // Godot uses reverse Z per https://github.com/godotengine/godot/pull/88328

	return context;
}

void MFXTemporalEffect::process(RendererRD::MFXTemporalContext *p_ctx, RendererRD::MFXTemporalEffect::Params p_params) {
	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.depth)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.motion)),
			p_params.exposure.is_valid() ? RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.exposure)) : RDD::TextureID(),
			p_params.jitter_offset,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.dst)),
			*p_ctx,
			p_params.reset);
	RD::CallbackResource res[3] = {
		{ .rid = p_params.src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
	};
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXTemporalEffect::callback, userdata, VectorView<RD::CallbackResource>(res, 3));
}

void MFXTemporalEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *depth = reinterpret_cast<MTL::Texture *>(p_userdata->depth.id);
	MTL::Texture *motion = reinterpret_cast<MTL::Texture *>(p_userdata->motion.id);
	MTL::Texture *exposure = reinterpret_cast<MTL::Texture *>(p_userdata->exposure.id);

	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::TemporalScalerBase *scaler = p_userdata->scaler;
	scaler->setReset(p_userdata->reset);
	scaler->setColorTexture(src_texture);
	scaler->setDepthTexture(depth);
	scaler->setMotionTexture(motion);
	scaler->setExposureTexture(exposure);
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	scaler->setOutputTexture(dst_texture);
	MTLFX::TemporalScaler *s = static_cast<MTLFX::TemporalScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

MFXDenoisedContext::~MFXDenoisedContext() {
	if (scaler) {
		scaler->release();
	}
}

bool MFXDenoisedEffect::is_supported() const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || rd->get_device_api_name() != "Metal") {
		return false;
	}
	RenderingDeviceDriverMetal *rdd = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	return MTLFX::TemporalDenoisedScalerDescriptor::supportsDevice(rdd->get_device());
}

MFXDenoisedContext *MFXDenoisedEffect::create_context(const CreateParams &p_params, String *r_error) const {
	if (!is_supported()) {
		if (r_error) {
			*r_error = "The active Metal device does not support temporal denoised MetalFX scaling.";
		}
		return nullptr;
	}
	RenderingDeviceDriverMetal *rdd = static_cast<RenderingDeviceDriverMetal *>(RD::get_singleton()->get_device_driver());
	PixelFormats &formats = rdd->get_pixel_formats();
	NS::SharedPtr<MTLFX::TemporalDenoisedScalerDescriptor> descriptor = NS::TransferPtr(MTLFX::TemporalDenoisedScalerDescriptor::alloc()->init());
	descriptor->setInputWidth(p_params.input_size.x);
	descriptor->setInputHeight(p_params.input_size.y);
	descriptor->setOutputWidth(p_params.output_size.x);
	descriptor->setOutputHeight(p_params.output_size.y);
	descriptor->setColorTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.color_format));
	descriptor->setDepthTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.depth_format));
	descriptor->setMotionTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.motion_format));
	descriptor->setNormalTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.normal_format));
	descriptor->setDiffuseAlbedoTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.diffuse_format));
	descriptor->setSpecularAlbedoTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.specular_format));
	descriptor->setRoughnessTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.roughness_format));
	descriptor->setDenoiseStrengthMaskTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.denoise_strength_format));
	descriptor->setReactiveMaskTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.reactive_format));
	descriptor->setSpecularHitDistanceTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.specular_distance_format));
	descriptor->setTransparencyOverlayTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.transparency_format));
	descriptor->setOutputTextureFormat((MTL::PixelFormat)formats.getMTLPixelFormat(p_params.output_format));
	descriptor->setAutoExposureEnabled(false);
	descriptor->setReactiveMaskTextureEnabled(true);
	descriptor->setDenoiseStrengthMaskTextureEnabled(true);
	descriptor->setSpecularHitDistanceTextureEnabled(true);
	descriptor->setTransparencyOverlayTextureEnabled(true);
	descriptor->setRequiresSynchronousInitialization(true);
	MFXDenoisedContext *context = memnew(MFXDenoisedContext);
	context->scaler = descriptor->newTemporalDenoisedScaler(rdd->get_device());
	if (!context->scaler) {
		memdelete(context);
		if (r_error) {
			*r_error = "MetalFX rejected the temporal denoised path-tracing guide configuration.";
		}
		return nullptr;
	}
	context->scaler->setDepthReversed(true);
	if (r_error) {
		r_error->clear();
	}
	return context;
}

static simd::float4x4 _mfx_matrix(const RendererPathTracing::Matrix4 &p_matrix) {
	simd::float4x4 matrix;
	for (uint32_t column = 0; column < 4; column++) {
		matrix.columns[column] = simd_make_float4(p_matrix.columns[column].x, p_matrix.columns[column].y, p_matrix.columns[column].z, p_matrix.columns[column].w);
	}
	return matrix;
}

void MFXDenoisedEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	(void)p_driver;
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTLFX::TemporalDenoisedScalerBase *scaler = p_userdata->scaler;
	scaler->setColorTexture(reinterpret_cast<MTL::Texture *>(p_userdata->color.id));
	scaler->setDepthTexture(reinterpret_cast<MTL::Texture *>(p_userdata->depth.id));
	scaler->setMotionTexture(reinterpret_cast<MTL::Texture *>(p_userdata->motion.id));
	scaler->setNormalTexture(reinterpret_cast<MTL::Texture *>(p_userdata->normal.id));
	scaler->setDiffuseAlbedoTexture(reinterpret_cast<MTL::Texture *>(p_userdata->diffuse.id));
	scaler->setSpecularAlbedoTexture(reinterpret_cast<MTL::Texture *>(p_userdata->specular.id));
	scaler->setRoughnessTexture(reinterpret_cast<MTL::Texture *>(p_userdata->roughness.id));
	scaler->setDenoiseStrengthMaskTexture(reinterpret_cast<MTL::Texture *>(p_userdata->denoise_strength.id));
	scaler->setReactiveMaskTexture(reinterpret_cast<MTL::Texture *>(p_userdata->reactive.id));
	scaler->setSpecularHitDistanceTexture(reinterpret_cast<MTL::Texture *>(p_userdata->specular_distance.id));
	scaler->setTransparencyOverlayTexture(reinterpret_cast<MTL::Texture *>(p_userdata->transparency.id));
	scaler->setOutputTexture(reinterpret_cast<MTL::Texture *>(p_userdata->output.id));
	scaler->setWorldToViewMatrix(_mfx_matrix(p_userdata->view_from_world));
	scaler->setViewToClipMatrix(_mfx_matrix(p_userdata->clip_from_view));
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	scaler->setMotionVectorScaleX(p_userdata->motion_vector_scale.x);
	scaler->setMotionVectorScaleY(p_userdata->motion_vector_scale.y);
	scaler->setPreExposure(p_userdata->pre_exposure);
	scaler->setShouldResetHistory(p_userdata->reset);
	MTLFX::TemporalDenoisedScaler *denoised_scaler = static_cast<MTLFX::TemporalDenoisedScaler *>(scaler);
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	denoised_scaler->encodeToCommandBuffer(metal_command->get_command_buffer());
	command->retain_resource(scaler);
	CallbackArgs::free(&p_userdata);
}

Error MFXDenoisedEffect::process(MFXDenoisedContext *p_context, const Params &p_params, String *r_error) {
	if (!p_context || !p_context->scaler) {
		if (r_error) {
			*r_error = "A valid MetalFX temporal denoised context is required.";
		}
		return ERR_INVALID_PARAMETER;
	}
	const RID inputs[] = { p_params.color, p_params.depth, p_params.motion, p_params.normal, p_params.diffuse, p_params.specular,
		p_params.roughness, p_params.denoise_strength, p_params.reactive, p_params.specular_distance, p_params.transparency };
	for (const RID &input : inputs) {
		if (!input.is_valid()) {
			if (r_error) {
				*r_error = "MetalFX temporal denoising requires the complete guide set.";
			}
			return ERR_INVALID_PARAMETER;
		}
	}
	if (!p_params.output.is_valid()) {
		if (r_error) {
			*r_error = "MetalFX temporal denoising requires a valid output texture.";
		}
		return ERR_INVALID_PARAMETER;
	}
	CallbackArgs *arguments = args_allocator.alloc();
	arguments->owner = this;
	arguments->scaler = p_context->scaler;
	RDD::TextureID *native_inputs[] = { &arguments->color, &arguments->depth, &arguments->motion, &arguments->normal, &arguments->diffuse,
		&arguments->specular, &arguments->roughness, &arguments->denoise_strength, &arguments->reactive, &arguments->specular_distance, &arguments->transparency };
	RD::CallbackResource resources[12];
	for (uint32_t index = 0; index < 11; index++) {
		*native_inputs[index] = RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, inputs[index]));
		resources[index] = { .rid = inputs[index], .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
	}
	arguments->output = RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.output));
	arguments->view_from_world = p_params.view_from_world;
	arguments->clip_from_view = p_params.clip_from_view;
	arguments->jitter_offset = p_params.jitter_offset;
	arguments->motion_vector_scale = p_params.motion_vector_scale;
	arguments->pre_exposure = p_params.pre_exposure;
	arguments->reset = p_params.reset;
	resources[11] = { .rid = p_params.output, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE };
	RD::get_singleton()->capture_timestamp("Path Tracing Reconstruct Begin");
	const Error error = RD::get_singleton()->driver_callback_add((RDD::DriverCallback)callback, arguments, VectorView<RD::CallbackResource>(resources, 12));
	RD::get_singleton()->capture_timestamp("Path Tracing Reconstruct End");
	if (error != OK) {
		CallbackArgs::free(&arguments);
		if (r_error) {
			*r_error = "MetalFX temporal denoising could not be scheduled.";
		}
		return error;
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

#endif

#endif
