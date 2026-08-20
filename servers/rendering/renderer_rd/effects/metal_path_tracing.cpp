/**************************************************************************/
/*  metal_path_tracing.cpp                                                */
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

#ifdef METAL_ENABLED

#include "metal_path_tracing.h"

#include "core/math/projection.h"
#include "core/templates/hash_map.h"
#include "drivers/metal/metal3_objects.h"
#include "drivers/metal/rendering_device_driver_metal.h"
#include "servers/rendering/renderer_rd/effects/metal_fx.h"
#include "servers/rendering/rendering_device.h"

#include <Metal/Metal.hpp>

using namespace RendererPathTracing;

namespace RendererRD {

enum MetalBLASAction : uint8_t {
	METAL_BLAS_REUSE,
	METAL_BLAS_BUILD,
	METAL_BLAS_REFIT,
};

struct MetalCachedGeometry {
	uint64_t topology_hash = 0;
	uint64_t content_hash = 0;
	uint64_t last_seen_frame = 0;
	NS::SharedPtr<MTL::AccelerationStructure> acceleration_structure;
};

struct MetalPathTracingCache {
	HashMap<uint64_t, MetalCachedGeometry *> geometries;
	NS::SharedPtr<MTL::ComputePipelineState> pipeline;
	uint64_t frame = 0;

	~MetalPathTracingCache() {
		for (const KeyValue<uint64_t, MetalCachedGeometry *> &entry : geometries) {
			memdelete(entry.value);
		}
	}
};

struct alignas(16) MetalFrameParameters {
	uint32_t dimensions_seed_view[4];
	uint32_t counts_and_guides[4];
	Matrix4 world_from_view;
	Matrix4 view_from_clip;
};

static Matrix4 _inverse_matrix(const Matrix4 &p_matrix) {
	Projection projection;
	for (uint32_t column = 0; column < 4; column++) {
		projection.columns[column] = Vector4(p_matrix.columns[column].x, p_matrix.columns[column].y, p_matrix.columns[column].z, p_matrix.columns[column].w);
	}
	projection = projection.inverse();
	Matrix4 inverse = {};
	for (uint32_t column = 0; column < 4; column++) {
		inverse.columns[column] = { (float)projection.columns[column].x, (float)projection.columns[column].y, (float)projection.columns[column].z, (float)projection.columns[column].w };
	}
	return inverse;
}

static Error _metal_path_tracing_fail(Error p_error, const String &p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static Error _resolve_output_texture(RenderingDevice *p_rd, const RID &p_rid, uint32_t p_width, uint32_t p_height, MTL::PixelFormat p_format, const char *p_name, MTL::Texture *&r_texture, String *r_error) {
	r_texture = reinterpret_cast<MTL::Texture *>(p_rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_rid));
	if (!r_texture || r_texture->width() != p_width || r_texture->height() != p_height || r_texture->pixelFormat() != p_format) {
		return _metal_path_tracing_fail(ERR_INVALID_PARAMETER, vformat("The Metal path-tracing %s target must be %dx%d with the required guide format.", p_name, p_width, p_height), r_error);
	}
	return OK;
}

static constexpr const char *PATH_TRACING_MSL = R"(
#include <metal_raytracing>
#include <metal_stdlib>
using namespace metal;

struct Float4 { float x, y, z, w; };
struct Matrix4 { Float4 columns[4]; };
struct CameraRecord {
    Matrix4 view_from_world;
    Matrix4 clip_from_view;
    Matrix4 previous_view_from_world;
    Matrix4 previous_clip_from_view;
    Float4 camera_relative_origin_and_exposure;
    uint view_index, render_width, render_height, history_reset;
};
struct InstanceRecord {
    Matrix4 world_from_object;
    Matrix4 previous_world_from_object;
    ulong geometry_id, material_id;
    uint instance_id, visibility_mask, flags, reserved;
};
struct MaterialRecord {
    Float4 base_color_and_opacity;
    Float4 emission_and_strength;
    Float4 specular_f0_and_perceptual_roughness;
    Float4 transmission_ior_alpha_cutoff_unused;
    uint base_color_texture, normal_texture, metallic_roughness_texture, emission_texture;
};
struct LightRecord {
    Float4 position_or_direction_and_type;
    Float4 linear_color_and_intensity;
    Float4 shape_parameters;
    uint light_id, visibility_mask, flags, reserved;
};
struct GeometryVertex {
    Float4 current_position;
    Float4 previous_position;
    Float4 normal;
    Float4 tangent;
    Float4 uv;
};
struct GeometryRecord {
    ulong geometry_id;
    uint vertex_offset, vertex_count, index_offset, index_count, flags, reserved;
    Float4 bounds_min, bounds_max;
};
struct FrameParameters {
    uint4 dimensions_seed_view;
    uint4 counts_and_guides;
    Matrix4 world_from_view;
    Matrix4 view_from_clip;
};

static float4x4 matrix_from_record(Matrix4 value) {
    return float4x4(float4(value.columns[0].x, value.columns[0].y, value.columns[0].z, value.columns[0].w),
                    float4(value.columns[1].x, value.columns[1].y, value.columns[1].z, value.columns[1].w),
                    float4(value.columns[2].x, value.columns[2].y, value.columns[2].z, value.columns[2].w),
                    float4(value.columns[3].x, value.columns[3].y, value.columns[3].z, value.columns[3].w));
}
static uint hash_uint(uint value) {
    value ^= value >> 16; value *= 0x7feb352d; value ^= value >> 15; value *= 0x846ca68b; return value ^ (value >> 16);
}
static float random_float(thread uint &state) {
    state = hash_uint(state); return float(state & 0x00ffffffu) / float(0x01000000u);
}
static float3 cosine_direction(float3 normal, thread uint &state) {
    float u1 = random_float(state), u2 = random_float(state), radius = sqrt(u1), angle = 6.28318530718f * u2;
    float3 helper = abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(helper, normal));
    return normalize(tangent * (radius * cos(angle)) + cross(normal, tangent) * (radius * sin(angle)) + normal * sqrt(max(0.0f, 1.0f - u1)));
}

