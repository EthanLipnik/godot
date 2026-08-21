/**************************************************************************/
/*  test_hybrid_runtime.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_hybrid_runtime)

#include "servers/rendering/path_tracing/hybrid_reconstruction.h"
#include "servers/rendering/path_tracing/hybrid_runtime.h"

namespace TestHybridRuntime {

using namespace RendererPathTracing;

TEST_CASE("[Rendering][Hybrid] Reconstruction selection is capability-driven and fail-closed") {
	ReconstructionCapabilities unavailable_dlss;
	unavailable_dlss.name = "DLSS Ray Reconstruction";
	unavailable_dlss.kind = ReconstructionKind::DLSS_RAY_RECONSTRUCTION;
	unavailable_dlss.required_guides = GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO | GUIDE_SPECULAR_ALBEDO | GUIDE_ROUGHNESS;
	unavailable_dlss.unavailable_reason = "The optional DLSS runtime is not installed.";

	ReconstructionCapabilities metal_fx;
	metal_fx.name = "MetalFX Temporal Denoised";
	metal_fx.kind = ReconstructionKind::METALFX;
	metal_fx.available = true;
	metal_fx.temporal_denoising = true;
	metal_fx.super_resolution = true;
	metal_fx.stereo = true;
	metal_fx.max_views = 2;
	metal_fx.required_guides = GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO | GUIDE_SPECULAR_ALBEDO |
			GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH | GUIDE_REACTIVE_MASK | GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY;

	Vector<ReconstructionCapabilities> candidates;
	candidates.push_back(unavailable_dlss);
	candidates.push_back(metal_fx);

	ReconstructionRequest request;
	request.view_count = 2;
	request.available_guides = metal_fx.required_guides;
	ReconstructionSelection selection = select_reconstruction_adapter(request, candidates);
	CHECK(selection.is_available());
	CHECK(selection.adapter_index == 1);
	CHECK(selection.kind == ReconstructionKind::METALFX);

	request.available_guides &= ~GUIDE_SPECULAR_HIT_DISTANCE;
	selection = select_reconstruction_adapter(request, candidates);
	CHECK_FALSE(selection.is_available());
	CHECK(selection.unavailable_reason.contains("specular") == false);
	CHECK(selection.unavailable_reason.contains("missing"));
}

TEST_CASE("[Rendering][Hybrid] Denoised reconstruction is selected only with the complete guide contract") {
	const uint32_t temporal_guides = GUIDE_DEPTH | GUIDE_MOTION;
	const uint32_t denoised_guides = temporal_guides | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO | GUIDE_SPECULAR_ALBEDO |
			GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH | GUIDE_REACTIVE_MASK | GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY;

	ReconstructionCapabilities denoised;
	denoised.name = "MetalFX Temporal Denoised";
	denoised.kind = ReconstructionKind::METALFX;
	denoised.available = true;
	denoised.temporal_denoising = true;
	denoised.super_resolution = true;
	denoised.max_views = 1;
	denoised.required_guides = denoised_guides;

	ReconstructionCapabilities ordinary;
	ordinary.name = "MetalFX Temporal";
	ordinary.kind = ReconstructionKind::METALFX;
	ordinary.available = true;
	ordinary.temporal_denoising = true;
	ordinary.super_resolution = true;
	ordinary.max_views = 1;
	ordinary.required_guides = temporal_guides;

	Vector<ReconstructionCapabilities> candidates;
	candidates.push_back(denoised);
	candidates.push_back(ordinary);

	ReconstructionRequest request;
	request.available_guides = denoised_guides;
	ReconstructionSelection selection = select_reconstruction_adapter(request, candidates);
	CHECK(selection.is_available());
	CHECK(selection.adapter_index == 0);
	CHECK(selection.adapter_name == denoised.name);

	request.available_guides &= ~GUIDE_SPECULAR_HIT_DISTANCE;
	selection = select_reconstruction_adapter(request, candidates);
	CHECK(selection.is_available());
	CHECK(selection.adapter_index == 1);
	CHECK(selection.adapter_name == ordinary.name);
}

TEST_CASE("[Rendering][Hybrid] Scene tracking distinguishes rebuild, refit, reuse, and deferred retirement") {
	HybridSceneTracker tracker(2);
	HybridSceneRevision revision = { 10, 20, 30, 40 };

	tracker.begin_frame(1);
	CHECK(tracker.touch(7, revision, true) == (HYBRID_SCENE_UPDATE_BUILD_BLAS | HYBRID_SCENE_UPDATE_TLAS | HYBRID_SCENE_UPDATE_MATERIAL));
	tracker.end_frame();
	CHECK(tracker.get_frame_stats().blas_built == 1);
	CHECK(tracker.get_tracked_count() == 1);

	tracker.begin_frame(2);
	CHECK(tracker.touch(7, revision, true) == HYBRID_SCENE_UPDATE_NONE);
	tracker.end_frame();
	CHECK(tracker.get_frame_stats().blas_reused == 1);

	revision.deformation++;
	tracker.begin_frame(3);
	CHECK(tracker.touch(7, revision, true) == HYBRID_SCENE_UPDATE_REFIT_BLAS);
	tracker.end_frame();
	CHECK(tracker.get_frame_stats().blas_refit == 1);

	revision.transform++;
	tracker.begin_frame(4);
	CHECK(tracker.touch(7, revision, true) == HYBRID_SCENE_UPDATE_TLAS);
	tracker.end_frame();

	revision.topology++;
	tracker.begin_frame(5);
	CHECK((tracker.touch(7, revision, true) & HYBRID_SCENE_UPDATE_BUILD_BLAS) != 0);
	tracker.end_frame();

	tracker.begin_frame(6);
	tracker.end_frame();
	CHECK(tracker.get_frame_stats().pending_retirements == 1);
	tracker.begin_frame(7);
	tracker.end_frame();
	CHECK(tracker.get_tracked_count() == 1);
	tracker.begin_frame(8);
	tracker.end_frame();
	CHECK(tracker.get_frame_stats().retired == 1);
	CHECK(tracker.get_tracked_count() == 0);
}

TEST_CASE("[Rendering][Hybrid] Reconstruction contexts require explicit formats and complete per-view guides") {
	ReconstructionCapabilities capabilities;
	capabilities.available = true;
	capabilities.temporal_denoising = true;
	capabilities.super_resolution = true;
	capabilities.stereo = true;
	capabilities.max_views = 2;

	HybridReconstructionConfig config;
	config.input_size = Vector2i(960, 540);
	config.output_size = Vector2i(1920, 1080);
	config.view_count = 2;
	CHECK(validate_reconstruction_config(config, capabilities) == ERR_INVALID_PARAMETER);

	config.color_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	config.depth_format = RD::DATA_FORMAT_R32_SFLOAT;
	config.motion_format = RD::DATA_FORMAT_R16G16_SFLOAT;
	config.normal_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	config.diffuse_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	config.specular_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	config.roughness_format = RD::DATA_FORMAT_R16_SFLOAT;
	config.denoise_strength_format = RD::DATA_FORMAT_R8_UNORM;
	config.reactive_format = RD::DATA_FORMAT_R8_UNORM;
	config.specular_distance_format = RD::DATA_FORMAT_R16_SFLOAT;
	config.transparency_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	config.output_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	CHECK(validate_reconstruction_config(config, capabilities) == OK);

	HybridReconstructionFrame frame;
	frame.view_index = 2;
	CHECK(validate_reconstruction_frame(config, frame) == ERR_INVALID_PARAMETER);
	frame.view_index = 0;
	CHECK(validate_reconstruction_frame(config, frame) == ERR_INVALID_PARAMETER);
}

} // namespace TestHybridRuntime
