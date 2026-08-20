/**************************************************************************/
/*  path_tracing_scene_packet.h                                           */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

namespace RendererPathTracing {

static constexpr uint32_t SCENE_PACKET_MAGIC = 0x50545053; // "SPTP" in little-endian storage.
static constexpr uint32_t SCENE_PACKET_VERSION = 1;
static constexpr uint32_t SCENE_PACKET_ENDIAN_TAG = 0x01020304;
static constexpr uint32_t SCENE_PACKET_MAX_VIEWS = 2;

enum MotionDirection : uint32_t {
	MOTION_CURRENT_TO_PREVIOUS = 1,
};

enum MotionUnits : uint32_t {
	MOTION_NORMALIZED_UV = 1,
};

enum DepthConvention : uint32_t {
	DEPTH_REVERSED_ZERO_TO_ONE = 1,
};

enum NormalSpace : uint32_t {
	NORMAL_WORLD_SPACE = 1,
};

enum RoughnessConvention : uint32_t {
	ROUGHNESS_PERCEPTUAL_ZERO_TO_ONE = 1,
};

enum InvalidPixelConvention : uint32_t {
	INVALID_PIXEL_QUIET_NAN = 1,
};

enum UVOrigin : uint32_t {
	UV_ORIGIN_TOP_LEFT = 1,
};

enum ColorSpace : uint32_t {
	COLOR_LINEAR_REC709 = 1,
};

enum GuideFlags : uint32_t {
	GUIDE_DEPTH = 1 << 0,
	GUIDE_MOTION = 1 << 1,
	GUIDE_NORMAL = 1 << 2,
	GUIDE_DIFFUSE_ALBEDO = 1 << 3,
	GUIDE_SPECULAR_ALBEDO = 1 << 4,
	GUIDE_ROUGHNESS = 1 << 5,
	GUIDE_DENOISE_STRENGTH = 1 << 6,
	GUIDE_REACTIVE_MASK = 1 << 7,
	GUIDE_SPECULAR_HIT_DISTANCE = 1 << 8,
	GUIDE_TRANSPARENCY_OVERLAY = 1 << 9,
};

enum RayVisibility : uint32_t {
	RAY_VISIBILITY_PRIMARY = 1 << 0,
	RAY_VISIBILITY_SHADOW = 1 << 1,
	RAY_VISIBILITY_REFLECTION_REFRACTION = 1 << 2,
	RAY_VISIBILITY_DIFFUSE_INDIRECT = 1 << 3,
	RAY_VISIBILITY_EDITOR_DEBUG = 1 << 4,
};

enum LightType : uint32_t {
	LIGHT_DIRECTIONAL = 0,
	LIGHT_POINT = 1,
	LIGHT_SPOT = 2,
	LIGHT_RECT = 3,
	LIGHT_ENVIRONMENT = 4,
};

struct alignas(16) Float4 {
	float x;
	float y;
	float z;
	float w;
};

// Column-major, right-handed, +Y up, -Z camera forward.
struct alignas(16) Matrix4 {
	Float4 columns[4];
};

struct alignas(16) GuideContract {
	uint32_t schema_version;
	uint32_t motion_direction;
	uint32_t motion_units;
	uint32_t depth_convention;
	uint32_t normal_space;
	uint32_t roughness_convention;
	uint32_t invalid_pixel_convention;
	uint32_t uv_origin;
	uint32_t color_space;
	uint32_t enabled_guides;
	float motion_to_pixel_scale_x;
	float motion_to_pixel_scale_y;
	uint32_t reserved[4];
};

struct alignas(16) CameraRecord {
	Matrix4 view_from_world;
	Matrix4 clip_from_view;
	Matrix4 previous_view_from_world;
	Matrix4 previous_clip_from_view;
	Float4 camera_relative_origin_and_exposure;
	uint32_t view_index;
	uint32_t render_width;
	uint32_t render_height;
	uint32_t history_reset;
};

struct alignas(16) InstanceRecord {
	Matrix4 world_from_object;
	Matrix4 previous_world_from_object;
	uint64_t geometry_id;
	// One-based index into the material table. Zero means no material.
	uint64_t material_id;
	uint32_t instance_id;
	uint32_t visibility_mask;
	uint32_t flags;
	uint32_t reserved;
};

struct alignas(16) MaterialRecord {
	Float4 base_color_and_opacity;
	Float4 emission_and_strength;
	Float4 specular_f0_and_perceptual_roughness;
	Float4 transmission_ior_alpha_cutoff_unused;
	uint32_t base_color_texture;
	uint32_t normal_texture;
	uint32_t metallic_roughness_texture;
	uint32_t emission_texture;
};

struct alignas(16) LightRecord {
	Float4 position_or_direction_and_type;
	Float4 linear_color_and_intensity;
	Float4 shape_parameters;
	uint32_t light_id;
	uint32_t visibility_mask;
	uint32_t flags;
	uint32_t reserved;
};

struct alignas(16) ScenePacketHeader {
	uint32_t magic;
	uint32_t schema_version;
	uint32_t endian_tag;
	uint32_t header_size;
	uint64_t total_size;
	uint64_t payload_hash;
	uint32_t guide_contract_offset;
	uint32_t camera_count;
	uint32_t camera_offset;
	uint32_t instance_count;
	uint32_t instance_offset;
	uint32_t material_count;
	uint32_t material_offset;
	uint32_t light_count;
	uint32_t light_offset;
	uint32_t reserved[3];
};

static_assert(sizeof(Float4) == 16);
static_assert(sizeof(Matrix4) == 64);
static_assert(sizeof(GuideContract) == 64);
static_assert(sizeof(CameraRecord) == 288);
static_assert(sizeof(InstanceRecord) == 160);
static_assert(sizeof(MaterialRecord) == 80);
static_assert(sizeof(LightRecord) == 64);
static_assert(sizeof(ScenePacketHeader) == 80);
static_assert(offsetof(CameraRecord, view_index) == 272);
static_assert(offsetof(InstanceRecord, geometry_id) == 128);

} // namespace RendererPathTracing
