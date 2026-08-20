#include <metal_raytracing>
#include <metal_stdlib>

using namespace metal;

struct PathMaterial {
	float4 base_color;
	float4 emission;
	float4 parameters; // x: roughness, y: metallic, z: specular, w: reserved.
};

struct FrameParameters {
	float4 camera_position;
	float4 camera_forward_and_tan_half_fov;
	float4 camera_right;
	float4 camera_up;
	uint4 dimensions_seed;
	float4 dynamic_motion_and_light_area;
};

static uint hash_uint(uint value) {
	value ^= value >> 16;
	value *= 0x7feb352d;
	value ^= value >> 15;
	value *= 0x846ca68b;
	return value ^ (value >> 16);
}

static float random_float(thread uint &state) {
	state = hash_uint(state);
	return float(state & 0x00ffffffu) / float(0x01000000u);
}

static float3 cosine_direction(float3 normal, thread uint &state) {
	const float u1 = random_float(state);
	const float u2 = random_float(state);
	const float radius = sqrt(u1);
	const float angle = 6.28318530718f * u2;
	const float3 helper = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	const float3 tangent = normalize(cross(helper, normal));
	const float3 bitangent = cross(normal, tangent);
	return normalize(tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)) + normal * sqrt(max(0.0f, 1.0f - u1)));
}

