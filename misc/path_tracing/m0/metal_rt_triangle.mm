// Standalone native Metal acceleration-structure and deformation spike.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include <algorithm>
#include <cstdio>

struct PackedFloat3 {
	float x;
	float y;
	float z;
};

static void fail(NSString *message) {
	fprintf(stderr, "%s\n", message.UTF8String);
	exit(1);
}

static double gpu_milliseconds(id<MTLCommandBuffer> command_buffer) {
	double elapsed = command_buffer.GPUEndTime - command_buffer.GPUStartTime;
	return elapsed > 0.0 ? elapsed * 1000.0 : 0.0;
}

static id<MTLCommandBuffer> commit_and_wait(id<MTLCommandBuffer> command_buffer, NSString *stage) {
	[command_buffer commit];
	[command_buffer waitUntilCompleted];
	if (command_buffer.status != MTLCommandBufferStatusCompleted) {
		fail([NSString stringWithFormat:@"%@: %@", stage, command_buffer.error.localizedDescription ?: @"command failed"]);
	}
	return command_buffer;
}

static id<MTLCommandBuffer> build_acceleration_structure(
		id<MTLCommandQueue> queue,
		id<MTLAccelerationStructure> acceleration_structure,
		MTLAccelerationStructureDescriptor *descriptor,
		id<MTLBuffer> scratch_buffer) {
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLAccelerationStructureCommandEncoder> encoder = [command_buffer accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:acceleration_structure
			descriptor:descriptor
			scratchBuffer:scratch_buffer
			scratchBufferOffset:0];
	[encoder endEncoding];
	return commit_and_wait(command_buffer, @"acceleration-structure build");
}

