// Generic dynamic-geometry validation kernels for Godot path tracing.
#include <metal_raytracing>
#include <metal_stdlib>

using namespace metal;

struct DeformationParameters {
	float morph_weight;
};

kernel void deform_triangle(
		device const packed_float3 *base_positions [[buffer(0)]],
		device const packed_float3 *morph_deltas [[buffer(1)]],
		device const ushort *joint_indices [[buffer(2)]],
		device const float *joint_weights [[buffer(3)]],
		device const float4x4 *bone_matrices [[buffer(4)]],
		constant DeformationParameters &parameters [[buffer(5)]],
		device packed_float3 *deformed_positions [[buffer(6)]],
		uint vertex_index [[thread_position_in_grid]]) {
	if (vertex_index >= 3) {
		return;
	}

	float3 local_position = float3(base_positions[vertex_index]) +
			float3(morph_deltas[vertex_index]) * parameters.morph_weight;
	float4 skinned_position = float4(0.0);
	for (uint influence = 0; influence < 8; influence++) {
		uint influence_index = vertex_index * 8 + influence;
		uint joint = joint_indices[influence_index];
		float weight = joint_weights[influence_index];
		skinned_position += (bone_matrices[joint] * float4(local_position, 1.0)) * weight;
	}
	deformed_positions[vertex_index] = packed_float3(skinned_position.xyz);
}

kernel void trace_triangle(
		raytracing::instance_acceleration_structure acceleration_structure [[buffer(0)]],
		device uint *result [[buffer(1)]],
		uint index [[thread_position_in_grid]]) {
	if (index != 0) {
		return;
	}

	raytracing::ray ray;
	ray.origin = float3(0.0, 0.0, 1.0);
	ray.direction = float3(0.0, 0.0, -1.0);
	ray.min_distance = 0.001;
	ray.max_distance = 10.0;

	raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
	intersector.assume_geometry_type(raytracing::geometry_type::triangle);
	auto intersection = intersector.intersect(ray, acceleration_structure, 0xff);
	result[0] = intersection.type == raytracing::intersection_type::triangle ? 1u : 0u;
}