kernel void trace_capture(
    raytracing::instance_acceleration_structure scene [[buffer(0)]],
    device const GeometryVertex *vertices [[buffer(1)]],
    device const uint *indices [[buffer(2)]],
    device const GeometryRecord *geometries [[buffer(3)]],
    device const InstanceRecord *instances [[buffer(4)]],
    device const uint *instance_geometry [[buffer(5)]],
    device const MaterialRecord *materials [[buffer(6)]],
    device const CameraRecord *cameras [[buffer(7)]],
    constant FrameParameters &parameters [[buffer(8)]],
    device const uint *instance_material [[buffer(9)]],
    device const LightRecord *lights [[buffer(10)]],
    texture2d<float, access::read_write> output [[texture(0)]],
    texture2d<float, access::write> output_depth [[texture(1)]],
    texture2d<float, access::write> output_motion [[texture(2)]],
    texture2d<float, access::write> output_normal [[texture(3)]],
    texture2d<float, access::write> output_diffuse [[texture(4)]],
    texture2d<float, access::write> output_specular [[texture(5)]],
    texture2d<float, access::write> output_roughness [[texture(6)]],
    texture2d<float, access::write> output_denoise_strength [[texture(7)]],
    texture2d<float, access::write> output_reactive [[texture(8)]],
    texture2d<float, access::write> output_specular_distance [[texture(9)]],
    texture2d<float, access::write> output_transparency [[texture(10)]],
    uint2 pixel [[thread_position_in_grid]]) {
    uint width = parameters.dimensions_seed_view.x, height = parameters.dimensions_seed_view.y;
    uint seed = parameters.dimensions_seed_view.z, view = parameters.dimensions_seed_view.w;
    if (pixel.x >= width || pixel.y >= height) return;
    const CameraRecord camera = cameras[view];
    float4x4 world_from_view = matrix_from_record(parameters.world_from_view);
    float4x4 view_from_clip = matrix_from_record(parameters.view_from_clip);
    float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 view_far_h = view_from_clip * float4(ndc, 0.0f, 1.0f);
    float3 view_direction = normalize(view_far_h.xyz / view_far_h.w);
    float3 ray_origin = world_from_view[3].xyz;
    float3 ray_direction = normalize((world_from_view * float4(view_direction, 0.0f)).xyz);
    float3 radiance = 0.0f, throughput = 1.0f;
	const float invalid_guide = as_type<float>(0x7fc00000u);
	float primary_depth = invalid_guide;
	float2 primary_motion = 0.0f;
	float3 primary_normal = float3(invalid_guide);
	float3 primary_diffuse = 0.0f;
	float3 primary_specular = 0.0f;
	float primary_roughness = invalid_guide;
	float primary_specular_distance = invalid_guide;
	bool primary_specular_ray = false;
    uint random_state = hash_uint(seed ^ (pixel.y * width + pixel.x) ^ (view * 0x9e3779b9u));
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
	for (uint bounce = 0; bounce < parameters.counts_and_guides.z; bounce++) {
        raytracing::ray ray = { ray_origin, ray_direction, 0.001f, 100000.0f };
        auto hit = intersector.intersect(ray, scene, 0xff);
        if (hit.type != raytracing::intersection_type::triangle) {
            float sky = 0.5f * (ray_direction.y + 1.0f);
			float3 environment = mix(float3(0.01f, 0.015f, 0.025f), float3(0.15f, 0.22f, 0.35f), sky);
			for (uint light_index = 0; light_index < parameters.counts_and_guides.x; light_index++) {
				LightRecord light = lights[light_index];
				if (uint(light.position_or_direction_and_type.w) == 4u) {
					environment = float3(light.linear_color_and_intensity.x, light.linear_color_and_intensity.y, light.linear_color_and_intensity.z) * light.linear_color_and_intensity.w;
					break;
				}
			}
			radiance += throughput * environment;
            break;
        }
        uint instance_index = hit.instance_id;
        InstanceRecord instance = instances[instance_index];
        GeometryRecord geometry = geometries[instance_geometry[instance_index]];
        uint primitive_index = geometry.index_offset + hit.primitive_id * 3;
        uint i0 = geometry.vertex_offset + indices[primitive_index];
        uint i1 = geometry.vertex_offset + indices[primitive_index + 1];
        uint i2 = geometry.vertex_offset + indices[primitive_index + 2];
        float4x4 world_from_object = matrix_from_record(instance.world_from_object);
        float3 p0 = (world_from_object * float4(vertices[i0].current_position.x, vertices[i0].current_position.y, vertices[i0].current_position.z, 1)).xyz;
        float3 p1 = (world_from_object * float4(vertices[i1].current_position.x, vertices[i1].current_position.y, vertices[i1].current_position.z, 1)).xyz;
        float3 p2 = (world_from_object * float4(vertices[i2].current_position.x, vertices[i2].current_position.y, vertices[i2].current_position.z, 1)).xyz;
        float3 normal = normalize(cross(p1 - p0, p2 - p0));
        if (dot(normal, -ray_direction) < 0.0f) normal = -normal;
        MaterialRecord material = materials[instance_material[instance_index]];
        float3 base_color = float3(material.base_color_and_opacity.x, material.base_color_and_opacity.y, material.base_color_and_opacity.z);
        float3 emission = float3(material.emission_and_strength.x, material.emission_and_strength.y, material.emission_and_strength.z) * material.emission_and_strength.w;
        radiance += throughput * emission;
        float3 hit_position = ray_origin + ray_direction * hit.distance;
        float roughness = material.specular_f0_and_perceptual_roughness.w;
		float metallic = material.transmission_ior_alpha_cutoff_unused.x;
		float3 f0 = float3(material.specular_f0_and_perceptual_roughness.x, material.specular_f0_and_perceptual_roughness.y, material.specular_f0_and_perceptual_roughness.z);
		if (bounce == 0) {
			float4 current_clip = matrix_from_record(camera.clip_from_view) * matrix_from_record(camera.view_from_world) * float4(hit_position, 1.0f);
			primary_depth = current_clip.z / current_clip.w;
			float2 barycentric = hit.triangle_barycentric_coord;
			float3 weights = float3(1.0f - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
			float3 previous_object =
				float3(vertices[i0].previous_position.x, vertices[i0].previous_position.y, vertices[i0].previous_position.z) * weights.x +
				float3(vertices[i1].previous_position.x, vertices[i1].previous_position.y, vertices[i1].previous_position.z) * weights.y +
				float3(vertices[i2].previous_position.x, vertices[i2].previous_position.y, vertices[i2].previous_position.z) * weights.z;
			float3 previous_world = (matrix_from_record(instance.previous_world_from_object) * float4(previous_object, 1.0f)).xyz;
			float4 previous_clip = matrix_from_record(camera.previous_clip_from_view) * matrix_from_record(camera.previous_view_from_world) * float4(previous_world, 1.0f);
			float2 current_ndc = current_clip.xy / current_clip.w;
			float2 previous_ndc = previous_clip.xy / previous_clip.w;
			float2 current_uv = float2(current_ndc.x * 0.5f + 0.5f, 0.5f - current_ndc.y * 0.5f);
			float2 previous_uv = float2(previous_ndc.x * 0.5f + 0.5f, 0.5f - previous_ndc.y * 0.5f);
			primary_motion = previous_uv - current_uv;
			primary_normal = normal;
			primary_diffuse = base_color * (1.0f - metallic);
			primary_specular = f0;
			primary_roughness = roughness;
			primary_specular_ray = roughness < 0.08f;
		} else if (primary_specular_ray) {
			primary_specular_distance = hit.distance;
		}
		for (uint light_index = 0; light_index < parameters.counts_and_guides.x; light_index++) {
			LightRecord light = lights[light_index];
			if (uint(light.position_or_direction_and_type.w) == 4u) continue;
			float3 to_light;
			float attenuation = 1.0f;
			float max_distance = 100000.0f;
			if (uint(light.position_or_direction_and_type.w) == 0u) {
				to_light = normalize(-float3(light.position_or_direction_and_type.x, light.position_or_direction_and_type.y, light.position_or_direction_and_type.z));
			} else {
				float3 delta = float3(light.position_or_direction_and_type.x, light.position_or_direction_and_type.y, light.position_or_direction_and_type.z) - hit_position;
				float distance_squared = max(dot(delta, delta), 0.0001f);
				max_distance = sqrt(distance_squared);
				to_light = delta / max_distance;
				attenuation = 1.0f / distance_squared;
			}
			float n_dot_l = max(dot(normal, to_light), 0.0f);
			if (n_dot_l > 0.0f) {
				raytracing::ray shadow_ray = { hit_position + normal * 0.002f, to_light, 0.001f, max_distance - 0.003f };
				auto blocker = intersector.intersect(shadow_ray, scene, 0x02);
				if (blocker.type == raytracing::intersection_type::none) {
					float3 light_value = float3(light.linear_color_and_intensity.x, light.linear_color_and_intensity.y, light.linear_color_and_intensity.z) * light.linear_color_and_intensity.w * attenuation;
					float3 half_direction = normalize(to_light - ray_direction);
					float gloss_exponent = mix(256.0f, 2.0f, roughness * roughness);
					float specular = pow(max(dot(normal, half_direction), 0.0f), gloss_exponent) * (gloss_exponent + 2.0f) * 0.15915494309f;
					float3 bounded_brdf = base_color * (1.0f - metallic) * 0.31830988618f + f0 * specular;
					radiance += throughput * bounded_brdf * light_value * n_dot_l;
				}
			}
		}
		float specular_probability = roughness < 0.08f ? 1.0f : mix(0.08f, 0.95f, metallic);
		if (random_float(random_state) < specular_probability) {
			throughput *= f0 / specular_probability;
			float3 reflected = normalize(reflect(ray_direction, normal));
			float3 rough_reflection = cosine_direction(reflected, random_state);
			ray_direction = normalize(mix(reflected, rough_reflection, roughness * roughness));
		} else {
			throughput *= base_color * (1.0f - metallic) / (1.0f - specular_probability);
			ray_direction = cosine_direction(normal, random_state);
		}
        ray_origin = hit_position + normal * 0.002f;
    }
	float4 resolved_radiance = float4(radiance * camera.camera_relative_origin_and_exposure.w, 1.0f);
	if (parameters.counts_and_guides.w != 0u && seed > 0u) {
		resolved_radiance = (output.read(pixel) * float(seed) + resolved_radiance) / float(seed + 1u);
	}
	output.write(resolved_radiance, pixel);
	uint guides = parameters.counts_and_guides.y;
	if (guides & 0x001u) output_depth.write(float4(primary_depth), pixel);
	if (guides & 0x002u) output_motion.write(float4(primary_motion, 0.0f, 0.0f), pixel);
	if (guides & 0x004u) output_normal.write(float4(primary_normal, 0.0f), pixel);
	if (guides & 0x008u) output_diffuse.write(float4(primary_diffuse, 1.0f), pixel);
	if (guides & 0x010u) output_specular.write(float4(primary_specular, 1.0f), pixel);
	if (guides & 0x020u) output_roughness.write(float4(primary_roughness), pixel);
	if (guides & 0x040u) output_denoise_strength.write(float4(0.0f), pixel);
	if (guides & 0x080u) output_reactive.write(float4(0.0f), pixel);
	if (guides & 0x100u) output_specular_distance.write(float4(primary_specular_distance), pixel);
	if (guides & 0x200u) output_transparency.write(float4(0.0f), pixel);
}
)";

