#version 460

struct ClosureInput {
	vec4 base_color_metallic;
	vec4 f0_roughness;
	vec4 normal_unused;
	vec4 light_view;
};

layout(local_size_x = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer Inputs { ClosureInput inputs[]; };
layout(std430, set = 0, binding = 1) writeonly buffer Outputs { vec4 outputs[]; };

void main() {
	uint index = gl_GlobalInvocationID.x;
	ClosureInput input_value = inputs[index];
	vec3 normal = normalize(input_value.normal_unused.xyz);
	vec3 light = normalize(input_value.light_view.xyz);
	vec3 view = normalize(vec3(input_value.light_view.w, 0.0, 1.0));
	float cosine = max(dot(normal, light), 0.0);
	vec3 half_direction = normalize(light + view);
	float exponent = mix(256.0, 2.0, input_value.f0_roughness.w * input_value.f0_roughness.w);
	float specular = pow(max(dot(normal, half_direction), 0.0), exponent) * (exponent + 2.0) * 0.15915494309;
	vec3 value = input_value.base_color_metallic.xyz * (1.0 - input_value.base_color_metallic.w) * 0.31830988618 + input_value.f0_roughness.xyz * specular;
	outputs[index] = vec4(value * cosine, 1.0);
}