kernel void trace_two_bounce_scene(
		raytracing::instance_acceleration_structure acceleration_structure [[buffer(0)]],
		device const packed_float3 *vertices [[buffer(1)]],
		device const uint *primitive_materials [[buffer(2)]],
		device const PathMaterial *materials [[buffer(3)]],
		constant FrameParameters &parameters [[buffer(4)]],
		device float4 *radiance_output [[buffer(5)]],
		device float *depth_output [[buffer(6)]],
		device float2 *motion_output [[buffer(7)]],
		device float4 *normal_output [[buffer(8)]],
		device float4 *diffuse_output [[buffer(9)]],
		device float4 *specular_output [[buffer(10)]],
		device float *roughness_output [[buffer(11)]],
		device float *specular_hit_distance_output [[buffer(12)]],
		device uint *reflection_diagnostic [[buffer(13)]],
		uint2 pixel [[thread_position_in_grid]]) {
	const uint width = parameters.dimensions_seed.x;
	const uint height = parameters.dimensions_seed.y;
	if (pixel.x >= width || pixel.y >= height) {
		return;
	}
	const uint output_index = pixel.y * width + pixel.x;
	uint random_state = hash_uint(output_index ^ parameters.dimensions_seed.z);
	const float2 jitter = float2(random_float(random_state), random_float(random_state)) - 0.5f;
	const float2 uv = (float2(pixel) + 0.5f + jitter) / float2(width, height);
	const float aspect = float(width) / float(height);
	const float2 screen = float2((uv.x * 2.0f - 1.0f) * aspect, 1.0f - uv.y * 2.0f);
	float3 ray_origin = parameters.camera_position.xyz;
	float3 ray_direction = normalize(parameters.camera_forward_and_tan_half_fov.xyz +
			parameters.camera_right.xyz * screen.x * parameters.camera_forward_and_tan_half_fov.w +
			parameters.camera_up.xyz * screen.y * parameters.camera_forward_and_tan_half_fov.w);
	float3 radiance = float3(0.0f);
	float3 throughput = float3(1.0f);
	bool primary_was_mirror = false;
	bool reflected_dynamic = false;
	const float invalid_guide = as_type<float>(0x7fc00000u);
	float primary_depth = invalid_guide;
	float3 primary_normal = float3(invalid_guide);
	float3 primary_diffuse = float3(0.0f);
	float3 primary_specular = float3(0.0f);
	float primary_roughness = invalid_guide;
	float primary_specular_distance = invalid_guide;
	float2 primary_motion = float2(0.0f);

	raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
	intersector.assume_geometry_type(raytracing::geometry_type::triangle);

	for (uint bounce = 0; bounce < 2; bounce++) {
		raytracing::ray ray;
		ray.origin = ray_origin;
		ray.direction = ray_direction;
		ray.min_distance = 0.001f;
		ray.max_distance = 100.0f;
		auto hit = intersector.intersect(ray, acceleration_structure, 0xff);
		if (hit.type != raytracing::intersection_type::triangle) {
			const float sky = 0.5f * (ray_direction.y + 1.0f);
			radiance += throughput * mix(float3(0.015f, 0.02f, 0.03f), float3(0.12f, 0.18f, 0.28f), sky);
			break;
		}

		const uint primitive = hit.primitive_id;
		const uint material_index = primitive_materials[primitive];
		const PathMaterial material = materials[material_index];
		const float3 p0 = float3(vertices[primitive * 3 + 0]);
		const float3 p1 = float3(vertices[primitive * 3 + 1]);
		const float3 p2 = float3(vertices[primitive * 3 + 2]);
		float3 normal = normalize(cross(p1 - p0, p2 - p0));
		if (dot(normal, -ray_direction) < 0.0f) {
			normal = -normal;
		}
		const float3 hit_position = ray_origin + ray_direction * hit.distance;
		if (bounce == 0) {
			primary_depth = 1.0f / (1.0f + hit.distance);
			primary_normal = normal;
			primary_diffuse = material.base_color.rgb * (1.0f - material.parameters.y);
			primary_specular = mix(float3(0.04f), material.base_color.rgb, material.parameters.y);
			primary_roughness = material.parameters.x;
			primary_was_mirror = material_index == 5;
			if (material_index == 6) {
				primary_motion = parameters.dynamic_motion_and_light_area.xy;
			}
		} else if (primary_was_mirror && material_index == 6) {
			reflected_dynamic = true;
			primary_specular_distance = hit.distance;
		}

		if (any(material.emission.rgb > float3(0.0f))) {
			radiance += throughput * material.emission.rgb * material.emission.w;
			break;
		}

		if (material.parameters.x < 0.05f && material.parameters.z > 0.5f) {
			throughput *= mix(float3(material.parameters.z), material.base_color.rgb, material.parameters.y);
			ray_origin = hit_position + normal * 0.002f;
			ray_direction = normalize(reflect(ray_direction, normal));
			continue;
		}

		const float3 light_position = float3(0.0f, 1.85f, -2.25f);
		const float3 to_light = light_position - hit_position;
		const float light_distance = length(to_light);
		const float3 light_direction = to_light / light_distance;
		const float cosine = max(0.0f, dot(normal, light_direction));
		if (cosine > 0.0f) {
			raytracing::ray shadow_ray;
			shadow_ray.origin = hit_position + normal * 0.002f;
			shadow_ray.direction = light_direction;
			shadow_ray.min_distance = 0.001f;
			shadow_ray.max_distance = max(0.001f, light_distance - 0.03f);
			auto shadow_hit = intersector.intersect(shadow_ray, acceleration_structure, 0xff);
			if (shadow_hit.type == raytracing::intersection_type::none) {
				const float attenuation = parameters.dynamic_motion_and_light_area.w / max(0.25f, light_distance * light_distance);
				radiance += throughput * material.base_color.rgb * float3(14.0f, 12.0f, 10.0f) * cosine * attenuation;
			}
		}
		throughput *= material.base_color.rgb;
		ray_origin = hit_position + normal * 0.002f;
		ray_direction = cosine_direction(normal, random_state);
	}

	radiance_output[output_index] = float4(radiance, 1.0f);
	depth_output[output_index] = primary_depth;
	motion_output[output_index] = primary_motion;
	normal_output[output_index] = float4(primary_normal, 0.0f);
	diffuse_output[output_index] = float4(primary_diffuse, 1.0f);
	specular_output[output_index] = float4(primary_specular, 1.0f);
	roughness_output[output_index] = primary_roughness;
	specular_hit_distance_output[output_index] = primary_specular_distance;
	reflection_diagnostic[output_index] = reflected_dynamic ? 1u : 0u;
}