struct MetalPathTracingWork {
	NS::SharedPtr<MTL::ComputePipelineState> pipeline;
	NS::SharedPtr<MTL::Buffer> vertex_buffer;
	NS::SharedPtr<MTL::Buffer> index_buffer;
	NS::SharedPtr<MTL::Buffer> geometry_buffer;
	NS::SharedPtr<MTL::Buffer> instance_buffer;
	NS::SharedPtr<MTL::Buffer> instance_geometry_buffer;
	NS::SharedPtr<MTL::Buffer> instance_material_buffer;
	NS::SharedPtr<MTL::Buffer> material_buffer;
	NS::SharedPtr<MTL::Buffer> light_buffer;
	NS::SharedPtr<MTL::Buffer> camera_buffer;
	Vector<NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor>> blas_descriptors;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas;
	Vector<NS::SharedPtr<MTL::Buffer>> blas_scratch;
	Vector<uint8_t> blas_actions;
	NS::SharedPtr<NS::Array> blas_array;
	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;
	NS::SharedPtr<MTL::Buffer> tlas_scratch;
	NS::SharedPtr<MTL::Buffer> tlas_instances;
	Vector<MTL::Texture *> outputs;
	Vector<MTL::Texture *> guide_outputs[10];
	Vector<MetalFrameParameters> parameters;
	uint32_t pending_callbacks = 3;
};

