/**************************************************************************/
/*  hybrid_reconstruction.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "hybrid_reconstruction.h"

namespace RendererPathTracing {

static Error _reconstruction_fail(Error p_error, const String &p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

Error validate_reconstruction_config(const HybridReconstructionConfig &p_config, const ReconstructionCapabilities &p_capabilities, String *r_error) {
	if (!p_capabilities.available) {
		return _reconstruction_fail(ERR_UNAVAILABLE, p_capabilities.unavailable_reason, r_error);
	}
	if (p_config.input_size.x <= 0 || p_config.input_size.y <= 0 || p_config.output_size.x <= 0 || p_config.output_size.y <= 0) {
		return _reconstruction_fail(ERR_INVALID_PARAMETER, "Hybrid reconstruction dimensions must be positive.", r_error);
	}
	if (p_config.view_count == 0 || p_config.view_count > p_capabilities.max_views || (p_config.view_count > 1 && !p_capabilities.stereo)) {
		return _reconstruction_fail(ERR_UNAVAILABLE, "The reconstruction adapter cannot maintain the requested independent view histories.", r_error);
	}
	const RD::DataFormat formats[] = {
		p_config.color_format,
		p_config.depth_format,
		p_config.motion_format,
		p_config.normal_format,
		p_config.diffuse_format,
		p_config.specular_format,
		p_config.roughness_format,
		p_config.denoise_strength_format,
		p_config.reactive_format,
		p_config.specular_distance_format,
		p_config.transparency_format,
		p_config.output_format,
	};
	for (RD::DataFormat format : formats) {
		if (format == RD::DATA_FORMAT_MAX) {
			return _reconstruction_fail(ERR_INVALID_PARAMETER, "Hybrid reconstruction requires an explicit format for every guide and output.", r_error);
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error validate_reconstruction_frame(const HybridReconstructionConfig &p_config, const HybridReconstructionFrame &p_frame, String *r_error) {
	if (p_frame.view_index >= p_config.view_count) {
		return _reconstruction_fail(ERR_INVALID_PARAMETER, "Hybrid reconstruction view index is out of range.", r_error);
	}
	const RID textures[] = {
		p_frame.color,
		p_frame.depth,
		p_frame.motion,
		p_frame.normal,
		p_frame.diffuse,
		p_frame.specular,
		p_frame.roughness,
		p_frame.denoise_strength,
		p_frame.reactive,
		p_frame.specular_distance,
		p_frame.transparency,
		p_frame.output,
	};
	for (const RID &texture : textures) {
		if (!texture.is_valid()) {
			return _reconstruction_fail(ERR_INVALID_PARAMETER, "Hybrid reconstruction requires the complete per-view guide set and output.", r_error);
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

} // namespace RendererPathTracing
