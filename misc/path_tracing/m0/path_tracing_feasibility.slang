struct PacketFloat4 {
	float x;
	float y;
	float z;
	float w;
};

struct PacketMatrix4 {
	PacketFloat4 columns[4];
};

struct PacketGuideContract {
	uint schemaVersion;
	uint motionDirection;
	uint motionUnits;
	uint depthConvention;
	uint normalSpace;
	uint roughnessConvention;
	uint invalidPixelConvention;
	uint uvOrigin;
	uint colorSpace;
	uint enabledGuides;
	float motionToPixelScaleX;
	float motionToPixelScaleY;
	uint reserved[4];
};

struct ClosureInput {
	float3 baseColor;
	float perceptualRoughness;
	float metallic;
	float3 normal;
};

struct ProbeOutput {
	uint float4Size;
	uint matrixSize;
	uint guideSize;
	float3 closureValue;
	float padding;
};

[[vk::binding(0, 0)]]
StructuredBuffer<ClosureInput> inputs;

[[vk::binding(1, 0)]]
RWStructuredBuffer<ProbeOutput> outputs;

float3 evaluateBoundedClosure(ClosureInput input, float3 lightDirection) {
	float3 normal = normalize(input.normal);
	float cosine = max(dot(normal, normalize(lightDirection)), 0.0);
	float3 diffuse = input.baseColor * (1.0 - input.metallic) * cosine;
	float3 f0 = lerp(float3(0.04), input.baseColor, input.metallic);
	float gloss = pow(cosine, lerp(64.0, 2.0, input.perceptualRoughness));
	return diffuse + f0 * gloss;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint index = dispatchThreadID.x;
	ProbeOutput result;
	result.float4Size = sizeof(PacketFloat4);
	result.matrixSize = sizeof(PacketMatrix4);
	result.guideSize = sizeof(PacketGuideContract);
	result.closureValue = evaluateBoundedClosure(inputs[index], float3(0.3, 0.8, 0.5));
	result.padding = 0.0;
	outputs[index] = result;
}