static void _release_work(MetalPathTracingWork *p_work) {
	p_work->pending_callbacks--;
	if (p_work->pending_callbacks == 0) {
		memdelete(p_work);
	}
}

static void _retain_trace_resources(MDCommandBufferBase *p_command_buffer, MetalPathTracingWork *p_work) {
	p_command_buffer->retain_resource(p_work->pipeline.get());
	p_command_buffer->retain_resource(p_work->vertex_buffer.get());
	p_command_buffer->retain_resource(p_work->index_buffer.get());
	p_command_buffer->retain_resource(p_work->geometry_buffer.get());
	p_command_buffer->retain_resource(p_work->instance_buffer.get());
	p_command_buffer->retain_resource(p_work->instance_geometry_buffer.get());
	p_command_buffer->retain_resource(p_work->instance_material_buffer.get());
	p_command_buffer->retain_resource(p_work->material_buffer.get());
	p_command_buffer->retain_resource(p_work->light_buffer.get());
	p_command_buffer->retain_resource(p_work->camera_buffer.get());
	p_command_buffer->retain_resource(p_work->tlas.get());
	p_command_buffer->retain_resource(p_work->tlas_scratch.get());
	p_command_buffer->retain_resource(p_work->tlas_instances.get());
	p_command_buffer->retain_resource(p_work->tlas_descriptor.get());
	p_command_buffer->retain_resource(p_work->blas_array.get());
	for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : p_work->blas) {
		p_command_buffer->retain_resource(blas.get());
	}
	for (const NS::SharedPtr<MTL::Buffer> &scratch : p_work->blas_scratch) {
		if (scratch) {
			p_command_buffer->retain_resource(scratch.get());
		}
	}
	for (const NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> &descriptor : p_work->blas_descriptors) {
		p_command_buffer->retain_resource(descriptor.get());
	}
}

static void _build_blas(RDD *p_driver, RDD::CommandBufferID p_command_buffer, MetalPathTracingWork *p_work) {
	(void)p_driver;
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	MTL::AccelerationStructureCommandEncoder *encoder = metal_command->get_command_buffer()->accelerationStructureCommandEncoder();
	for (uint32_t i = 0; i < p_work->blas.size(); i++) {
		if (p_work->blas_actions[i] == METAL_BLAS_BUILD) {
			encoder->buildAccelerationStructure(p_work->blas[i].get(), p_work->blas_descriptors[i].get(), p_work->blas_scratch[i].get(), 0);
		} else if (p_work->blas_actions[i] == METAL_BLAS_REFIT) {
			encoder->refitAccelerationStructure(p_work->blas[i].get(), p_work->blas_descriptors[i].get(), p_work->blas[i].get(), p_work->blas_scratch[i].get(), 0);
		}
	}
	encoder->endEncoding();
	_release_work(p_work);
}

static void _build_tlas(RDD *p_driver, RDD::CommandBufferID p_command_buffer, MetalPathTracingWork *p_work) {
	(void)p_driver;
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	MTL::AccelerationStructureCommandEncoder *encoder = metal_command->get_command_buffer()->accelerationStructureCommandEncoder();
	encoder->buildAccelerationStructure(p_work->tlas.get(), p_work->tlas_descriptor.get(), p_work->tlas_scratch.get(), 0);
	encoder->endEncoding();
	_release_work(p_work);
}