static id<MTLCommandBuffer> trace(
		id<MTLCommandQueue> queue,
		id<MTLComputePipelineState> pipeline,
		id<MTLAccelerationStructure> tlas,
		id<MTLBuffer> result_buffer) {
	static_cast<uint32_t *>(result_buffer.contents)[0] = 0;
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
	[encoder setComputePipelineState:pipeline];
	[encoder setAccelerationStructure:tlas atBufferIndex:0];
	[encoder setBuffer:result_buffer offset:0 atIndex:1];
	[encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	[encoder endEncoding];
	return commit_and_wait(command_buffer, @"ray trace");
}

static id<MTLCommandBuffer> deform(
		id<MTLCommandQueue> queue,
		id<MTLComputePipelineState> pipeline,
		id<MTLBuffer> base_positions,
		id<MTLBuffer> morph_deltas,
		id<MTLBuffer> joint_indices,
		id<MTLBuffer> joint_weights,
		id<MTLBuffer> bone_matrices,
		id<MTLBuffer> parameters,
		id<MTLBuffer> output_positions) {
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
	[encoder setComputePipelineState:pipeline];
	[encoder setBuffer:base_positions offset:0 atIndex:0];
	[encoder setBuffer:morph_deltas offset:0 atIndex:1];
	[encoder setBuffer:joint_indices offset:0 atIndex:2];
	[encoder setBuffer:joint_weights offset:0 atIndex:3];
	[encoder setBuffer:bone_matrices offset:0 atIndex:4];
	[encoder setBuffer:parameters offset:0 atIndex:5];
	[encoder setBuffer:output_positions offset:0 atIndex:6];
	[encoder dispatchThreads:MTLSizeMake(3, 1, 1) threadsPerThreadgroup:MTLSizeMake(3, 1, 1)];
	[encoder endEncoding];
	return commit_and_wait(command_buffer, @"eight-weight skinning and blend-shape deformation");
}

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		if (argc != 2) {
			fail(@"usage: metal_rt_triangle <trace_triangle.metallib>");
		}

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			fail(@"no Metal device");
		}
		if (!device.supportsRaytracing) {
			fail(@"selected Metal device does not support ray tracing");
		}

		id<MTLCommandQueue> queue = [device newCommandQueue];
		if (queue == nil) {
			fail(@"failed to create Metal command queue");
		}

		PackedFloat3 base_positions[] = {
			{ -0.75f, -0.75f, 0.0f },
			{ 0.75f, -0.75f, 0.0f },
			{ 0.0f, 0.75f, 0.0f },
		};
		PackedFloat3 morph_deltas[] = {
			{ 0.5f, 0.0f, 0.0f },
			{ 0.5f, 0.0f, 0.0f },
			{ 0.5f, 0.0f, 0.0f },
		};
		uint16_t joint_indices[3 * 8];
		float joint_weights[3 * 8];
		for (uint32_t i = 0; i < 3 * 8; i++) {
			joint_indices[i] = i % 2;
			joint_weights[i] = 0.125f;
		}
		simd_float4x4 bone_matrices[] = { matrix_identity_float4x4, matrix_identity_float4x4 };
		float morph_weight = 0.0f;

		id<MTLBuffer> base_position_buffer = [device newBufferWithBytes:base_positions
				length:sizeof(base_positions)
				options:MTLResourceStorageModeShared];
		id<MTLBuffer> morph_delta_buffer = [device newBufferWithBytes:morph_deltas
				length:sizeof(morph_deltas)
				options:MTLResourceStorageModeShared];
		id<MTLBuffer> joint_index_buffer = [device newBufferWithBytes:joint_indices
				length:sizeof(joint_indices)
				options:MTLResourceStorageModeShared];
		id<MTLBuffer> joint_weight_buffer = [device newBufferWithBytes:joint_weights
				length:sizeof(joint_weights)
				options:MTLResourceStorageModeShared];
		id<MTLBuffer> bone_matrix_buffer = [device newBufferWithBytes:bone_matrices
				length:sizeof(bone_matrices)
				options:MTLResourceStorageModeShared];
		id<MTLBuffer> parameter_buffer = [device newBufferWithBytes:&morph_weight
				length:sizeof(morph_weight)
				options:MTLResourceStorageModeShared];
		id<MTLBuffer> vertex_buffer = [device newBufferWithLength:sizeof(base_positions)
				options:MTLResourceStorageModePrivate];
		vertex_buffer.label = @"M0 dynamic triangle vertices";
		id<MTLBuffer> previous_position_buffer = [device newBufferWithLength:sizeof(base_positions)
				options:MTLResourceStorageModePrivate];

		NSError *library_error = nil;
		NSURL *library_url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
		id<MTLLibrary> library = [device newLibraryWithURL:library_url error:&library_error];
		if (library == nil) {
			fail([NSString stringWithFormat:@"failed to load metallib: %@", library_error.localizedDescription]);
		}
		NSError *pipeline_error = nil;
		id<MTLComputePipelineState> deformation_pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"deform_triangle"] error:&pipeline_error];
		if (deformation_pipeline == nil) {
			fail([NSString stringWithFormat:@"failed to create deformation pipeline: %@", pipeline_error.localizedDescription]);
		}
		id<MTLComputePipelineState> trace_pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"trace_triangle"] error:&pipeline_error];
		if (trace_pipeline == nil) {
			fail([NSString stringWithFormat:@"failed to create trace pipeline: %@", pipeline_error.localizedDescription]);
		}

		id<MTLCommandBuffer> initial_deformation = deform(queue, deformation_pipeline, base_position_buffer,
				morph_delta_buffer, joint_index_buffer, joint_weight_buffer, bone_matrix_buffer,
				parameter_buffer, vertex_buffer);

		MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
				[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		geometry.vertexBuffer = vertex_buffer;
		geometry.vertexBufferOffset = 0;
		geometry.vertexFormat = MTLAttributeFormatFloat3;
		geometry.vertexStride = sizeof(PackedFloat3);
		geometry.triangleCount = 1;
		geometry.opaque = YES;

		MTLPrimitiveAccelerationStructureDescriptor *blas_descriptor =
				[MTLPrimitiveAccelerationStructureDescriptor descriptor];
		blas_descriptor.geometryDescriptors = @[ geometry ];
		blas_descriptor.usage = MTLAccelerationStructureUsageRefit | MTLAccelerationStructureUsagePreferFastIntersection;
		MTLAccelerationStructureSizes blas_sizes = [device accelerationStructureSizesWithDescriptor:blas_descriptor];
		id<MTLAccelerationStructure> blas = [device newAccelerationStructureWithSize:blas_sizes.accelerationStructureSize];
		id<MTLBuffer> blas_scratch = [device newBufferWithLength:std::max(blas_sizes.buildScratchBufferSize, blas_sizes.refitScratchBufferSize)
				options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> blas_build = build_acceleration_structure(queue, blas, blas_descriptor, blas_scratch);

		MTLAccelerationStructureInstanceDescriptor instance = {};
		instance.transformationMatrix.columns[0] = MTLPackedFloat3{ 1.0f, 0.0f, 0.0f };
		instance.transformationMatrix.columns[1] = MTLPackedFloat3{ 0.0f, 1.0f, 0.0f };
		instance.transformationMatrix.columns[2] = MTLPackedFloat3{ 0.0f, 0.0f, 1.0f };
		instance.transformationMatrix.columns[3] = MTLPackedFloat3{ 0.0f, 0.0f, 0.0f };
		instance.options = MTLAccelerationStructureInstanceOptionOpaque;
		instance.mask = 0xff;
		instance.accelerationStructureIndex = 0;
		id<MTLBuffer> instance_buffer = [device newBufferWithBytes:&instance
				length:sizeof(instance)
				options:MTLResourceStorageModeShared];

		MTLInstanceAccelerationStructureDescriptor *tlas_descriptor =
				[MTLInstanceAccelerationStructureDescriptor descriptor];
		tlas_descriptor.instancedAccelerationStructures = @[ blas ];
		tlas_descriptor.instanceCount = 1;
		tlas_descriptor.instanceDescriptorBuffer = instance_buffer;
		MTLAccelerationStructureSizes tlas_sizes = [device accelerationStructureSizesWithDescriptor:tlas_descriptor];
		id<MTLAccelerationStructure> tlas = [device newAccelerationStructureWithSize:tlas_sizes.accelerationStructureSize];
		id<MTLBuffer> tlas_scratch = [device newBufferWithLength:tlas_sizes.buildScratchBufferSize
				options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> tlas_build = build_acceleration_structure(queue, tlas, tlas_descriptor, tlas_scratch);

		id<MTLBuffer> result_buffer = [device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];

		id<MTLCommandBuffer> initial_trace = trace(queue, trace_pipeline, tlas, result_buffer);
		uint32_t initial_hit = static_cast<uint32_t *>(result_buffer.contents)[0];

		id<MTLCommandBuffer> previous_copy = [queue commandBuffer];
		id<MTLBlitCommandEncoder> previous_copy_encoder = [previous_copy blitCommandEncoder];
		[previous_copy_encoder copyFromBuffer:vertex_buffer sourceOffset:0 toBuffer:previous_position_buffer destinationOffset:0 size:sizeof(base_positions)];
		[previous_copy_encoder endEncoding];
		commit_and_wait(previous_copy, @"previous-position copy");

		simd_float4x4 *updated_bones = static_cast<simd_float4x4 *>(bone_matrix_buffer.contents);
		updated_bones[0].columns[3].x = 2.5f;
		updated_bones[1].columns[3].x = 2.5f;
		static_cast<float *>(parameter_buffer.contents)[0] = 1.0f;
		id<MTLCommandBuffer> deformed_geometry = deform(queue, deformation_pipeline, base_position_buffer,
				morph_delta_buffer, joint_index_buffer, joint_weight_buffer, bone_matrix_buffer,
				parameter_buffer, vertex_buffer);

		id<MTLCommandBuffer> refit_command = [queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> refit_encoder = [refit_command accelerationStructureCommandEncoder];
		[refit_encoder refitAccelerationStructure:blas
				descriptor:blas_descriptor
				destination:blas
				scratchBuffer:blas_scratch
				scratchBufferOffset:0];
		[refit_encoder endEncoding];
		commit_and_wait(refit_command, @"BLAS refit");

		id<MTLCommandBuffer> deformed_trace = trace(queue, trace_pipeline, tlas, result_buffer);
		uint32_t deformed_hit = static_cast<uint32_t *>(result_buffer.contents)[0];
		bool passed = initial_hit == 1 && deformed_hit == 0;

		printf("{\n");
		printf("  \"experiment\": \"M0-A+M0-F-metal\",\n");
		printf("  \"device\": \"%s\",\n", device.name.UTF8String);
		printf("  \"supports_ray_tracing\": true,\n");
		printf("  \"apple_family_9\": %s,\n", [device supportsFamily:MTLGPUFamilyApple9] ? "true" : "false");
		printf("  \"apple_family_10\": %s,\n", [device supportsFamily:MTLGPUFamilyApple10] ? "true" : "false");
		printf("  \"initial_hit\": %u,\n", initial_hit);
		printf("  \"deformed_hit_after_refit\": %u,\n", deformed_hit);
		printf("  \"deformation\": {\"influences\": 8, \"blend_shapes\": 1, \"previous_positions_preserved\": true},\n");
		printf("  \"gpu_ms\": {\"initial_deformation\": %.6f, \"blas_build\": %.6f, \"tlas_build\": %.6f, \"initial_trace\": %.6f, \"previous_position_copy\": %.6f, \"deformed_geometry\": %.6f, \"blas_refit\": %.6f, \"deformed_trace\": %.6f},\n",
				gpu_milliseconds(initial_deformation), gpu_milliseconds(blas_build), gpu_milliseconds(tlas_build),
				gpu_milliseconds(initial_trace), gpu_milliseconds(previous_copy), gpu_milliseconds(deformed_geometry),
				gpu_milliseconds(refit_command), gpu_milliseconds(deformed_trace));
		printf("  \"passed\": %s\n", passed ? "true" : "false");
		printf("}\n");
		return passed ? 0 : 2;
	}
}
