/**************************************************************************/
/*  metal_hybrid_effect.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#ifdef METAL_ENABLED

#include "metal_hybrid_effect.h"

#include "drivers/metal/metal3_objects.h"
#include "drivers/metal/rendering_device_driver_metal.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"

#include <Metal/Metal.hpp>
#include <mach/mach_time.h>

#include <atomic>
#include <memory>

namespace RendererRD {

static constexpr uint32_t HYBRID_MAX_ALBEDO_TEXTURES = 16u;

struct MetalHybridCachedGeometry {
	uint64_t topology_revision = 0;
	uint64_t deformation_revision = 0;
	uint64_t last_seen_frame = 0;
	NS::SharedPtr<MTL::AccelerationStructure> acceleration_structure;
};

struct MetalHybridTimingCapture {
	NS::SharedPtr<MTL::CounterSampleBuffer> samples;
	std::atomic_bool complete = false;
	MTL::Timestamp cpu_begin = 0;
	MTL::Timestamp gpu_begin = 0;
	MTL::Timestamp cpu_end = 0;
	MTL::Timestamp gpu_end = 0;
	uint32_t view_count = 0;
	bool shadow_only = false;
};

struct MetalHybridEffectCache {
	HashMap<uint64_t, MetalHybridCachedGeometry *> geometries;
	NS::SharedPtr<MTL::ComputePipelineState> trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> shadow_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> filter_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> temporal_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> composite_pipeline;
	NS::SharedPtr<MTL::SamplerState> albedo_sampler;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;
	Vector<MTL::AccelerationStructure *> tlas_blas_order;
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> tlas_instances;
	Vector<std::shared_ptr<MetalHybridTimingCapture>> timing_captures;
	uint64_t frame = 0;

	~MetalHybridEffectCache() {
		for (const KeyValue<uint64_t, MetalHybridCachedGeometry *> &entry : geometries) {
			memdelete(entry.value);
		}
	}
};

static constexpr const char *HYBRID_MSL = R"(
#include <metal_raytracing>
#include <metal_stdlib>
using namespace metal;

#define HYBRID_MAX_ALBEDO_TEXTURES 16u

struct Parameters {
    float4x4 world_from_view;
    float4x4 view_from_clip;
    float4x4 clip_from_world;
    float4x4 prev_clip_from_world;
    float4 light_direction_and_reflection_strength;
    float4 ao_distance_strength_roughness_flags;
    uint2 dimensions;
    uint shadow_sample_count;
    float directional_light_angular_radius;
    uint gi_sample_count;
    uint frame_index;
	float gi_strength;
	uint history_valid;
	uint emissive_count;
};

struct MaterialRecord {
    float4 albedo_metallic;
    float4 emission_roughness;
	uint albedo_texture_index;
	uint padding0;
	uint padding1;
	uint padding2;
};

struct GeometryRecord {
    device const uchar *vertex_data;
    device const uchar *index_data;
	device const uchar *attribute_data;
    uint vertex_count;
    uint index_type;
    uint position_stride;
    uint normal_offset;
    uint normal_stride;
    uint has_normals;
	uint compressed;
	uint attribute_stride;
	uint uv_offset;
	uint has_uv;
	uint padding;
    float4x4 normal_from_object;
	float4x4 world_from_object;
	float4 position_scale;
	float4 position_offset;
};

struct EmissiveRecord {
	uint instance_id;
	uint triangle_count;
	uint2 padding;
};

static uint triangle_vertex_index(constant GeometryRecord &geometry, uint primitive_id, uint corner) {
	uint index = primitive_id * 3u + corner;
	if (geometry.index_data == nullptr) return index;
	if (geometry.index_type == 16u) return reinterpret_cast<device const ushort *>(geometry.index_data)[index];
	return reinterpret_cast<device const uint *>(geometry.index_data)[index];
}

static float3 world_vertex_position(constant GeometryRecord &geometry, uint vertex_index) {
	float3 object_position;
	if (geometry.compressed != 0u) {
		device const ushort4 *packed = reinterpret_cast<device const ushort4 *>(geometry.vertex_data + vertex_index * geometry.position_stride);
		object_position = float4(*packed).xyz / 65535.0f * geometry.position_scale.xyz + geometry.position_offset.xyz;
	} else {
		device const float3 *position = reinterpret_cast<device const float3 *>(geometry.vertex_data + vertex_index * geometry.position_stride);
		object_position = *position;
	}
	return (geometry.world_from_object * float4(object_position, 1.0f)).xyz;
}

static float3 decode_oct_normal(uint packed) {
    float2 oct = float2(packed & 0xffffu, packed >> 16u) / 65535.0f;
    oct = oct * 2.0f - 1.0f;
    float3 normal = float3(oct.x, oct.y, 1.0f - abs(oct.x) - abs(oct.y));
    if (normal.z < 0.0f) {
        normal.xy = (1.0f - abs(normal.yx)) * select(float2(-1.0f), float2(1.0f), normal.xy >= 0.0f);
    }
    return normalize(normal);
}

static float3 intersection_normal(constant GeometryRecord *geometry_records, uint instance_id, uint primitive_id, float2 barycentric, float3 fallback) {
    constant GeometryRecord &geometry = geometry_records[instance_id];
    if (geometry.has_normals == 0u) return fallback;
    uint indices[3];
    for (uint corner = 0; corner < 3; corner++) {
        uint index = primitive_id * 3u + corner;
        if (geometry.index_data == nullptr) {
            indices[corner] = index;
        } else if (geometry.index_type == 16u) {
            indices[corner] = reinterpret_cast<device const ushort *>(geometry.index_data)[index];
        } else {
            indices[corner] = reinterpret_cast<device const uint *>(geometry.index_data)[index];
        }
    }
    float3 normals[3];
    for (uint corner = 0; corner < 3; corner++) {
        device const uint *packed_normal = reinterpret_cast<device const uint *>(geometry.vertex_data + geometry.normal_offset + indices[corner] * geometry.normal_stride);
        normals[corner] = decode_oct_normal(*packed_normal);
    }
    float3 weights = float3(1.0f - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
    float3 object_normal = normals[0] * weights.x + normals[1] * weights.y + normals[2] * weights.z;
    return normalize((geometry.normal_from_object * float4(object_normal, 0.0f)).xyz);
}

static float2 intersection_uv(constant GeometryRecord *geometry_records, uint instance_id, uint primitive_id, float2 barycentric) {
	constant GeometryRecord &geometry = geometry_records[instance_id];
	if (geometry.has_uv == 0u || geometry.attribute_data == nullptr) return float2(0.0f);
	const uint indices[3] = {
		triangle_vertex_index(geometry, primitive_id, 0u),
		triangle_vertex_index(geometry, primitive_id, 1u),
		triangle_vertex_index(geometry, primitive_id, 2u),
	};
	float2 uvs[3];
	for (uint corner = 0u; corner < 3u; corner++) {
		device const uchar *attribute = geometry.attribute_data + geometry.uv_offset + indices[corner] * geometry.attribute_stride;
		if (geometry.compressed != 0u) {
			uvs[corner] = float2(*reinterpret_cast<device const ushort2 *>(attribute)) / 65535.0f;
		} else {
			uvs[corner] = *reinterpret_cast<device const float2 *>(attribute);
		}
	}
	const float3 weights = float3(1.0f - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
	return uvs[0] * weights.x + uvs[1] * weights.y + uvs[2] * weights.z;
}

static float3 material_albedo(
		thread const MaterialRecord &material,
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float2 barycentric,
		array<texture2d<float, access::sample>, HYBRID_MAX_ALBEDO_TEXTURES> albedo_textures,
		sampler albedo_sampler) {
	float3 albedo = material.albedo_metallic.rgb;
	if (material.albedo_texture_index < HYBRID_MAX_ALBEDO_TEXTURES) {
		const float2 uv = intersection_uv(geometry_records, instance_id, primitive_id, barycentric);
		albedo *= albedo_textures[material.albedo_texture_index].sample(albedo_sampler, uv, level(0.0f)).rgb;
	}
	return albedo;
}

static uint hash_u32(uint value) {
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu; return value ^ (value >> 16);
}

static float random_float(thread uint &state) {
    state = hash_u32(state + 0x9e3779b9u);
    return float(state & 0x00ffffffu) / float(0x01000000u);
}

static float radical_inverse_vdc(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f;
}

// Owen-scrambled, progressive Hammersley dimensions. Consecutive frames visit
// disjoint points from the same bounded sequence, while the per-pixel rotation
// stays fixed. This gives temporal reconstruction new, low-discrepancy samples
// without the large screen-wide lighting shifts produced by a frame-random hash.
static float hammersley_dimension(uint frame_index, uint sample, uint sample_count, uint pixel_seed, uint dimension) {
	const uint sequence_length = 256u;
	const uint sequence_index = (frame_index * max(sample_count, 1u) + sample) & (sequence_length - 1u);
	const uint scramble = hash_u32(pixel_seed ^ (0x9e3779b9u * (dimension + 1u)));
	const float rotation = float(scramble & 0x00ffffffu) / float(0x01000000u);
	if (dimension == 0u) {
		return fract((float(sequence_index) + 0.5f) / float(sequence_length) + rotation);
	}
	return fract(radical_inverse_vdc(sequence_index + dimension * 0x9e3779b9u) + rotation);
}

static float3 sample_cone(float3 axis, float angular_radius, thread uint &state) {
    if (angular_radius <= 0.00001f) return axis;
    float cos_max = cos(angular_radius);
    float cos_theta = mix(cos_max, 1.0f, random_float(state));
    float sin_theta = sqrt(max(0.0f, 1.0f - cos_theta * cos_theta));
    float phi = random_float(state) * 6.28318530718f;
    float3 helper = abs(axis.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(helper, axis));
    float3 bitangent = cross(axis, tangent);
    return normalize(axis * cos_theta + tangent * (cos(phi) * sin_theta) + bitangent * (sin(phi) * sin_theta));
}

static float3 sample_ggx_reflection(float3 incident, float3 normal, float roughness, uint frame_index, uint pixel_seed) {
	float alpha = max(roughness * roughness, 0.001f);
	float alpha_squared = alpha * alpha;
	float u1 = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, 6u);
	float u2 = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, 7u);
	float cos_theta = sqrt(max((1.0f - u1) / (1.0f + (alpha_squared - 1.0f) * u1), 0.0f));
	float sin_theta = sqrt(max(1.0f - cos_theta * cos_theta, 0.0f));
	float phi = 6.28318530718f * u2;
	float3 helper = abs(normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
	float3 tangent = normalize(cross(helper, normal));
	float3 bitangent = cross(normal, tangent);
	float3 half_vector = normalize(tangent * (cos(phi) * sin_theta) + bitangent * (sin(phi) * sin_theta) + normal * cos_theta);
	float3 reflected = normalize(reflect(incident, half_vector));
	return dot(reflected, normal) > 0.0f ? reflected : normalize(reflect(incident, normal));
}

static float smith_ggx_lambda(float normal_dot_direction, float alpha_squared) {
	float cos_squared = max(normal_dot_direction * normal_dot_direction, 0.000001f);
	float tan_squared = max((1.0f - cos_squared) / cos_squared, 0.0f);
	return 0.5f * (-1.0f + sqrt(1.0f + alpha_squared * tan_squared));
}

// The reflection direction is sampled from the GGX half-vector distribution.
// Convert that sample to a radiance estimator here; multiplying by Fresnel a
// second time after this function would incorrectly brighten or darken glossy
// transport, especially for the low-roughness validation sphere.
static float3 ggx_reflection_throughput(float3 incident, float3 normal, float3 reflected, float roughness, float3 f0) {
	float3 view = -incident;
	float normal_dot_view = max(dot(normal, view), 0.0001f);
	float normal_dot_light = max(dot(normal, reflected), 0.0001f);
	float3 half_vector = normalize(view + reflected);
	float normal_dot_half = max(dot(normal, half_vector), 0.0001f);
	float view_dot_half = max(dot(view, half_vector), 0.0001f);
	float alpha = max(roughness * roughness, 0.001f);
	float alpha_squared = alpha * alpha;
	float geometry = 1.0f / (1.0f + smith_ggx_lambda(normal_dot_view, alpha_squared) + smith_ggx_lambda(normal_dot_light, alpha_squared));
	float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - view_dot_half, 5.0f);
	return fresnel * geometry * view_dot_half / max(normal_dot_view * normal_dot_half, 0.0001f);
}

static float3 sample_emissive_lighting(
		float3 world_position,
		float3 world_normal,
		float3 diffuse_albedo,
		uint sample_count,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
		constant EmissiveRecord *emissives,
		uint emissive_count,
		uint frame_index,
		thread uint &state,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector,
		raytracing::instance_acceleration_structure scene) {
	if (emissive_count == 0u || all(diffuse_albedo <= float3(0.0001f))) return 0.0f;
	float3 result = 0.0f;
	sample_count = max(sample_count, 1u);
	const uint pixel_seed = state;
	for (uint sample = 0u; sample < sample_count; sample++) {
		uint emitter_index = min(uint(hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 0u) * float(emissive_count)), emissive_count - 1u);
		constant EmissiveRecord &emitter = emissives[emitter_index];
		constant GeometryRecord &geometry = geometry_records[emitter.instance_id];
		uint primitive_id = min(uint(hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 1u) * float(emitter.triangle_count)), emitter.triangle_count - 1u);
		float3 p0 = world_vertex_position(geometry, triangle_vertex_index(geometry, primitive_id, 0u));
		float3 p1 = world_vertex_position(geometry, triangle_vertex_index(geometry, primitive_id, 1u));
		float3 p2 = world_vertex_position(geometry, triangle_vertex_index(geometry, primitive_id, 2u));
		float3 area_vector = cross(p1 - p0, p2 - p0);
		float triangle_area = 0.5f * length(area_vector);
		if (triangle_area <= 0.000001f) continue;
		float sqrt_x = sqrt(hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 2u));
		float bary_y = hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 3u);
		float3 light_position = p0 * (1.0f - sqrt_x) + p1 * (sqrt_x * (1.0f - bary_y)) + p2 * (sqrt_x * bary_y);
		float3 to_light = light_position - world_position;
		float distance_squared = dot(to_light, to_light);
		float distance_to_light = sqrt(distance_squared);
		if (distance_to_light <= 0.02f) continue;
		float3 light_direction = to_light / distance_to_light;
		float surface_cosine = max(dot(world_normal, light_direction), 0.0f);
		// Godot's packed triangle winding is opposite the visible material normal
		// in this Metal geometry path. Recover the material-facing emitter normal,
		// then evaluate a one-sided cosine term. abs() here lit both sides of the
		// Cornell ceiling panel and produced the nonphysical wall/ceiling patches.
		float3 emitter_normal = -normalize(area_vector);
		float light_cosine = max(dot(emitter_normal, -light_direction), 0.0f);
		if (surface_cosine <= 0.0f || light_cosine <= 0.0f) continue;
		raytracing::ray visibility_ray = { world_position + world_normal * 0.003f, light_direction, 0.001f, distance_to_light - 0.01f };
		auto blocker = intersector.intersect(visibility_ray, scene, 0xff);
		if (blocker.type == raytracing::intersection_type::triangle) continue;
		float inverse_pdf = float(emissive_count * emitter.triangle_count) * triangle_area;
		float3 emitted_radiance = materials[emitter.instance_id].emission_roughness.rgb;
		result += diffuse_albedo * (1.0f / M_PI_F) * emitted_radiance * surface_cosine * light_cosine * inverse_pdf / max(distance_squared, 0.0001f);
	}
	return clamp(result / float(sample_count), 0.0f, 32.0f);
}

kernel void trace_hybrid_shadow(
    raytracing::instance_acceleration_structure scene [[buffer(0)]],
    constant Parameters &parameters [[buffer(1)]],
    texture2d<float, access::read> depth_texture [[texture(0)]],
    texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
    texture2d<float, access::write> effect_texture [[texture(2)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float depth = depth_texture.read(pixel).r;
    if (depth <= 0.000001f) {
        effect_texture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
        return;
    }
    float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
	// Forward+ applies its TAA offset to the raster projection. Convert the
	// jittered depth sample back to the unjittered projection used below.
	float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
    float3 view_position = view_h.xyz / view_h.w;
    float3 world_position = (parameters.world_from_view * float4(view_position, 1.0f)).xyz;
    float3 view_normal = normalize(normal_roughness_texture.read(pixel).xyz * 2.0f - 1.0f);
    float3 world_normal = normalize((parameters.world_from_view * float4(view_normal, 0.0f)).xyz);
    float3 light_direction = normalize(parameters.light_direction_and_reflection_strength.xyz);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
    uint state = hash_u32(pixel.x + pixel.y * parameters.dimensions.x);
	uint flags = uint(parameters.ao_distance_strength_roughness_flags.w);
	float occlusion = 1.0f;
	if ((flags & 2u) != 0u) {
		float angle = random_float(state) * 6.28318530718f;
		float3 helper = abs(world_normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
		float3 tangent = normalize(cross(helper, world_normal));
		float3 bitangent = cross(world_normal, tangent);
		float3 ao_direction = normalize(world_normal + 0.65f * (cos(angle) * tangent + sin(angle) * bitangent));
		raytracing::ray ao_ray = { world_position + world_normal * 0.002f, ao_direction, 0.001f, parameters.ao_distance_strength_roughness_flags.x };
		auto blocker = intersector.intersect(ao_ray, scene, 0xff);
		if (blocker.type == raytracing::intersection_type::triangle) {
			float proximity = 1.0f - clamp(blocker.distance / parameters.ao_distance_strength_roughness_flags.x, 0.0f, 1.0f);
			occlusion = 1.0f - proximity * parameters.ao_distance_strength_roughness_flags.y;
		}
	}
    float visibility = 0.0f;
    uint sample_count = max(parameters.shadow_sample_count, 1u);
    for (uint sample = 0; sample < sample_count; sample++) {
        float3 direction = sample_cone(light_direction, parameters.directional_light_angular_radius, state);
        raytracing::ray ray = { world_position + world_normal * 0.003f, direction, 0.001f, 100000.0f };
        auto hit = intersector.intersect(ray, scene, 0xff);
        visibility += hit.type == raytracing::intersection_type::triangle ? 0.0f : 1.0f;
    }
    effect_texture.write(float4(occlusion, 0.0f, 0.0f, visibility / float(sample_count)), pixel);
}

kernel void trace_hybrid(
    raytracing::instance_acceleration_structure scene [[buffer(0)]],
    constant Parameters &parameters [[buffer(1)]],
    constant MaterialRecord *materials [[buffer(2)]],
    constant GeometryRecord *geometry_records [[buffer(3)]],
	constant EmissiveRecord *emissives [[buffer(4)]],
    depth2d<float, access::read> depth_texture [[texture(0)]],
    texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
    texture2d<float, access::read> color_texture [[texture(2)]],
    texture2d<float, access::write> effect_texture [[texture(3)]],
	texture2d<float, access::write> guide_normal [[texture(4)]],
	texture2d<float, access::write> guide_diffuse [[texture(5)]],
	texture2d<float, access::write> guide_specular [[texture(6)]],
	texture2d<float, access::write> guide_roughness [[texture(7)]],
	texture2d<float, access::write> guide_denoise_strength [[texture(8)]],
	texture2d<float, access::write> guide_reactive [[texture(9)]],
	texture2d<float, access::write> guide_specular_distance [[texture(10)]],
	texture2d<float, access::write> guide_transparency [[texture(11)]],
	array<texture2d<float, access::sample>, HYBRID_MAX_ALBEDO_TEXTURES> albedo_textures [[texture(12)]],
	sampler albedo_sampler [[sampler(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float depth = depth_texture.read(pixel);
    if (depth <= 0.0f) {
        effect_texture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
		guide_normal.write(float4(0.0f), pixel);
		guide_diffuse.write(float4(0.0f), pixel);
		guide_specular.write(float4(0.0f), pixel);
		guide_roughness.write(float4(1.0f), pixel);
		guide_denoise_strength.write(float4(1.0f), pixel);
		guide_reactive.write(float4(0.0f), pixel);
		guide_specular_distance.write(float4(0.0f), pixel);
		guide_transparency.write(float4(0.0f), pixel);
        return;
    }
    float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
    float3 view_position = view_h.xyz / view_h.w;
    float3 world_position = (parameters.world_from_view * float4(view_position, 1.0f)).xyz;
    float4 normal_roughness = normal_roughness_texture.read(pixel);
    float3 view_normal = normalize(normal_roughness.xyz * 2.0f - 1.0f);
    float3 world_normal = normalize((parameters.world_from_view * float4(view_normal, 0.0f)).xyz);
    float roughness = normal_roughness.w;
    roughness = roughness > 0.5f ? (1.0f - roughness) * (255.0f / 127.0f) : roughness * (255.0f / 127.0f);
    float3 view_direction = normalize(world_position - parameters.world_from_view[3].xyz);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
	// The first-frame hybrid path is intentionally screen-space deterministic.
	// A changing hash here makes one-sample area-light and GGX estimates form
	// large travelling lobes that a temporal scaler cannot classify as motion.
	// Per-pixel scrambling still decorrelates neighboring rays; camera motion is
	// handled by the primary depth/motion guides rather than a random frame seed.
	uint state = hash_u32(pixel.x + pixel.y * parameters.dimensions.x);
	float3 primary_diffuse = 1.0f;
	float3 primary_f0 = 0.04f;
	float3 camera_position = parameters.world_from_view[3].xyz;
	float3 primary_direction = world_position - camera_position;
	float primary_distance = length(primary_direction);
	if (primary_distance > 0.001f) {
		raytracing::ray primary_ray = { camera_position, primary_direction / primary_distance, 0.001f, 100000.0f };
		auto primary_hit = intersector.intersect(primary_ray, scene, 0xff);
		if (primary_hit.type == raytracing::intersection_type::triangle) {
			MaterialRecord primary_material = materials[primary_hit.instance_id];
			float primary_metallic = primary_material.albedo_metallic.a;
			float3 primary_albedo = material_albedo(primary_material, geometry_records, primary_hit.instance_id, primary_hit.primitive_id, primary_hit.triangle_barycentric_coord, albedo_textures, albedo_sampler);
			primary_diffuse = primary_albedo * (1.0f - primary_metallic);
			primary_f0 = mix(float3(0.04f), primary_albedo, primary_metallic);
			world_position = primary_ray.origin + primary_ray.direction * primary_hit.distance;
			world_normal = intersection_normal(geometry_records, primary_hit.instance_id, primary_hit.primitive_id, primary_hit.triangle_barycentric_coord, -primary_ray.direction);
			if (dot(world_normal, -primary_ray.direction) < 0.0f) world_normal = -world_normal;
			roughness = primary_material.emission_roughness.a;
		}
	}
	float3 emissive_direct = sample_emissive_lighting(world_position, world_normal, primary_diffuse, max(parameters.shadow_sample_count, 2u), materials, geometry_records, emissives, parameters.emissive_count, parameters.frame_index, state, intersector, scene);
	float3 reflection = 0.0f;
	float reflection_weight = 0.0f;
	float specular_hit_distance = 0.0f;
	// MetalFX denoises the primary image while using these guides to identify
	// transport. For a ray-validated reflection, describe the reflected surface
	// instead of the mirror surface. This is primary-surface replacement: depth
	// and motion still belong to the mirror pixel, while material/normal guides
	// describe the radiance source. It costs no additional ray query.
	float3 reflection_guide_normal = world_normal;
	float3 reflection_guide_diffuse = primary_diffuse;
	float3 reflection_guide_f0 = primary_f0;
	float reflection_guide_roughness = roughness;
	float reflection_guide_cosine = max(dot(-view_direction, world_normal), 0.0f);
    uint flags = uint(parameters.ao_distance_strength_roughness_flags.w);
    if ((flags & 1u) != 0u && roughness <= parameters.ao_distance_strength_roughness_flags.z) {
		float3 reflected = sample_ggx_reflection(view_direction, world_normal, roughness, parameters.frame_index, state);
        raytracing::ray ray = { world_position + world_normal * 0.002f, reflected, 0.001f, 10000.0f };
        auto hit = intersector.intersect(ray, scene, 0xff);
		if (hit.type == raytracing::intersection_type::triangle) {
			specular_hit_distance = hit.distance;
			MaterialRecord material = materials[hit.instance_id];
			float3 hit_normal = intersection_normal(geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, -ray.direction);
			if (dot(hit_normal, -ray.direction) < 0.0f) hit_normal = -hit_normal;
			float3 hit_position = ray.origin + ray.direction * hit.distance;
			float3 hit_albedo = material_albedo(material, geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, albedo_textures, albedo_sampler);
			float3 hit_diffuse = hit_albedo * (1.0f - material.albedo_metallic.a);
			reflection_guide_normal = hit_normal;
			reflection_guide_diffuse = hit_diffuse;
			reflection_guide_f0 = mix(float3(0.04f), hit_albedo, material.albedo_metallic.a);
			reflection_guide_roughness = material.emission_roughness.a;
			reflection_guide_cosine = max(dot(-ray.direction, hit_normal), 0.0f);
			reflection = material.emission_roughness.rgb + sample_emissive_lighting(hit_position, hit_normal, hit_diffuse, 1u, materials, geometry_records, emissives, parameters.emissive_count, parameters.frame_index, state, intersector, scene);
        } else {
			reflection = float3(0.002f);
        }
		reflection *= ggx_reflection_throughput(view_direction, world_normal, reflected, roughness, primary_f0);
		reflection_weight = parameters.light_direction_and_reflection_strength.w;
    }
    float3 indirect = 0.0f;
    if ((flags & 4u) != 0u) {
        float3 helper = abs(world_normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        float3 tangent = normalize(cross(helper, world_normal));
        float3 bitangent = cross(world_normal, tangent);
		uint state = hash_u32(pixel.x + pixel.y * parameters.dimensions.x);
		uint sample_count = max(parameters.gi_sample_count, 1u);
		for (uint sample = 0; sample < sample_count; sample++) {
			const float u = hammersley_dimension(parameters.frame_index, sample, sample_count, state, 4u);
			const float v = hammersley_dimension(parameters.frame_index, sample, sample_count, state, 5u);
			float phi = v * 6.28318530718f;
			float radius = sqrt(u);
            float z = sqrt(max(0.0f, 1.0f - radius * radius));
            float3 direction = normalize(tangent * (cos(phi) * radius) + bitangent * (sin(phi) * radius) + world_normal * z);
            raytracing::ray gi_ray = { world_position + world_normal * 0.003f, direction, 0.001f, 100000.0f };
            auto gi_hit = intersector.intersect(gi_ray, scene, 0xff);
            float3 incoming = 0.0f;
			if (gi_hit.type == raytracing::intersection_type::triangle) {
				MaterialRecord material = materials[gi_hit.instance_id];
				float3 hit_normal = intersection_normal(geometry_records, gi_hit.instance_id, gi_hit.primitive_id, gi_hit.triangle_barycentric_coord, -gi_ray.direction);
				// Secondary diffuse direct-light evaluation is one-sided. Match the
				// primary/reflection-hit convention so a valid interior hit does not
				// reject the area emitter merely because its packed vertex winding is
				// opposite the ray-facing geometric normal.
				if (dot(hit_normal, -gi_ray.direction) < 0.0f) hit_normal = -hit_normal;
				float3 hit_position = gi_ray.origin + gi_ray.direction * gi_hit.distance;
				float3 hit_albedo = material_albedo(material, geometry_records, gi_hit.instance_id, gi_hit.primitive_id, gi_hit.triangle_barycentric_coord, albedo_textures, albedo_sampler);
				float3 hit_diffuse = hit_albedo * (1.0f - material.albedo_metallic.a);
				incoming = material.emission_roughness.rgb + sample_emissive_lighting(hit_position, hit_normal, hit_diffuse, 1u, materials, geometry_records, emissives, parameters.emissive_count, parameters.frame_index, state, intersector, scene);
            } else {
				incoming = float3(0.002f);
            }
            indirect += incoming;
        }
        indirect *= primary_diffuse * parameters.gi_strength / float(sample_count);
    }
	effect_texture.write(float4(clamp(emissive_direct + reflection * reflection_weight + indirect, 0.0f, 32.0f), 1.0f), pixel);
	// Apple MetalFX expects world-space geometric guides. Reflections use the
	// already-traced hit surface above; non-reflective pixels use the ray-validated
	// primary surface rather than the packed Forward+ normal/roughness buffer.
	guide_normal.write(float4(reflection_guide_normal, 0.0f), pixel);
	guide_diffuse.write(float4(clamp(reflection_guide_diffuse, 0.0f, 1.0f), 1.0f), pixel);
	// MetalFX needs the view-dependent Fresnel albedo that contributed to this
	// pixel, not a normal-incidence F0 material parameter.
	float3 specular_albedo = reflection_guide_f0 + (1.0f - reflection_guide_f0) * pow(1.0f - reflection_guide_cosine, 5.0f);
	guide_specular.write(float4(clamp(specular_albedo, 0.0f, 1.0f), 1.0f), pixel);
	guide_roughness.write(float4(clamp(reflection_guide_roughness, 0.0f, 1.0f)), pixel);
	// Apple's denoise-strength mask uses 1 to exclude a pixel from denoising,
	// and a reactive value of 1 discards temporal history. The full hybrid output
	// carries stochastic direct, indirect, and reflected transport for every
	// opaque primary surface, so a stable reflection must remain denoisable and
	// temporally reconstructible. Genuine motion/disocclusion is represented by
	// Godot's depth/motion guides and the explicit history-reset contract.
	guide_denoise_strength.write(float4(0.0f), pixel);
	guide_reactive.write(float4(0.0f), pixel);
	guide_specular_distance.write(float4(specular_hit_distance), pixel);
	// Full hybrid MetalFX is gated to opaque frames. A zero overlay is therefore
	// semantically correct and never pretends to represent alpha compositing.
	guide_transparency.write(float4(0.0f), pixel);
}

kernel void composite_hybrid(
    texture2d<float, access::read> effect_texture [[texture(0)]],
    texture2d<float, access::read_write> color_texture [[texture(1)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= uint2(color_texture.get_width(), color_texture.get_height()))) return;
    float4 color = color_texture.read(pixel);
    float4 effect = effect_texture.read(pixel);
    color.rgb += effect.rgb;
    color_texture.write(color, pixel);
}

kernel void filter_hybrid(
    constant Parameters &parameters [[buffer(0)]],
    depth2d<float, access::read> depth_texture [[texture(0)]],
    texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
    texture2d<float, access::read> effect_texture [[texture(2)]],
    texture2d<float, access::write> filtered_texture [[texture(3)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float center_depth = depth_texture.read(pixel);
    if (center_depth <= 0.0f) {
        filtered_texture.write(effect_texture.read(pixel), pixel);
        return;
    }
    float3 center_normal = normalize(normal_roughness_texture.read(pixel).xyz * 2.0f - 1.0f);
    float4 sum = 0.0f;
    float weight_sum = 0.0f;
    int2 dimensions = int2(parameters.dimensions);
    for (int y = -3; y <= 3; y++) {
        for (int x = -3; x <= 3; x++) {
            int2 sample_position = clamp(int2(pixel) + int2(x, y), int2(0), dimensions - 1);
            uint2 sample_pixel = uint2(sample_position);
            float sample_depth = depth_texture.read(sample_pixel);
            if (sample_depth <= 0.0f) continue;
            float3 sample_normal = normalize(normal_roughness_texture.read(sample_pixel).xyz * 2.0f - 1.0f);
            float spatial_weight = exp(-0.18f * float(x * x + y * y));
            float normal_weight = pow(max(dot(center_normal, sample_normal), 0.0f), 24.0f);
            float relative_depth = abs(sample_depth - center_depth) / max(abs(center_depth), 0.0001f);
            float depth_weight = exp(-relative_depth * 300.0f);
            float weight = spatial_weight * normal_weight * depth_weight;
            sum += effect_texture.read(sample_pixel) * weight;
            weight_sum += weight;
        }
    }
    filtered_texture.write(sum / max(weight_sum, 0.0001f), pixel);
}

kernel void accumulate_hybrid(
    constant Parameters &parameters [[buffer(0)]],
    texture2d<float, access::read> current_effect [[texture(0)]],
    depth2d<float, access::read> current_depth [[texture(1)]],
    texture2d<float, access::read> current_normal [[texture(2)]],
    texture2d<float, access::read> velocity_texture [[texture(3)]],
    texture2d<float, access::read> history_effect [[texture(4)]],
    texture2d<float, access::read> history_depth [[texture(5)]],
    texture2d<float, access::read> history_normal [[texture(6)]],
    texture2d<float, access::write> output_effect [[texture(7)]],
    texture2d<float, access::write> output_depth [[texture(8)]],
    texture2d<float, access::write> output_normal [[texture(9)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float depth = current_depth.read(pixel);
    float4 normal_roughness = current_normal.read(pixel);
    float4 current = current_effect.read(pixel);
    float4 accumulated = current;
    if (parameters.history_valid != 0u && depth > 0.0f) {
        float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
        float2 previous_uv = uv + velocity_texture.read(pixel).xy;
        if (all(previous_uv >= 0.0f) && all(previous_uv < 1.0f)) {
            uint2 previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
            float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
            float3 world_position = (parameters.world_from_view * float4(view_h.xyz / view_h.w, 1.0f)).xyz;
            float4 previous_clip = parameters.prev_clip_from_world * float4(world_position, 1.0f);
            float expected_depth = previous_clip.z / previous_clip.w;
            float old_depth = history_depth.read(previous_pixel).r;
            float3 old_normal = normalize(history_normal.read(previous_pixel).xyz * 2.0f - 1.0f);
            float3 new_normal = normalize(normal_roughness.xyz * 2.0f - 1.0f);
            float depth_error = abs(old_depth - expected_depth) / max(abs(expected_depth), 0.0001f);
            bool valid = previous_clip.w > 0.0f && old_depth > 0.0f && depth_error < 0.02f && dot(old_normal, new_normal) > 0.8f;
            if (valid) {
                float4 old = history_effect.read(previous_pixel);
				float history_samples = clamp(old.a, 0.0f, 31.0f);
				float history_weight = min(history_samples / (history_samples + 1.0f), 0.96875f);
				accumulated.rgb = mix(current.rgb, old.rgb, history_weight);
				accumulated.a = min(history_samples + 1.0f, 32.0f);
            }
        }
    }
    output_effect.write(accumulated, pixel);
    output_depth.write(float4(depth), pixel);
    output_normal.write(normal_roughness, pixel);
}
)";

struct MetalHybridParameters {
	simd::float4x4 world_from_view;
	simd::float4x4 view_from_clip;
	simd::float4x4 clip_from_world;
	simd::float4x4 prev_clip_from_world;
	simd::float4 light_direction_and_reflection_strength;
	simd::float4 ao_distance_strength_roughness_flags;
	simd::uint2 dimensions;
	uint32_t shadow_sample_count;
	float directional_light_angular_radius;
	uint32_t gi_sample_count;
	uint32_t frame_index;
	float gi_strength;
	uint32_t history_valid;
	uint32_t emissive_count;
};

struct MetalHybridMaterial {
	simd::float4 albedo_metallic;
	simd::float4 emission_roughness;
	uint32_t albedo_texture_index = 0xffffffffu;
	uint32_t padding[3] = {};
};

struct MetalHybridGeometry {
	uint64_t vertex_address = 0;
	uint64_t index_address = 0;
	uint64_t attribute_address = 0;
	uint32_t vertex_count = 0;
	uint32_t index_type = 0;
	uint32_t position_stride = 0;
	uint32_t normal_offset = 0;
	uint32_t normal_stride = 0;
	uint32_t has_normals = 0;
	uint32_t compressed = 0;
	uint32_t attribute_stride = 0;
	uint32_t uv_offset = 0;
	uint32_t has_uv = 0;
	uint32_t padding = 0;
	simd::float4x4 normal_from_object;
	simd::float4x4 world_from_object;
	simd::float4 position_scale;
	simd::float4 position_offset;
};

static_assert(sizeof(MetalHybridMaterial) == 48, "MSL MaterialRecord ABI drifted.");
static_assert(sizeof(MetalHybridGeometry) == 240, "MSL GeometryRecord ABI drifted.");

struct MetalHybridEmissive {
	uint32_t instance_id = 0;
	uint32_t triangle_count = 0;
	uint32_t padding[2] = {};
};

struct MetalHybridWork {
	NS::SharedPtr<MTL::ComputePipelineState> trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> shadow_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> filter_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> temporal_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> composite_pipeline;
	Vector<NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor>> blas_descriptors;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas_sources;
	Vector<NS::SharedPtr<MTL::Buffer>> blas_scratch;
	Vector<uint8_t> blas_actions;
	Vector<NS::SharedPtr<MTL::Buffer>> decode_transforms;
	NS::SharedPtr<NS::Array> blas_array;
	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;
	NS::SharedPtr<MTL::Buffer> tlas_scratch;
	NS::SharedPtr<MTL::Buffer> tlas_instances;
	NS::SharedPtr<MTL::Buffer> materials;
	NS::SharedPtr<MTL::Buffer> geometries;
	NS::SharedPtr<MTL::Buffer> emissives;
	NS::SharedPtr<MTL::SamplerState> albedo_sampler;
	bool tlas_build = false;
	Vector<MTL::Buffer *> vertex_buffers;
	Vector<MTL::Buffer *> index_buffers;
	Vector<MTL::Buffer *> attribute_buffers;
	Vector<MTL::Texture *> albedo_textures;
	Vector<MTL::Texture *> color;
	Vector<MTL::Texture *> depth;
	Vector<MTL::Texture *> normal_roughness;
	Vector<MTL::Texture *> effect_output;
	Vector<MTL::Texture *> filtered_output;
	Vector<MTL::Texture *> velocity;
	Vector<MTL::Texture *> history_input;
	Vector<MTL::Texture *> history_output;
	Vector<MTL::Texture *> depth_history_input;
	Vector<MTL::Texture *> depth_history_output;
	Vector<MTL::Texture *> normal_history_input;
	Vector<MTL::Texture *> normal_history_output;
	Vector<MTL::Texture *> guide_normal;
	Vector<MTL::Texture *> guide_diffuse;
	Vector<MTL::Texture *> guide_specular;
	Vector<MTL::Texture *> guide_roughness;
	Vector<MTL::Texture *> guide_denoise_strength;
	Vector<MTL::Texture *> guide_reactive;
	Vector<MTL::Texture *> guide_specular_distance;
	Vector<MTL::Texture *> guide_transparency;
	Vector<MetalHybridParameters> parameters;
	std::shared_ptr<MetalHybridTimingCapture> timing;
	bool shadow_only = false;
	bool temporal_enabled = false;
	bool metalfx_denoiser = false;
};

static simd::float4x4 _metal_matrix(const Projection &p_matrix) {
	simd::float4x4 matrix;
	for (uint32_t column = 0; column < 4; column++) {
		matrix.columns[column] = simd_make_float4(p_matrix.columns[column].x, p_matrix.columns[column].y, p_matrix.columns[column].z, p_matrix.columns[column].w);
	}
	return matrix;
}

static simd::float4x4 _metal_matrix(const Transform3D &p_transform) {
	const Vector3 x = p_transform.basis.get_column(0);
	const Vector3 y = p_transform.basis.get_column(1);
	const Vector3 z = p_transform.basis.get_column(2);
	return simd::float4x4(simd_make_float4(x.x, x.y, x.z, 0.0f),
			simd_make_float4(y.x, y.y, y.z, 0.0f),
			simd_make_float4(z.x, z.y, z.z, 0.0f),
			simd_make_float4(p_transform.origin.x, p_transform.origin.y, p_transform.origin.z, 1.0f));
}

static void _hybrid_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, MetalHybridWork *p_work) {
	RenderingDeviceDriverMetal *metal_driver = static_cast<RenderingDeviceDriverMetal *>(p_driver);
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	MTL::CommandBuffer *command_buffer = metal_command->get_command_buffer();
	if (p_work->timing) {
		metal_driver->get_device()->sampleTimestamps(&p_work->timing->cpu_begin, &p_work->timing->gpu_begin);
	}
	auto acceleration_encoder = [&](uint32_t p_begin, uint32_t p_end) {
		if (!p_work->timing) {
			return command_buffer->accelerationStructureCommandEncoder();
		}
		MTL::AccelerationStructurePassDescriptor *descriptor = MTL::AccelerationStructurePassDescriptor::accelerationStructurePassDescriptor();
		MTL::AccelerationStructurePassSampleBufferAttachmentDescriptor *attachment = descriptor->sampleBufferAttachments()->object(0);
		attachment->setSampleBuffer(p_work->timing->samples.get());
		attachment->setStartOfEncoderSampleIndex(p_begin);
		attachment->setEndOfEncoderSampleIndex(p_end);
		return command_buffer->accelerationStructureCommandEncoder(descriptor);
	};
	bool has_blas_work = false;
	for (uint8_t action : p_work->blas_actions) {
		has_blas_work |= action != 0;
	}
	if (has_blas_work) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = acceleration_encoder(0, 1);
		for (uint32_t index = 0; index < p_work->blas.size(); index++) {
			if (p_work->blas_actions[index] == 1) {
				as_encoder->buildAccelerationStructure(p_work->blas[index].get(), p_work->blas_descriptors[index].get(), p_work->blas_scratch[index].get(), 0);
			} else if (p_work->blas_actions[index] == 2) {
				as_encoder->refitAccelerationStructure(p_work->blas_sources[index].get(), p_work->blas_descriptors[index].get(), p_work->blas[index].get(), p_work->blas_scratch[index].get(), 0, MTL::AccelerationStructureRefitOptionVertexData);
			}
		}
		as_encoder->endEncoding();
	}
	if (p_work->tlas_build) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = acceleration_encoder(2, 3);
		as_encoder->buildAccelerationStructure(p_work->tlas.get(), p_work->tlas_descriptor.get(), p_work->tlas_scratch.get(), 0);
		as_encoder->endEncoding();
	}
	for (uint32_t view = 0; view < p_work->color.size(); view++) {
		const uint32_t timing_base = 4 + view * 8;
		auto compute_encoder = [&](uint32_t p_begin, uint32_t p_end) {
			if (!p_work->timing) {
				return command_buffer->computeCommandEncoder();
			}
			MTL::ComputePassDescriptor *descriptor = MTL::ComputePassDescriptor::computePassDescriptor();
			MTL::ComputePassSampleBufferAttachmentDescriptor *attachment = descriptor->sampleBufferAttachments()->object(0);
			attachment->setSampleBuffer(p_work->timing->samples.get());
			attachment->setStartOfEncoderSampleIndex(p_begin);
			attachment->setEndOfEncoderSampleIndex(p_end);
			return command_buffer->computeCommandEncoder(descriptor);
		};
		MTL::ComputeCommandEncoder *trace = compute_encoder(timing_base, timing_base + 1);
		trace->setComputePipelineState(p_work->shadow_only ? p_work->shadow_pipeline.get() : p_work->trace_pipeline.get());
		trace->setAccelerationStructure(p_work->tlas.get(), 0);
		// The TLAS indirectly references primitive acceleration structures. Declare
		// both levels resident for every trace rather than relying on a build in an
		// earlier encoder to keep the primitive resources available.
		trace->useResource(p_work->tlas.get(), MTL::ResourceUsageRead);
		for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : p_work->blas) {
			trace->useResource(blas.get(), MTL::ResourceUsageRead);
		}
		trace->setBytes(&p_work->parameters[view], sizeof(MetalHybridParameters), 1);
		if (!p_work->shadow_only) {
			trace->setBuffer(p_work->materials.get(), 0, 2);
			trace->setBuffer(p_work->geometries.get(), 0, 3);
			trace->setBuffer(p_work->emissives.get(), 0, 4);
			for (MTL::Buffer *buffer : p_work->vertex_buffers) {
				trace->useResource(buffer, MTL::ResourceUsageRead);
			}
			for (MTL::Buffer *buffer : p_work->index_buffers) {
				if (buffer) {
					trace->useResource(buffer, MTL::ResourceUsageRead);
				}
			}
			for (MTL::Buffer *buffer : p_work->attribute_buffers) {
				if (buffer) {
					trace->useResource(buffer, MTL::ResourceUsageRead);
				}
			}
			for (uint32_t texture_index = 0; texture_index < p_work->albedo_textures.size(); texture_index++) {
				trace->setTexture(p_work->albedo_textures[texture_index], 12 + texture_index);
				trace->useResource(p_work->albedo_textures[texture_index], MTL::ResourceUsageRead);
			}
			trace->setSamplerState(p_work->albedo_sampler.get(), 0);
		}
		trace->setTexture(p_work->depth[view], 0);
		trace->setTexture(p_work->normal_roughness[view], 1);
		if (p_work->shadow_only) {
			trace->setTexture(p_work->effect_output[view], 2);
		} else {
			trace->setTexture(p_work->color[view], 2);
			trace->setTexture(p_work->effect_output[view], 3);
			trace->setTexture(p_work->guide_normal[view], 4);
			trace->setTexture(p_work->guide_diffuse[view], 5);
			trace->setTexture(p_work->guide_specular[view], 6);
			trace->setTexture(p_work->guide_roughness[view], 7);
			trace->setTexture(p_work->guide_denoise_strength[view], 8);
			trace->setTexture(p_work->guide_reactive[view], 9);
			trace->setTexture(p_work->guide_specular_distance[view], 10);
			trace->setTexture(p_work->guide_transparency[view], 11);
		}
		trace->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
		trace->endEncoding();
		if (p_work->shadow_only) {
			continue;
		}
		if (!p_work->metalfx_denoiser) {
			MTL::ComputeCommandEncoder *filter = compute_encoder(timing_base + 2, timing_base + 3);
			filter->setComputePipelineState(p_work->filter_pipeline.get());
			filter->setBytes(&p_work->parameters[view], sizeof(MetalHybridParameters), 0);
			filter->setTexture(p_work->depth[view], 0);
			filter->setTexture(p_work->normal_roughness[view], 1);
			filter->setTexture(p_work->effect_output[view], 2);
			filter->setTexture(p_work->filtered_output[view], 3);
			filter->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
			filter->endEncoding();
		}
		if (p_work->temporal_enabled) {
			MTL::ComputeCommandEncoder *temporal = compute_encoder(timing_base + 4, timing_base + 5);
			temporal->setComputePipelineState(p_work->temporal_pipeline.get());
			temporal->setBytes(&p_work->parameters[view], sizeof(MetalHybridParameters), 0);
			temporal->setTexture(p_work->filtered_output[view], 0);
			temporal->setTexture(p_work->depth[view], 1);
			temporal->setTexture(p_work->normal_roughness[view], 2);
			temporal->setTexture(p_work->velocity[view], 3);
			temporal->setTexture(p_work->history_input[view], 4);
			temporal->setTexture(p_work->depth_history_input[view], 5);
			temporal->setTexture(p_work->normal_history_input[view], 6);
			temporal->setTexture(p_work->history_output[view], 7);
			temporal->setTexture(p_work->depth_history_output[view], 8);
			temporal->setTexture(p_work->normal_history_output[view], 9);
			temporal->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
			temporal->endEncoding();
		}
		MTL::ComputeCommandEncoder *composite = compute_encoder(timing_base + 6, timing_base + 7);
		composite->setComputePipelineState(p_work->composite_pipeline.get());
		composite->setTexture(p_work->metalfx_denoiser ? p_work->effect_output[view] : (p_work->temporal_enabled ? p_work->history_output[view] : p_work->filtered_output[view]), 0);
		composite->setTexture(p_work->color[view], 1);
		composite->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
		composite->endEncoding();
	}
	if (p_work->timing) {
		const std::shared_ptr<MetalHybridTimingCapture> timing = p_work->timing;
		MTL::Device *device = metal_driver->get_device();
		command_buffer->addCompletedHandler([timing, device](MTL::CommandBuffer *) {
			device->sampleTimestamps(&timing->cpu_end, &timing->gpu_end);
			timing->complete.store(true, std::memory_order_release);
		});
	}
	command->retain_resource(p_work->trace_pipeline.get());
	command->retain_resource(p_work->shadow_pipeline.get());
	command->retain_resource(p_work->filter_pipeline.get());
	command->retain_resource(p_work->temporal_pipeline.get());
	command->retain_resource(p_work->composite_pipeline.get());
	command->retain_resource(p_work->tlas.get());
	if (p_work->tlas_scratch) {
		command->retain_resource(p_work->tlas_scratch.get());
	}
	if (p_work->tlas_instances) {
		command->retain_resource(p_work->tlas_instances.get());
	}
	if (p_work->materials) {
		command->retain_resource(p_work->materials.get());
	}
	if (p_work->geometries) {
		command->retain_resource(p_work->geometries.get());
	}
	if (p_work->emissives) {
		command->retain_resource(p_work->emissives.get());
	}
	if (p_work->albedo_sampler) {
		command->retain_resource(p_work->albedo_sampler.get());
	}
	for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : p_work->blas) {
		command->retain_resource(blas.get());
	}
	for (const NS::SharedPtr<MTL::AccelerationStructure> &source : p_work->blas_sources) {
		if (source) {
			command->retain_resource(source.get());
		}
	}
	for (const NS::SharedPtr<MTL::Buffer> &scratch : p_work->blas_scratch) {
		if (scratch) {
			command->retain_resource(scratch.get());
		}
	}
	for (const NS::SharedPtr<MTL::Buffer> &transform : p_work->decode_transforms) {
		command->retain_resource(transform.get());
	}
	for (MTL::Buffer *buffer : p_work->vertex_buffers) {
		command->retain_resource(buffer);
	}
	for (MTL::Buffer *buffer : p_work->index_buffers) {
		if (buffer) {
			command->retain_resource(buffer);
		}
	}
	for (MTL::Buffer *buffer : p_work->attribute_buffers) {
		if (buffer) {
			command->retain_resource(buffer);
		}
	}
	for (MTL::Texture *texture : p_work->albedo_textures) {
		if (texture) {
			command->retain_resource(texture);
		}
	}
	memdelete(p_work);
}

MetalHybridEffect::MetalHybridEffect() {
	cache = memnew(MetalHybridEffectCache);
}

MetalHybridEffect::~MetalHybridEffect() {
	memdelete(cache);
}

bool MetalHybridEffect::is_supported() const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || rd->get_device_api_name() != "Metal") {
		return false;
	}
	RenderingDeviceDriverMetal *driver = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	return driver->get_device()->supportsRaytracing();
}

static Error _hybrid_fail(Error p_error, const String &p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static MTL::CounterSet *_timestamp_counter_set(MTL::Device *p_device) {
	NS::Array *sets = p_device->counterSets();
	for (NS::UInteger index = 0; index < sets->count(); index++) {
		MTL::CounterSet *set = static_cast<MTL::CounterSet *>(sets->object(index));
		if (set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
			return set;
		}
	}
	return nullptr;
}

Error MetalHybridEffect::render(const FrameRequest &p_request, FrameResult &r_result, String *r_error) {
	r_result = FrameResult();
	if (!is_supported()) {
		return _hybrid_fail(ERR_UNAVAILABLE, "The active Metal device does not support hybrid ray effects.", r_error);
	}
	if (p_request.surfaces.is_empty() || p_request.instances.is_empty() || p_request.views.is_empty()) {
		return _hybrid_fail(ERR_INVALID_PARAMETER, "Hybrid rendering requires geometry, instances, and at least one view.", r_error);
	}
	RenderingDevice *rd = RD::get_singleton();
	RenderingDeviceDriverMetal *rdd = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	MTL::Device *device = rdd->get_device();
	if (!cache->trace_pipeline) {
		NS::Error *compile_error = nullptr;
		NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(NS::String::string(HYBRID_MSL, NS::UTF8StringEncoding), nullptr, &compile_error));
		if (!library) {
			return _hybrid_fail(ERR_CANT_CREATE, compile_error ? String::utf8(compile_error->localizedDescription()->utf8String()) : "Metal hybrid shader compilation failed.", r_error);
		}
		NS::SharedPtr<MTL::Function> trace = NS::TransferPtr(library->newFunction(NS::String::string("trace_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> shadow = NS::TransferPtr(library->newFunction(NS::String::string("trace_hybrid_shadow", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> filter = NS::TransferPtr(library->newFunction(NS::String::string("filter_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> temporal = NS::TransferPtr(library->newFunction(NS::String::string("accumulate_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> composite = NS::TransferPtr(library->newFunction(NS::String::string("composite_hybrid", NS::UTF8StringEncoding)));
		cache->trace_pipeline = NS::TransferPtr(device->newComputePipelineState(trace.get(), &compile_error));
		cache->shadow_pipeline = NS::TransferPtr(device->newComputePipelineState(shadow.get(), &compile_error));
		cache->filter_pipeline = NS::TransferPtr(device->newComputePipelineState(filter.get(), &compile_error));
		cache->temporal_pipeline = NS::TransferPtr(device->newComputePipelineState(temporal.get(), &compile_error));
		cache->composite_pipeline = NS::TransferPtr(device->newComputePipelineState(composite.get(), &compile_error));
		if (!cache->trace_pipeline || !cache->shadow_pipeline || !cache->filter_pipeline || !cache->temporal_pipeline || !cache->composite_pipeline) {
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid pipelines could not be created.", r_error);
		}
		NS::SharedPtr<MTL::SamplerDescriptor> sampler_descriptor = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeRepeat);
		cache->albedo_sampler = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		if (!cache->albedo_sampler) {
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid albedo sampler could not be created.", r_error);
		}
	}
	cache->frame++;
	MetalHybridWork *work = memnew(MetalHybridWork);
	work->trace_pipeline = cache->trace_pipeline;
	work->shadow_pipeline = cache->shadow_pipeline;
	work->filter_pipeline = cache->filter_pipeline;
	work->temporal_pipeline = cache->temporal_pipeline;
	work->composite_pipeline = cache->composite_pipeline;
	work->albedo_sampler = cache->albedo_sampler;
	work->shadow_only = p_request.shadow_only;
	work->metalfx_denoiser = p_request.use_metalfx_denoiser && !p_request.shadow_only;
	Vector<RID> sampled_texture_resources;
	HashMap<uint64_t, uint32_t> surface_indices;
	for (uint32_t index = 0; index < (uint32_t)p_request.surfaces.size(); index++) {
		const Surface &surface = p_request.surfaces[index];
		if (!surface.vertex_buffer.is_valid() || surface.vertex_count < 3 || (surface.index_count && !surface.index_buffer.is_valid())) {
			continue;
		}
		MTL::Buffer *vertex = reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, surface.vertex_buffer));
		MTL::Buffer *indices = surface.index_buffer.is_valid() ? reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, surface.index_buffer)) : nullptr;
		MTL::Buffer *attributes = surface.has_uv && surface.attribute_buffer.is_valid() ? reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, surface.attribute_buffer)) : nullptr;
		if (!vertex) {
			continue;
		}
		work->vertex_buffers.push_back(vertex);
		work->index_buffers.push_back(indices);
		work->attribute_buffers.push_back(attributes);
		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> triangle = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		triangle->setVertexBuffer(vertex);
		triangle->setVertexFormat(surface.compressed ? MTL::AttributeFormatUShort4Normalized : MTL::AttributeFormatFloat3);
		triangle->setVertexStride(surface.vertex_stride);
		if (surface.compressed) {
			MTL::PackedFloat4x3 decode(MTL::PackedFloat3(surface.compressed_aabb.size.x, 0, 0), MTL::PackedFloat3(0, surface.compressed_aabb.size.y, 0), MTL::PackedFloat3(0, 0, surface.compressed_aabb.size.z), MTL::PackedFloat3(surface.compressed_aabb.position.x, surface.compressed_aabb.position.y, surface.compressed_aabb.position.z));
			NS::SharedPtr<MTL::Buffer> decode_buffer = NS::TransferPtr(device->newBuffer(&decode, sizeof(decode), MTL::ResourceStorageModeShared));
			triangle->setTransformationMatrixBuffer(decode_buffer.get());
			work->decode_transforms.push_back(decode_buffer);
		}
		if (indices) {
			triangle->setIndexBuffer(indices);
			triangle->setIndexType(surface.vertex_count <= 65536 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32);
		}
		triangle->setTriangleCount((surface.index_count ? surface.index_count : surface.vertex_count) / 3);
		triangle->setOpaque(true);
		NS::Object *object = triangle.get();
		NS::SharedPtr<NS::Array> array = NS::TransferPtr(NS::Array::array(&object, 1)->retain());
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
		descriptor->setGeometryDescriptors(array.get());
		descriptor->setUsage(surface.dynamic ? MTL::AccelerationStructureUsageRefit : MTL::AccelerationStructureUsagePreferFastIntersection);
		MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(descriptor.get());
		MetalHybridCachedGeometry **cached_ptr = cache->geometries.getptr(surface.stable_id);
		MetalHybridCachedGeometry *cached = cached_ptr ? *cached_ptr : nullptr;
		uint8_t action = 1;
		if (!cached) {
			cached = memnew(MetalHybridCachedGeometry);
			cache->geometries.insert(surface.stable_id, cached);
		} else if (cached->topology_revision == surface.topology_revision && cached->acceleration_structure) {
			action = cached->deformation_revision == surface.deformation_revision ? 0 : (surface.dynamic ? 2 : 1);
		}
		NS::SharedPtr<MTL::AccelerationStructure> source;
		if (action == 1) {
			source = cached->acceleration_structure;
			cached->acceleration_structure = NS::TransferPtr(device->newAccelerationStructure(sizes.accelerationStructureSize));
		} else if (action == 2) {
			// Metal explicitly supports an in-place refit. Keeping the resource identity
			// stable also means an unchanged instance hierarchy can reuse its TLAS.
			source = cached->acceleration_structure;
		}
		cached->topology_revision = surface.topology_revision;
		cached->deformation_revision = surface.deformation_revision;
		cached->last_seen_frame = cache->frame;
		work->blas_descriptors.push_back(descriptor);
		work->blas.push_back(cached->acceleration_structure);
		work->blas_sources.push_back(source);
		work->blas_actions.push_back(action);
		uint64_t scratch_size = action == 2 ? sizes.refitScratchBufferSize : sizes.buildScratchBufferSize;
		work->blas_scratch.push_back(action == 0 ? NS::SharedPtr<MTL::Buffer>() : NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), scratch_size), MTL::ResourceStorageModePrivate)));
		surface_indices.insert(surface.stable_id, work->blas.size() - 1);
		if (action == 1) {
			r_result.scene.blas_built++;
		} else if (action == 2) {
			r_result.scene.blas_refit++;
		} else {
			r_result.scene.blas_reused++;
		}
	}
	Vector<uint64_t> retired_geometries;
	for (const KeyValue<uint64_t, MetalHybridCachedGeometry *> &entry : cache->geometries) {
		if (entry.value->last_seen_frame == cache->frame) {
			continue;
		}
		if (entry.value->last_seen_frame + 3 <= cache->frame) {
			retired_geometries.push_back(entry.key);
		} else {
			r_result.scene.pending_retirements++;
		}
	}
	for (uint64_t stable_id : retired_geometries) {
		MetalHybridCachedGeometry **geometry = cache->geometries.getptr(stable_id);
		if (geometry) {
			memdelete(*geometry);
			cache->geometries.erase(stable_id);
			r_result.scene.retired++;
		}
	}
	Vector<MTL::AccelerationStructure *> blas_ptrs;
	for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : work->blas) {
		blas_ptrs.push_back(blas.get());
	}
	work->blas_array = NS::TransferPtr(NS::Array::array(reinterpret_cast<NS::Object *const *>(blas_ptrs.ptr()), blas_ptrs.size())->retain());
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> metal_instances;
	Vector<MetalHybridMaterial> metal_materials;
	Vector<MetalHybridGeometry> metal_geometries;
	Vector<MetalHybridEmissive> metal_emissives;
	HashMap<uint64_t, const Surface *> surface_records;
	for (const Surface &surface : p_request.surfaces) {
		surface_records.insert(surface.stable_id, &surface);
	}
	// The trace kernel has 12 fixed image bindings for the scene/guides. Keep a
	// deliberately small, capability-gated material texture table rather than
	// assuming bindless support or borrowing Forward+'s material uniform sets.
	const bool texture_bindings_supported = rd->limit_get(RD::LIMIT_MAX_TEXTURES_PER_SHADER_STAGE) >= 12u + HYBRID_MAX_ALBEDO_TEXTURES;
	const RID default_albedo_texture = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	MTL::Texture *default_albedo = default_albedo_texture.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, default_albedo_texture)) : nullptr;
	if (texture_bindings_supported && default_albedo) {
		work->albedo_textures.resize(HYBRID_MAX_ALBEDO_TEXTURES);
		for (MTL::Texture *&texture : work->albedo_textures) {
			texture = default_albedo;
		}
		sampled_texture_resources.push_back(default_albedo_texture);
	}
	HashMap<RID, uint32_t> albedo_texture_indices;
	uint32_t albedo_texture_count = 0;
	for (const Instance &instance : p_request.instances) {
		const uint32_t *surface_index = surface_indices.getptr(instance.surface_id);
		const Surface *const *surface_ptr = surface_records.getptr(instance.surface_id);
		if (!surface_index || !surface_ptr) {
			continue;
		}
		MTL::AccelerationStructureUserIDInstanceDescriptor native = {};
		const Vector3 x = instance.transform.basis.get_column(0);
		const Vector3 y = instance.transform.basis.get_column(1);
		const Vector3 z = instance.transform.basis.get_column(2);
		native.transformationMatrix = MTL::PackedFloat4x3(MTL::PackedFloat3(x.x, x.y, x.z), MTL::PackedFloat3(y.x, y.y, y.z), MTL::PackedFloat3(z.x, z.y, z.z), MTL::PackedFloat3(instance.transform.origin.x, instance.transform.origin.y, instance.transform.origin.z));
		native.options = MTL::AccelerationStructureInstanceOptionOpaque;
		native.mask = instance.visibility_mask & 0xff;
		native.accelerationStructureIndex = *surface_index;
		const uint32_t instance_id = metal_instances.size();
		native.userID = instance_id;
		metal_instances.push_back(native);
		MetalHybridMaterial material = {};
		material.albedo_metallic = simd_make_float4(instance.albedo.r, instance.albedo.g, instance.albedo.b, instance.metallic);
		material.emission_roughness = simd_make_float4(instance.emission.r, instance.emission.g, instance.emission.b, instance.roughness);
		const Surface &surface = **surface_ptr;
		if (instance.albedo_texture.is_valid()) {
			const bool has_uv_source = surface.has_uv && *surface_index < uint32_t(work->attribute_buffers.size()) && work->attribute_buffers[*surface_index];
			const uint32_t *existing_texture_index = albedo_texture_indices.getptr(instance.albedo_texture);
			if (has_uv_source && existing_texture_index) {
				material.albedo_texture_index = *existing_texture_index;
				r_result.textured_materials++;
			} else if (texture_bindings_supported && has_uv_source && albedo_texture_count < HYBRID_MAX_ALBEDO_TEXTURES) {
				MTL::Texture *texture = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, instance.albedo_texture));
				if (texture && texture->textureType() == MTL::TextureType2D && texture->sampleCount() == 1) {
					const uint32_t texture_index = albedo_texture_count++;
					work->albedo_textures.write[texture_index] = texture;
					albedo_texture_indices.insert(instance.albedo_texture, texture_index);
					material.albedo_texture_index = texture_index;
					sampled_texture_resources.push_back(instance.albedo_texture);
					r_result.textured_materials++;
				} else {
					r_result.texture_fallbacks++;
				}
			} else {
				r_result.texture_fallbacks++;
			}
		}
		metal_materials.push_back(material);
		MetalHybridGeometry geometry = {};
		geometry.vertex_address = work->vertex_buffers[*surface_index]->gpuAddress();
		geometry.index_address = work->index_buffers[*surface_index] ? work->index_buffers[*surface_index]->gpuAddress() : 0;
		geometry.attribute_address = surface.has_uv && work->attribute_buffers[*surface_index] ? work->attribute_buffers[*surface_index]->gpuAddress() : 0;
		geometry.vertex_count = surface.vertex_count;
		geometry.index_type = surface.vertex_count <= 65536 ? 16 : 32;
		geometry.position_stride = surface.vertex_stride;
		geometry.normal_offset = surface.normal_offset;
		geometry.normal_stride = surface.normal_stride;
		geometry.has_normals = surface.has_normals ? 1u : 0u;
		geometry.compressed = surface.compressed ? 1u : 0u;
		geometry.attribute_stride = surface.attribute_stride;
		geometry.uv_offset = surface.uv_offset;
		geometry.has_uv = geometry.attribute_address != 0 ? 1u : 0u;
		const Basis normal_basis = instance.transform.basis.inverse().transposed();
		geometry.normal_from_object = _metal_matrix(Transform3D(normal_basis, Vector3()));
		geometry.world_from_object = _metal_matrix(instance.transform);
		geometry.position_scale = simd_make_float4(surface.compressed_aabb.size.x, surface.compressed_aabb.size.y, surface.compressed_aabb.size.z, 0.0f);
		geometry.position_offset = simd_make_float4(surface.compressed_aabb.position.x, surface.compressed_aabb.position.y, surface.compressed_aabb.position.z, 0.0f);
		metal_geometries.push_back(geometry);
		if (instance.emission.r > 0.0001f || instance.emission.g > 0.0001f || instance.emission.b > 0.0001f) {
			MetalHybridEmissive emitter;
			emitter.instance_id = instance_id;
			emitter.triangle_count = (surface.index_count ? surface.index_count : surface.vertex_count) / 3;
			if (emitter.triangle_count > 0) {
				metal_emissives.push_back(emitter);
			}
		}
	}
	if (work->blas.is_empty() || metal_instances.is_empty()) {
		memdelete(work);
		return _hybrid_fail(ERR_INVALID_DATA, "No supported triangle surfaces were available to the Metal hybrid renderer.", r_error);
	}
	bool tlas_dirty = !cache->tlas || cache->tlas_blas_order.size() != blas_ptrs.size() || cache->tlas_instances.size() != metal_instances.size();
	if (!tlas_dirty && !blas_ptrs.is_empty()) {
		tlas_dirty = memcmp(cache->tlas_blas_order.ptr(), blas_ptrs.ptr(), blas_ptrs.size() * sizeof(MTL::AccelerationStructure *)) != 0;
	}
	if (!tlas_dirty && !metal_instances.is_empty()) {
		tlas_dirty = memcmp(cache->tlas_instances.ptr(), metal_instances.ptr(), metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor)) != 0;
	}
	if (tlas_dirty) {
		work->tlas_instances = NS::TransferPtr(device->newBuffer(metal_instances.ptr(), metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor), MTL::ResourceStorageModeShared));
		work->tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		work->tlas_descriptor->setInstancedAccelerationStructures(work->blas_array.get());
		work->tlas_descriptor->setInstanceDescriptorBuffer(work->tlas_instances.get());
		work->tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
		work->tlas_descriptor->setInstanceCount(metal_instances.size());
		MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(work->tlas_descriptor.get());
		cache->tlas = NS::TransferPtr(device->newAccelerationStructure(tlas_sizes.accelerationStructureSize));
		work->tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), tlas_sizes.buildScratchBufferSize), MTL::ResourceStorageModePrivate));
		cache->tlas_blas_order = blas_ptrs;
		cache->tlas_instances = metal_instances;
		work->tlas_build = true;
		r_result.scene.tlas_updated = 1;
	}
	work->tlas = cache->tlas;
	const uint32_t emissive_count = metal_emissives.size();
	if (metal_emissives.is_empty()) {
		metal_emissives.push_back(MetalHybridEmissive());
	}
	work->materials = NS::TransferPtr(device->newBuffer(metal_materials.ptr(), metal_materials.size() * sizeof(MetalHybridMaterial), MTL::ResourceStorageModeShared));
	work->geometries = NS::TransferPtr(device->newBuffer(metal_geometries.ptr(), metal_geometries.size() * sizeof(MetalHybridGeometry), MTL::ResourceStorageModeShared));
	work->emissives = NS::TransferPtr(device->newBuffer(metal_emissives.ptr(), metal_emissives.size() * sizeof(MetalHybridEmissive), MTL::ResourceStorageModeShared));
	Vector<RD::CallbackResource> resources;
	for (const Surface &surface : p_request.surfaces) {
		if (surface.vertex_buffer.is_valid()) {
			resources.push_back({ .rid = surface.vertex_buffer, .type = RD::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RD::CALLBACK_RESOURCE_USAGE_VERTEX_BUFFER_READ });
		}
		if (surface.index_buffer.is_valid()) {
			resources.push_back({ .rid = surface.index_buffer, .type = RD::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RD::CALLBACK_RESOURCE_USAGE_INDEX_BUFFER_READ });
		}
		if (surface.has_uv && surface.attribute_buffer.is_valid()) {
			resources.push_back({ .rid = surface.attribute_buffer, .type = RD::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RD::CALLBACK_RESOURCE_USAGE_VERTEX_BUFFER_READ });
		}
	}
	for (const RID &texture : sampled_texture_resources) {
		resources.push_back({ .rid = texture, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
	}
	for (const View &view : p_request.views) {
		MTL::Texture *color = view.color.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.color)) : nullptr;
		MTL::Texture *depth = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.depth));
		MTL::Texture *normal = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.normal_roughness));
		MTL::Texture *effect_output = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.effect_output));
		MTL::Texture *filtered_output = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.filtered_output));
		MTL::Texture *velocity = view.velocity.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.velocity)) : nullptr;
		MTL::Texture *history_input = view.history_input.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.history_input)) : nullptr;
		MTL::Texture *history_output = view.history_output.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.history_output)) : nullptr;
		MTL::Texture *depth_history_input = view.depth_history_input.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.depth_history_input)) : nullptr;
		MTL::Texture *depth_history_output = view.depth_history_output.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.depth_history_output)) : nullptr;
		MTL::Texture *normal_history_input = view.normal_history_input.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.normal_history_input)) : nullptr;
		MTL::Texture *normal_history_output = view.normal_history_output.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.normal_history_output)) : nullptr;
		MTL::Texture *guide_normal = view.guide_normal.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_normal)) : nullptr;
		MTL::Texture *guide_diffuse = view.guide_diffuse.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_diffuse)) : nullptr;
		MTL::Texture *guide_specular = view.guide_specular.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_specular)) : nullptr;
		MTL::Texture *guide_roughness = view.guide_roughness.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_roughness)) : nullptr;
		MTL::Texture *guide_denoise_strength = view.guide_denoise_strength.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_denoise_strength)) : nullptr;
		MTL::Texture *guide_reactive = view.guide_reactive.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_reactive)) : nullptr;
		MTL::Texture *guide_specular_distance = view.guide_specular_distance.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_specular_distance)) : nullptr;
		MTL::Texture *guide_transparency = view.guide_transparency.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_transparency)) : nullptr;
		const bool any_temporal = velocity || history_input || history_output || depth_history_input || depth_history_output || normal_history_input || normal_history_output;
		const bool all_temporal = velocity && history_input && history_output && depth_history_input && depth_history_output && normal_history_input && normal_history_output;
		const bool complete_guides = guide_normal && guide_diffuse && guide_specular && guide_roughness && guide_denoise_strength && guide_reactive && guide_specular_distance && guide_transparency;
		if ((!p_request.shadow_only && (!color || !filtered_output || (work->metalfx_denoiser && !complete_guides))) || (any_temporal && !all_temporal) || !depth || !normal || !effect_output) {
			memdelete(work);
			return _hybrid_fail(ERR_INVALID_PARAMETER, "A hybrid view texture or MetalFX guide is invalid.", r_error);
		}
		work->color.push_back(color);
		work->depth.push_back(depth);
		work->normal_roughness.push_back(normal);
		work->effect_output.push_back(effect_output);
		work->filtered_output.push_back(filtered_output);
		work->velocity.push_back(velocity);
		work->history_input.push_back(history_input);
		work->history_output.push_back(history_output);
		work->depth_history_input.push_back(depth_history_input);
		work->depth_history_output.push_back(depth_history_output);
		work->normal_history_input.push_back(normal_history_input);
		work->normal_history_output.push_back(normal_history_output);
		work->guide_normal.push_back(guide_normal);
		work->guide_diffuse.push_back(guide_diffuse);
		work->guide_specular.push_back(guide_specular);
		work->guide_roughness.push_back(guide_roughness);
		work->guide_denoise_strength.push_back(guide_denoise_strength);
		work->guide_reactive.push_back(guide_reactive);
		work->guide_specular_distance.push_back(guide_specular_distance);
		work->guide_transparency.push_back(guide_transparency);
		work->temporal_enabled |= all_temporal && !work->metalfx_denoiser;
		MetalHybridParameters parameters = {};
		parameters.world_from_view = _metal_matrix(view.world_from_view);
		parameters.view_from_clip = simd_inverse(_metal_matrix(view.clip_from_view));
		parameters.clip_from_world = simd_mul(_metal_matrix(view.clip_from_view), simd_inverse(parameters.world_from_view));
		parameters.prev_clip_from_world = _metal_matrix(view.prev_clip_from_world);
		parameters.light_direction_and_reflection_strength = simd_make_float4(p_request.directional_light_direction.x, p_request.directional_light_direction.y, p_request.directional_light_direction.z, p_request.reflection_strength);
		parameters.ao_distance_strength_roughness_flags = simd_make_float4(p_request.ambient_occlusion_distance, p_request.ambient_occlusion_strength, p_request.reflection_roughness_cutoff, float((p_request.reflections ? 1u : 0u) | (p_request.ambient_occlusion ? 2u : 0u)));
		parameters.ao_distance_strength_roughness_flags.w = float((p_request.reflections ? 1u : 0u) | (p_request.ambient_occlusion ? 2u : 0u) | (p_request.global_illumination ? 4u : 0u));
		parameters.dimensions = p_request.shadow_only ? simd_make_uint2(effect_output->width(), effect_output->height()) : simd_make_uint2(color->width(), color->height());
		parameters.shadow_sample_count = MAX(1u, p_request.shadow_sample_count);
		parameters.directional_light_angular_radius = p_request.directional_light_angular_radius;
		parameters.gi_sample_count = MAX(1u, p_request.global_illumination_samples);
		parameters.frame_index = p_request.frame_index;
		parameters.gi_strength = p_request.global_illumination_strength;
		parameters.history_valid = p_request.history_valid ? 1u : 0u;
		parameters.emissive_count = emissive_count;
		work->parameters.push_back(parameters);
		if (view.color.is_valid()) {
			resources.push_back({ .rid = view.color, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		resources.push_back({ .rid = view.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		resources.push_back({ .rid = view.normal_roughness, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		resources.push_back({ .rid = view.effect_output, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		if (view.filtered_output.is_valid()) {
			resources.push_back({ .rid = view.filtered_output, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		const RID guide_resources[] = { view.guide_normal, view.guide_diffuse, view.guide_specular, view.guide_roughness, view.guide_denoise_strength, view.guide_reactive, view.guide_specular_distance, view.guide_transparency };
		for (const RID &resource : guide_resources) {
			if (resource.is_valid()) {
				resources.push_back({ .rid = resource, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
			}
		}
		const RID temporal_resources[] = { view.velocity, view.history_input, view.history_output, view.depth_history_input, view.depth_history_output, view.normal_history_input, view.normal_history_output };
		for (const RID &resource : temporal_resources) {
			if (resource.is_valid()) {
				resources.push_back({ .rid = resource, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
			}
		}
	}
	if (p_request.collect_gpu_timings && cache->timing_captures.is_empty() && device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary)) {
		MTL::CounterSet *timestamp_set = _timestamp_counter_set(device);
		if (timestamp_set) {
			NS::SharedPtr<MTL::CounterSampleBufferDescriptor> descriptor = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
			descriptor->setCounterSet(timestamp_set);
			descriptor->setSampleCount(4 + p_request.views.size() * 8);
			descriptor->setStorageMode(MTL::StorageModeShared);
			NS::Error *counter_error = nullptr;
			std::shared_ptr<MetalHybridTimingCapture> timing = std::make_shared<MetalHybridTimingCapture>();
			timing->samples = NS::TransferPtr(device->newCounterSampleBuffer(descriptor.get(), &counter_error));
			timing->view_count = p_request.views.size();
			timing->shadow_only = p_request.shadow_only;
			if (timing->samples) {
				work->timing = timing;
				cache->timing_captures.push_back(timing);
			}
		}
	}
	rd->capture_timestamp("Hybrid BLAS TLAS Ray Effects Begin");
	Error callback_error = rd->driver_callback_add((RDD::DriverCallback)_hybrid_callback, work, VectorView<RD::CallbackResource>(resources.ptr(), resources.size()));
	rd->capture_timestamp("Hybrid BLAS TLAS Ray Effects End");
	if (callback_error != OK) {
		memdelete(work);
		return _hybrid_fail(callback_error, "Metal hybrid work could not be scheduled.", r_error);
	}
	r_result.rendered_views = p_request.views.size();
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error MetalHybridEffect::collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error) {
	r_timings.clear();
	for (int capture_index = cache->timing_captures.size() - 1; capture_index >= 0; capture_index--) {
		const std::shared_ptr<MetalHybridTimingCapture> &capture = cache->timing_captures[capture_index];
		if (!capture->complete.load(std::memory_order_acquire)) {
			continue;
		}
		const uint32_t sample_count = 4 + capture->view_count * 8;
		NS::Data *data = capture->samples->resolveCounterRange(NS::Range::Make(0, sample_count));
		if (data && capture->gpu_end > capture->gpu_begin && capture->cpu_end > capture->cpu_begin) {
			const MTL::CounterResultTimestamp *samples = static_cast<const MTL::CounterResultTimestamp *>(data->bytes());
			mach_timebase_info_data_t timebase = {};
			mach_timebase_info(&timebase);
			const double nanoseconds_per_gpu_tick = (double(capture->cpu_end - capture->cpu_begin) * double(timebase.numer) / double(timebase.denom)) / double(capture->gpu_end - capture->gpu_begin);
			auto append_timing = [&](const StringName &p_stage, uint64_t p_begin, uint64_t p_end) {
				if (p_end > p_begin) {
					r_timings.push_back({ p_stage, double(p_end - p_begin) * nanoseconds_per_gpu_tick / 1000000.0 });
				}
			};
			append_timing("blas", samples[0].timestamp, samples[1].timestamp);
			append_timing("tlas", samples[2].timestamp, samples[3].timestamp);
			double trace_ms = 0.0;
			double reconstruction_ms = 0.0;
			double temporal_ms = 0.0;
			double composition_ms = 0.0;
			for (uint32_t view = 0; view < capture->view_count; view++) {
				const uint32_t base = 4 + view * 8;
				trace_ms += double(samples[base + 1].timestamp - samples[base].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
				reconstruction_ms += double(samples[base + 3].timestamp - samples[base + 2].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
				temporal_ms += double(samples[base + 5].timestamp - samples[base + 4].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
				composition_ms += double(samples[base + 7].timestamp - samples[base + 6].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
			}
			r_timings.push_back({ capture->shadow_only ? StringName("ray_shadows") : StringName("ray_effects"), trace_ms });
			if (!capture->shadow_only) {
				r_timings.push_back({ "spatial_reconstruction", reconstruction_ms });
				r_timings.push_back({ "temporal_reconstruction", temporal_ms });
				r_timings.push_back({ "composition", composition_ms });
			}
		}
		cache->timing_captures.remove_at(capture_index);
	}
	if (r_timings.is_empty()) {
		return _hybrid_fail(ERR_BUSY, "No completed Metal hybrid GPU counter capture is available.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

} // namespace RendererRD

#endif