static void _trace(RDD *p_driver, RDD::CommandBufferID p_command_buffer, MetalPathTracingWork *p_work) {
	(void)p_driver;
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	MTL::ComputeCommandEncoder *encoder = metal_command->get_command_buffer()->computeCommandEncoder();
	encoder->setComputePipelineState(p_work->pipeline.get());
	encoder->setAccelerationStructure(p_work->tlas.get(), 0);
	encoder->setBuffer(p_work->vertex_buffer.get(), 0, 1);
	encoder->setBuffer(p_work->index_buffer.get(), 0, 2);
	encoder->setBuffer(p_work->geometry_buffer.get(), 0, 3);
	encoder->setBuffer(p_work->instance_buffer.get(), 0, 4);
	encoder->setBuffer(p_work->instance_geometry_buffer.get(), 0, 5);
	encoder->setBuffer(p_work->material_buffer.get(), 0, 6);
	encoder->setBuffer(p_work->camera_buffer.get(), 0, 7);
	encoder->setBuffer(p_work->instance_material_buffer.get(), 0, 9);
	encoder->setBuffer(p_work->light_buffer.get(), 0, 10);
	for (uint32_t view = 0; view < p_work->outputs.size(); view++) {
		encoder->setBytes(&p_work->parameters[view], sizeof(MetalFrameParameters), 8);
		encoder->setTexture(p_work->outputs[view], 0);
		for (uint32_t guide = 0; guide < 10; guide++) {
			encoder->setTexture(p_work->guide_outputs[guide].is_empty() ? nullptr : p_work->guide_outputs[guide][view], guide + 1);
		}
		encoder->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions_seed_view[0], p_work->parameters[view].dimensions_seed_view[1], 1), MTL::Size(8, 8, 1));
	}
	encoder->endEncoding();
	_retain_trace_resources(command, p_work);
	_release_work(p_work);
}

StringName MetalPathTracingBackend::get_name() const {
	return "metal_reference";
}

MetalPathTracingBackend::MetalPathTracingBackend() {
	cache = memnew(MetalPathTracingCache);
}

MetalPathTracingBackend::~MetalPathTracingBackend() {
	memdelete(cache);
}

BackendCapabilities MetalPathTracingBackend::get_capabilities() const {
	BackendCapabilities capabilities;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		capabilities.unavailable_reason = "RenderingDevice is not initialized.";
		return capabilities;
	}
	if (rd->get_device_api_name() != "Metal") {
		capabilities.unavailable_reason = "The active RenderingDevice is not a ray-tracing-capable Metal device.";
		return capabilities;
	}
	RenderingDeviceDriverMetal *driver = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	if (!driver->get_device()->supportsRaytracing()) {
		capabilities.unavailable_reason = "The active Metal device does not support ray tracing.";
		return capabilities;
	}
	capabilities.available = true;
	capabilities.acceleration_structures = true;
	capabilities.dynamic_blas_update = true;
	capabilities.temporal_reconstruction = MFXDenoisedEffect().is_supported();
	capabilities.hardware_ray_query = true;
	capabilities.max_views = SCENE_PACKET_MAX_VIEWS;
	capabilities.supported_guides = GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO |
			GUIDE_SPECULAR_ALBEDO | GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH | GUIDE_REACTIVE_MASK |
			GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY;
	return capabilities;
}

