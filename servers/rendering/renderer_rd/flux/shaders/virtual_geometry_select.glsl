#[compute]

#version 450

#VERSION_DEFINES

// Compute/indirect reference selector. This intentionally emits ordinary
// indexed indirect commands; native mesh/object stages are not represented by
// this shader or claimed by the capability contract.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct CandidateRecord {
	// indexCount, firstIndex, baseVertex bits, Flux instance-data index.
	uvec4 draw;
	// Descriptor table index, published descriptor generation, per-eye mask,
	// and the canonical heap-schema revision. The latter two are retained for
	// diagnostics/capture even though raster hardware clips each eye itself.
	uvec4 metadata;
};

struct IndexedIndirectCommand {
	uint index_count;
	uint instance_count;
	uint first_index;
	int vertex_offset;
	uint first_instance;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Candidates { CandidateRecord data[]; } candidates;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Commands {
	IndexedIndirectCommand data[];
} commands;
layout(set = 0, binding = 2, std430) restrict buffer Counters {
	uint selected_count;
	uint overflow_count;
} counters;
// The published descriptor generation prevents a freshly recycled page range
// from becoming visible through an old command stream.
layout(set = 0, binding = 3, std430) restrict readonly buffer Descriptors { uint data[]; } descriptors;

layout(push_constant, std430) uniform Params {
	uint candidate_count;
	uint command_capacity;
	uint descriptor_generation;
	uint eye_index;
} params;

void main() {
	uint candidate = gl_GlobalInvocationID.x;
	if (candidate >= params.candidate_count) {
		return;
	}
	// The draw call uses the fixed candidate extent. Always clear the matching
	// command slot before validation so a rejected record can never replay a
	// command left by an older submission.
	commands.data[candidate].index_count = 0u;
	commands.data[candidate].instance_count = 0u;
	commands.data[candidate].first_index = 0u;
	commands.data[candidate].vertex_offset = 0;
	commands.data[candidate].first_instance = 0u;
	CandidateRecord candidate_record = candidates.data[candidate];
	// VirtualGeometryGPUClusterDescriptor is 16 uints. Its generation is at
	// uint offset 10 (40 bytes), independent of backend packing rules.
	if (candidate_record.metadata.y != params.descriptor_generation || descriptors.data[candidate_record.metadata.x * 16u + 10u] != params.descriptor_generation) {
		return;
	}
	if (candidate >= params.command_capacity) {
		atomicAdd(counters.overflow_count, 1u);
		return;
	}
	atomicAdd(counters.selected_count, 1u);
	commands.data[candidate].index_count = candidate_record.draw.x;
	commands.data[candidate].instance_count = 1u;
	commands.data[candidate].first_index = candidate_record.draw.y;
	commands.data[candidate].vertex_offset = int(candidate_record.draw.z);
	commands.data[candidate].first_instance = candidate_record.draw.w;
}
