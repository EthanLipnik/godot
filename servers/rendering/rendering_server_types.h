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
	uint64_t frame = 0;
	int32_t effective_mode = 0;
	bool ray_effects_active = false;
	bool environment_active = false;
	String environment_status = "disabled";
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

	bool material_tier2 = false;
	uint32_t material_capacity = 0;
	FluxTextureDiagnostics material_albedo;
	FluxTextureDiagnostics material_normal;
	FluxTextureDiagnostics material_orm;
	FluxTextureDiagnostics material_emissive;
	FluxTextureDiagnostics material_opacity;
	FluxTextureDiagnostics material_alpha_occupancy;

	bool timings_valid = false;
	FluxStageTimings timings_ms;

	void reset_for_frame(uint64_t p_frame, int32_t p_effective_mode) {
		*this = FluxDiagnostics();
		valid = true;
		frame = p_frame;
		effective_mode = p_effective_mode;
	}

	void set_transport_counts(uint32_t p_primary_geometry_count, uint32_t p_selected_geometry_count, uint32_t p_eligible_geometry_count) {
		transport_primary_geometry_count = p_primary_geometry_count;
		transport_selected_geometry_count = p_selected_geometry_count;
		transport_eligible_geometry_count = p_eligible_geometry_count;
		transport_retained_non_primary_geometry_count = p_selected_geometry_count > p_primary_geometry_count ? p_selected_geometry_count - p_primary_geometry_count : 0;
		transport_retains_non_primary_geometry = transport_retained_non_primary_geometry_count > 0;
	}

	static Dictionary _texture_dictionary(const FluxTextureDiagnostics &p_texture) {
		Dictionary texture;
		texture["requested"] = p_texture.requested;
		texture["resident"] = p_texture.resident;
		texture["misses"] = p_texture.misses;
		return texture;
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

		Dictionary materials;
		materials["tier2"] = material_tier2;
		materials["capacity"] = material_capacity;
		materials["albedo"] = _texture_dictionary(material_albedo);
		materials["normal"] = _texture_dictionary(material_normal);
		materials["orm"] = _texture_dictionary(material_orm);
		materials["emissive"] = _texture_dictionary(material_emissive);
		materials["opacity"] = _texture_dictionary(material_opacity);
		materials["alpha_occupancy"] = _texture_dictionary(material_alpha_occupancy);

		Dictionary timings;
		timings["blas"] = timings_ms.blas;
		timings["tlas"] = timings_ms.tlas;
		timings["ray_shadows"] = timings_ms.ray_shadows;
		timings["ray_effects"] = timings_ms.ray_effects;
		timings["spatial"] = timings_ms.spatial;
		timings["temporal"] = timings_ms.temporal;
		timings["composition"] = timings_ms.composition;

		Dictionary diagnostics;
		diagnostics["valid"] = valid;
		diagnostics["frame"] = int64_t(frame);
		diagnostics["effective_mode"] = effective_mode;
		diagnostics["ray_effects_active"] = ray_effects_active;
		diagnostics["environment_active"] = environment_active;
		diagnostics["environment_status"] = environment_status;
		diagnostics["primary_surface_version"] = primary_surface_version;
		diagnostics["ray_owned_shading"] = ray_owned_shading;
		diagnostics["primary_surface_view_count"] = primary_surface_view_count;
		diagnostics["primary_unsupported_surface_count"] = primary_unsupported_surface_count;
		diagnostics["transport_complete"] = transport_complete;
		diagnostics["transport_incomplete_reason"] = transport_incomplete_reason;
		diagnostics["invalid_pdf_sample_count"] = invalid_pdf_sample_count;
		diagnostics["nonfinite_lobe_sample_count"] = nonfinite_lobe_sample_count;
		diagnostics["rejected_energy_sample_count"] = rejected_energy_sample_count;
		diagnostics["primary_valid_pixel_count"] = primary_valid_pixel_count;
		diagnostics["primary_invalid_pixel_count"] = primary_invalid_pixel_count;
		diagnostics["primary_lit_pixel_count"] = primary_lit_pixel_count;
		diagnostics["admitted"] = admitted;
		diagnostics["transport"] = transport;
		diagnostics["materials"] = materials;
		diagnostics["timings_valid"] = timings_valid;
		diagnostics["timings_ms"] = timings;
		return diagnostics;
	}
};

struct RenderInfo {
	int info[RSE::VIEWPORT_RENDER_INFO_TYPE_MAX][RSE::VIEWPORT_RENDER_INFO_MAX] = {};
	uint64_t flux_owner_id = 0;
	uint64_t flux_current_frame = 0;
	int32_t flux_last_effective_mode = -1;
	FluxDiagnostics flux;
};

} // namespace RenderingServerTypes
