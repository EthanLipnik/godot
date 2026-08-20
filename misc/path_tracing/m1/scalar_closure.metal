#include <metal_stdlib>
using namespace metal;

struct ClosureInput {
	float4 base_color_metallic;
	float4 f0_roughness;
	float4 normal_unused;
	float4 light_view;
};

kernel void evaluate_scalar_closure(device const ClosureInput *inputs [[buffer(0)]], device float4 *outputs [[buffer(1)]], uint index [[thread_position_in_grid]]) {
	ClosureInput input = inputs[index];
	float3 normal = normalize(input.normal_unused.xyz);
	float3 light = normalize(input.light_view.xyz);
	float3 view = normalize(float3(input.light_view.w, 0.0f, 1.0f));
	float cosine = max(dot(normal, light), 0.0f);
	float3 half_direction = normalize(light + view);
	float exponent = mix(256.0f, 2.0f, input.f0_roughness.w * input.f0_roughness.w);
	float specular = pow(max(dot(normal, half_direction), 0.0f), exponent) * (exponent + 2.0f) * 0.15915494309f;
	float3 value = input.base_color_metallic.xyz * (1.0f - input.base_color_metallic.w) * 0.31830988618f + input.f0_roughness.xyz * specular;
	outputs[index] = float4(value * cosine, 1.0f);
}
