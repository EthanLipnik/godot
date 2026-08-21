/**************************************************************************/
/*  hybrid_reconstruction.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "hybrid_runtime.h"

#include "servers/rendering/rendering_device.h"

namespace RendererPathTracing {

struct HybridReconstructionConfig {
	Vector2i input_size;
	Vector2i output_size;
	uint32_t view_count = 1;
	RD::DataFormat color_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat depth_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat motion_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat normal_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat diffuse_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat specular_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat roughness_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat denoise_strength_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat reactive_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat specular_distance_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat transparency_format = RD::DATA_FORMAT_MAX;
	RD::DataFormat output_format = RD::DATA_FORMAT_MAX;
};

struct HybridReconstructionFrame {
	uint32_t view_index = 0;
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
	Matrix4 view_from_world;
	Matrix4 clip_from_view;
	Vector2 jitter_offset;
	Vector2 motion_vector_scale;
	float pre_exposure = 1.0f;
	bool reset_history = false;
};

class HybridReconstructionContext {
public:
	virtual ~HybridReconstructionContext() = default;
};

class HybridReconstructionAdapter {
public:
	virtual ~HybridReconstructionAdapter() = default;
	virtual ReconstructionCapabilities get_capabilities() const = 0;
	virtual HybridReconstructionContext *create_context(const HybridReconstructionConfig &p_config, String *r_error = nullptr) = 0;
	virtual Error process(HybridReconstructionContext *p_context, const HybridReconstructionFrame &p_frame, String *r_error = nullptr) = 0;
};

Error validate_reconstruction_config(const HybridReconstructionConfig &p_config, const ReconstructionCapabilities &p_capabilities, String *r_error = nullptr);
Error validate_reconstruction_frame(const HybridReconstructionConfig &p_config, const HybridReconstructionFrame &p_frame, String *r_error = nullptr);

} // namespace RendererPathTracing