Error MetalPathTracingBackend::collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error) const {
	r_timings.clear();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || rd->get_device_api_name() != "Metal") {
		return _metal_path_tracing_fail(ERR_UNAVAILABLE, "A Metal RenderingDevice is required to collect path-tracing timings.", r_error);
	}
	struct TimingPair {
		const char *stage;
		const char *begin;
		const char *end;
	};
	const TimingPair pairs[] = {
		{ "blas", "Path Tracing BLAS Begin", "Path Tracing BLAS End" },
		{ "tlas", "Path Tracing TLAS Begin", "Path Tracing TLAS End" },
		{ "trace", "Path Tracing Trace Begin", "Path Tracing Trace End" },
		{ "reconstruct", "Path Tracing Reconstruct Begin", "Path Tracing Reconstruct End" },
	};
	for (const TimingPair &pair : pairs) {
		uint64_t begin_time = 0;
		uint64_t end_time = 0;
		bool found_begin = false;
		bool found_end = false;
		for (uint32_t index = 0; index < rd->get_captured_timestamps_count(); index++) {
			const String name = rd->get_captured_timestamp_name(index);
			if (name == pair.begin) {
				begin_time = rd->get_captured_timestamp_gpu_time(index);
				found_begin = true;
			} else if (name == pair.end) {
				end_time = rd->get_captured_timestamp_gpu_time(index);
				found_end = true;
			}
		}
		if (found_begin && found_end && end_time > begin_time) {
			StageTiming timing;
			timing.stage = pair.stage;
			timing.milliseconds = double(end_time - begin_time) / 1000000.0;
			r_timings.push_back(timing);
		}
	}
	if (r_timings.size() < 3) {
		r_timings.clear();
		return _metal_path_tracing_fail(ERR_UNAVAILABLE, "Godot's Metal RenderingDevice timestamp queries do not currently return GPU times; use the standalone Metal counter harness for measured stage evidence.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error MetalPathTracingBackend::render(const FrameRequest &p_request, FrameResult &r_result, String *r_error) {
	const Error request_error = validate_request(p_request, r_error);
	if (request_error != OK) {
		return request_error;
	}
	if (p_request.mode != RenderMode::INTERACTIVE && p_request.mode != RenderMode::PROGRESSIVE_REFERENCE) {
		return _metal_path_tracing_fail(ERR_UNAVAILABLE, "The initial Metal capture backend supports only full interactive and progressive-reference modes.", r_error);
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RenderingDeviceDriverMetal *driver = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	MTL::Device *device = driver->get_device();

	SceneCaptureHeader capture_header = {};
	SceneCompiler::read_record(p_request.capture, 0, capture_header);
	ScenePacketHeader packet_header = {};
	SceneCompiler::read_record(p_request.capture, capture_header.scene_packet_offset, packet_header);
	const uint8_t *capture = p_request.capture.ptr();
	const uint8_t *packet = capture + capture_header.scene_packet_offset;

	if (!cache->pipeline) {
		NS::Error *compile_error = nullptr;
		NS::SharedPtr<MTL::CompileOptions> compile_options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
		NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(NS::String::string(PATH_TRACING_MSL, NS::UTF8StringEncoding), compile_options.get(), &compile_error));
		if (!library) {
			if (r_error) {
				*r_error = compile_error ? String::utf8(compile_error->localizedDescription()->utf8String()) : "Metal path-tracing shader compilation failed.";
			}
			return ERR_CANT_CREATE;
		}
		NS::SharedPtr<MTL::Function> function = NS::TransferPtr(library->newFunction(NS::String::string("trace_capture", NS::UTF8StringEncoding)));
		cache->pipeline = NS::TransferPtr(device->newComputePipelineState(function.get(), &compile_error));
		if (!cache->pipeline) {
			if (r_error) {
				*r_error = compile_error ? String::utf8(compile_error->localizedDescription()->utf8String()) : "Metal path-tracing pipeline creation failed.";
			}
			return ERR_CANT_CREATE;
		}
	}

	MetalPathTracingWork *work = memnew(MetalPathTracingWork);
	work->pipeline = cache->pipeline;
	cache->frame++;
	work->vertex_buffer = NS::TransferPtr(device->newBuffer(capture + capture_header.vertex_offset, capture_header.vertex_count * sizeof(GeometryVertex), MTL::ResourceStorageModeShared));
	work->index_buffer = NS::TransferPtr(device->newBuffer(capture + capture_header.index_offset, capture_header.index_count * sizeof(uint32_t), MTL::ResourceStorageModeShared));
	work->geometry_buffer = NS::TransferPtr(device->newBuffer(capture + capture_header.geometry_offset, capture_header.geometry_count * sizeof(GeometryRecord), MTL::ResourceStorageModeShared));
	work->instance_buffer = NS::TransferPtr(device->newBuffer(packet + packet_header.instance_offset, packet_header.instance_count * sizeof(InstanceRecord), MTL::ResourceStorageModeShared));
	work->camera_buffer = NS::TransferPtr(device->newBuffer(packet + packet_header.camera_offset, packet_header.camera_count * sizeof(CameraRecord), MTL::ResourceStorageModeShared));
	Vector<MaterialRecord> gpu_materials;
	gpu_materials.resize(packet_header.material_count + 1);
	if (packet_header.material_count > 0) {
		memcpy(gpu_materials.ptrw(), packet + packet_header.material_offset, packet_header.material_count * sizeof(MaterialRecord));
	}
	MaterialRecord &fallback_material = gpu_materials.write[packet_header.material_count];
	fallback_material.base_color_and_opacity = { 1.0f, 0.0f, 1.0f, 1.0f };
	fallback_material.specular_f0_and_perceptual_roughness = { 0.04f, 0.04f, 0.04f, 0.5f };
	fallback_material.transmission_ior_alpha_cutoff_unused.y = 1.5f;
	work->material_buffer = NS::TransferPtr(device->newBuffer(gpu_materials.ptr(), gpu_materials.size() * sizeof(MaterialRecord), MTL::ResourceStorageModeShared));
	LightRecord no_light = {};
	work->light_buffer = packet_header.light_count > 0 ? NS::TransferPtr(device->newBuffer(packet + packet_header.light_offset, packet_header.light_count * sizeof(LightRecord), MTL::ResourceStorageModeShared)) : NS::TransferPtr(device->newBuffer(&no_light, sizeof(no_light), MTL::ResourceStorageModeShared));

	HashMap<uint64_t, uint32_t> geometry_indices;
	Vector<uint32_t> instance_geometry;
	Vector<uint32_t> instance_material;
	instance_geometry.resize(packet_header.instance_count);
	instance_material.resize(packet_header.instance_count);
	for (uint32_t i = 0; i < capture_header.geometry_count; i++) {
		GeometryRecord geometry = {};
		memcpy(&geometry, capture + capture_header.geometry_offset + i * sizeof(GeometryRecord), sizeof(geometry));
		geometry_indices.insert(geometry.geometry_id, i);
		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> triangle = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		triangle->setVertexBuffer(work->vertex_buffer.get());
		triangle->setVertexBufferOffset(geometry.vertex_offset * sizeof(GeometryVertex));
		triangle->setVertexFormat(MTL::AttributeFormatFloat3);
		triangle->setVertexStride(sizeof(GeometryVertex));
		triangle->setIndexBuffer(work->index_buffer.get());
		triangle->setIndexBufferOffset(geometry.index_offset * sizeof(uint32_t));
		triangle->setIndexType(MTL::IndexTypeUInt32);
		triangle->setTriangleCount(geometry.index_count / 3);
		triangle->setOpaque((geometry.flags & GEOMETRY_OPAQUE) != 0);
		NS::Object *triangle_object = triangle.get();
		NS::SharedPtr<NS::Array> triangle_array = NS::TransferPtr(NS::Array::array(&triangle_object, 1)->retain());
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
		descriptor->setGeometryDescriptors(triangle_array.get());
		descriptor->setUsage((geometry.flags & GEOMETRY_DYNAMIC) ? MTL::AccelerationStructureUsageRefit : MTL::AccelerationStructureUsagePreferFastIntersection);
		const MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(descriptor.get());
		const uint64_t topology_hash = SceneCompiler::hash_payload(capture + capture_header.index_offset + geometry.index_offset * sizeof(uint32_t), geometry.index_count * sizeof(uint32_t)) ^
				(uint64_t(geometry.vertex_count) << 32) ^ geometry.index_count;
		const uint64_t content_hash = SceneCompiler::hash_payload(capture + capture_header.vertex_offset + geometry.vertex_offset * sizeof(GeometryVertex), geometry.vertex_count * sizeof(GeometryVertex));
		MetalCachedGeometry **cached_ptr = cache->geometries.getptr(geometry.geometry_id);
		MetalCachedGeometry *cached = cached_ptr ? *cached_ptr : nullptr;
		MetalBLASAction action = METAL_BLAS_BUILD;
		if (!cached) {
			cached = memnew(MetalCachedGeometry);
			cache->geometries.insert(geometry.geometry_id, cached);
		} else if (cached->topology_hash == topology_hash && cached->acceleration_structure) {
			if (cached->content_hash == content_hash) {
				action = METAL_BLAS_REUSE;
			} else if (geometry.flags & GEOMETRY_DYNAMIC) {
				action = METAL_BLAS_REFIT;
			}
		}
		if (action == METAL_BLAS_BUILD) {
			cached->acceleration_structure = NS::TransferPtr(device->newAccelerationStructure(sizes.accelerationStructureSize));
			r_result.blas_rebuilt++;
		} else if (action == METAL_BLAS_REFIT) {
			r_result.blas_refit++;
		} else {
			r_result.blas_reused++;
		}
		cached->topology_hash = topology_hash;
		cached->content_hash = content_hash;
		cached->last_seen_frame = cache->frame;
		work->blas_descriptors.push_back(descriptor);
		work->blas.push_back(cached->acceleration_structure);
		work->blas_actions.push_back(action);
		const uint64_t scratch_size = action == METAL_BLAS_REFIT ? sizes.refitScratchBufferSize : sizes.buildScratchBufferSize;
		work->blas_scratch.push_back(action == METAL_BLAS_REUSE ? NS::SharedPtr<MTL::Buffer>() : NS::TransferPtr(device->newBuffer(MAX(scratch_size, uint64_t(1)), MTL::ResourceStorageModePrivate)));
	}
	Vector<uint64_t> stale_geometry_ids;
	for (const KeyValue<uint64_t, MetalCachedGeometry *> &entry : cache->geometries) {
		if (entry.value->last_seen_frame != cache->frame) {
			stale_geometry_ids.push_back(entry.key);
		}
	}
	for (const uint64_t geometry_id : stale_geometry_ids) {
		MetalCachedGeometry *stale = cache->geometries[geometry_id];
		cache->geometries.erase(geometry_id);
		memdelete(stale);
	}
	for (uint32_t i = 0; i < packet_header.instance_count; i++) {
		InstanceRecord instance = {};
		memcpy(&instance, packet + packet_header.instance_offset + i * sizeof(InstanceRecord), sizeof(instance));
		instance_geometry.write[i] = geometry_indices[instance.geometry_id];
		instance_material.write[i] = instance.material_id == 0 ? packet_header.material_count : uint32_t(instance.material_id - 1);
	}
	work->instance_geometry_buffer = NS::TransferPtr(device->newBuffer(instance_geometry.ptr(), instance_geometry.size() * sizeof(uint32_t), MTL::ResourceStorageModeShared));
	work->instance_material_buffer = NS::TransferPtr(device->newBuffer(instance_material.ptr(), instance_material.size() * sizeof(uint32_t), MTL::ResourceStorageModeShared));

	Vector<MTL::AccelerationStructure *> blas_pointers;
	blas_pointers.resize(work->blas.size());
	for (uint32_t i = 0; i < work->blas.size(); i++) {
		blas_pointers.write[i] = work->blas[i].get();
	}
	work->blas_array = NS::TransferPtr(NS::Array::array(reinterpret_cast<NS::Object *const *>(blas_pointers.ptr()), blas_pointers.size())->retain());
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> metal_instances;
	metal_instances.resize(packet_header.instance_count);
	for (uint32_t i = 0; i < packet_header.instance_count; i++) {
		InstanceRecord instance = {};
		memcpy(&instance, packet + packet_header.instance_offset + i * sizeof(InstanceRecord), sizeof(instance));
		MTL::AccelerationStructureUserIDInstanceDescriptor metal_instance = {};
		metal_instance.transformationMatrix = MTL::PackedFloat4x3(
				MTL::PackedFloat3(instance.world_from_object.columns[0].x, instance.world_from_object.columns[0].y, instance.world_from_object.columns[0].z),
				MTL::PackedFloat3(instance.world_from_object.columns[1].x, instance.world_from_object.columns[1].y, instance.world_from_object.columns[1].z),
				MTL::PackedFloat3(instance.world_from_object.columns[2].x, instance.world_from_object.columns[2].y, instance.world_from_object.columns[2].z),
				MTL::PackedFloat3(instance.world_from_object.columns[3].x, instance.world_from_object.columns[3].y, instance.world_from_object.columns[3].z));
		metal_instance.options = MTL::AccelerationStructureInstanceOptionOpaque;
		metal_instance.mask = instance.visibility_mask & 0xff;
		metal_instance.accelerationStructureIndex = instance_geometry[i];
		metal_instance.userID = i;
		metal_instances.write[i] = metal_instance;
	}
	work->tlas_instances = NS::TransferPtr(device->newBuffer(metal_instances.ptr(), metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor), MTL::ResourceStorageModeShared));
	work->tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
	work->tlas_descriptor->setInstancedAccelerationStructures(work->blas_array.get());
	work->tlas_descriptor->setInstanceDescriptorBuffer(work->tlas_instances.get());
	work->tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
	work->tlas_descriptor->setInstanceCount(packet_header.instance_count);
	const MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(work->tlas_descriptor.get());
	work->tlas = NS::TransferPtr(device->newAccelerationStructure(tlas_sizes.accelerationStructureSize));
	work->tlas_scratch = NS::TransferPtr(device->newBuffer(tlas_sizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));

	work->outputs.resize(packet_header.camera_count);
	work->parameters.resize(packet_header.camera_count);
	const Vector<RID> *guide_requests[10] = {
		&p_request.output_depth,
		&p_request.output_motion,
		&p_request.output_normal,
		&p_request.output_diffuse_albedo,
		&p_request.output_specular_albedo,
		&p_request.output_roughness,
		&p_request.output_denoise_strength,
		&p_request.output_reactive_mask,
		&p_request.output_specular_hit_distance,
		&p_request.output_transparency_overlay,
	};
	const MTL::PixelFormat guide_formats[10] = {
		MTL::PixelFormatR32Float,
		MTL::PixelFormatRG16Float,
		MTL::PixelFormatRGBA16Float,
		MTL::PixelFormatRGBA16Float,
		MTL::PixelFormatRGBA16Float,
		MTL::PixelFormatR16Float,
		MTL::PixelFormatR8Unorm,
		MTL::PixelFormatR8Unorm,
		MTL::PixelFormatR16Float,
		MTL::PixelFormatRGBA16Float,
	};
	const char *guide_names[10] = {
		"depth", "motion", "normal", "diffuse-albedo", "specular-albedo", "roughness",
		"denoise-strength", "reactive-mask", "specular-hit-distance", "transparency-overlay"
	};
	const uint32_t guide_flags[10] = {
		GUIDE_DEPTH, GUIDE_MOTION, GUIDE_NORMAL, GUIDE_DIFFUSE_ALBEDO, GUIDE_SPECULAR_ALBEDO,
		GUIDE_ROUGHNESS, GUIDE_DENOISE_STRENGTH, GUIDE_REACTIVE_MASK, GUIDE_SPECULAR_HIT_DISTANCE,
		GUIDE_TRANSPARENCY_OVERLAY
	};
	GuideContract guide_contract = {};
	SceneCompiler::read_record(p_request.capture, capture_header.scene_packet_offset + packet_header.guide_contract_offset, guide_contract);
	uint32_t requested_guides = 0;
	for (uint32_t guide = 0; guide < 10; guide++) {
		if (!guide_requests[guide]->is_empty()) {
			if ((guide_contract.enabled_guides & guide_flags[guide]) == 0) {
				memdelete(work);
				return _metal_path_tracing_fail(ERR_INVALID_PARAMETER, vformat("The capture does not enable the requested %s guide.", guide_names[guide]), r_error);
			}
			requested_guides |= guide_flags[guide];
			work->guide_outputs[guide].resize(packet_header.camera_count);
		}
	}
	for (uint32_t view = 0; view < packet_header.camera_count; view++) {
		CameraRecord camera = {};
		memcpy(&camera, packet + packet_header.camera_offset + view * sizeof(CameraRecord), sizeof(camera));
		MTL::Texture *texture = nullptr;
		if (_resolve_output_texture(rd, p_request.output_color[view], camera.render_width, camera.render_height, MTL::PixelFormatRGBA16Float, "color", texture, r_error) != OK) {
			memdelete(work);
			return ERR_INVALID_PARAMETER;
		}
		work->outputs.write[view] = texture;
		for (uint32_t guide = 0; guide < 10; guide++) {
			if (!guide_requests[guide]->is_empty()) {
				MTL::Texture *guide_texture = nullptr;
				if (_resolve_output_texture(rd, (*guide_requests[guide])[view], camera.render_width, camera.render_height, guide_formats[guide], guide_names[guide], guide_texture, r_error) != OK) {
					memdelete(work);
					return ERR_INVALID_PARAMETER;
				}
				work->guide_outputs[guide].write[view] = guide_texture;
			}
		}
		MetalFrameParameters parameters = {};
		parameters.dimensions_seed_view[0] = camera.render_width;
		parameters.dimensions_seed_view[1] = camera.render_height;
		parameters.dimensions_seed_view[2] = p_request.sample_index;
		parameters.dimensions_seed_view[3] = view;
		parameters.counts_and_guides[0] = packet_header.light_count;
		parameters.counts_and_guides[1] = requested_guides;
		parameters.counts_and_guides[2] = p_request.max_bounces;
		parameters.counts_and_guides[3] = p_request.mode == RenderMode::PROGRESSIVE_REFERENCE;
		parameters.world_from_view = _inverse_matrix(camera.view_from_world);
		parameters.view_from_clip = _inverse_matrix(camera.clip_from_view);
		work->parameters.write[view] = parameters;
	}

	RD::CallbackResource resources[SCENE_PACKET_MAX_VIEWS * 11];
	uint32_t resource_count = 0;
	for (uint32_t view = 0; view < packet_header.camera_count; view++) {
		resources[resource_count++] = { .rid = p_request.output_color[view], .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE };
		for (uint32_t guide = 0; guide < 10; guide++) {
			if (!guide_requests[guide]->is_empty()) {
				resources[resource_count++] = { .rid = (*guide_requests[guide])[view], .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE };
			}
		}
	}
	rd->capture_timestamp("Path Tracing BLAS Begin");
	uint32_t queued_callbacks = 0;
	const VectorView<RD::CallbackResource> callback_resources(resources, resource_count);
	Error error = rd->driver_callback_add((RDD::DriverCallback)_build_blas, work, callback_resources);
	queued_callbacks += error == OK ? 1 : 0;
	rd->capture_timestamp("Path Tracing BLAS End");
	if (error == OK) {
		rd->capture_timestamp("Path Tracing TLAS Begin");
		error = rd->driver_callback_add((RDD::DriverCallback)_build_tlas, work, callback_resources);
		queued_callbacks += error == OK ? 1 : 0;
		rd->capture_timestamp("Path Tracing TLAS End");
	}
	if (error == OK) {
		rd->capture_timestamp("Path Tracing Trace Begin");
		error = rd->driver_callback_add((RDD::DriverCallback)_trace, work, callback_resources);
		queued_callbacks += error == OK ? 1 : 0;
		rd->capture_timestamp("Path Tracing Trace End");
	}
	if (error != OK) {
		const uint32_t unscheduled_callbacks = work->pending_callbacks - queued_callbacks;
		for (uint32_t i = 0; i < unscheduled_callbacks; i++) {
			_release_work(work);
		}
		return error;
	}
	r_result.rendered_views = packet_header.camera_count;
	r_result.emitted_guides = requested_guides;
	r_result.history_reset = p_request.sample_index == 0;
	return OK;
}

} // namespace RendererRD

#endif // METAL_ENABLED
