#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "canonical_scene_packet.h"

#include <cstdio>

using namespace GodotPathTracingM0;

static void fail(NSString *message) {
	std::fprintf(stderr, "%s\n", message.UTF8String);
	exit(1);
}

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		if (argc != 2) {
			fail(@"usage: validate_metal_packet_layout <layout.metallib>");
		}
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			fail(@"no Metal device");
		}
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
		if (library == nil) {
			fail([NSString stringWithFormat:@"failed to load layout metallib: %@", error.localizedDescription]);
		}
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"report_packet_layout"] error:&error];
		if (pipeline == nil) {
			fail([NSString stringWithFormat:@"failed to create layout pipeline: %@", error.localizedDescription]);
		}
		id<MTLBuffer> values_buffer = [device newBufferWithLength:12 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
		id<MTLCommandQueue> queue = [device newCommandQueue];
		id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
		id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
		[encoder setComputePipelineState:pipeline];
		[encoder setBuffer:values_buffer offset:0 atIndex:0];
		[encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
		[encoder endEncoding];
		[command_buffer commit];
		[command_buffer waitUntilCompleted];
		if (command_buffer.status != MTLCommandBufferStatusCompleted) {
			fail([NSString stringWithFormat:@"layout kernel failed: %@", command_buffer.error.localizedDescription]);
		}
		const uint32_t expected[] = {
			sizeof(Float4), sizeof(Matrix4), sizeof(GuideContract), sizeof(CameraRecord),
			sizeof(InstanceRecord), sizeof(MaterialRecord), sizeof(LightRecord), sizeof(ScenePacketHeader),
			offsetof(CameraRecord, view_index), offsetof(InstanceRecord, geometry_id),
			offsetof(ScenePacketHeader, total_size), offsetof(ScenePacketHeader, guide_contract_offset),
		};
		const uint32_t *actual = static_cast<const uint32_t *>(values_buffer.contents);
		bool passed = true;
		for (uint32_t i = 0; i < 12; i++) {
			passed = passed && actual[i] == expected[i];
		}
		std::printf("{\n");
		std::printf("  \"experiment\": \"M0-J-metal-layout\",\n");
		std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
		std::printf("  \"record_sizes\": [%u, %u, %u, %u, %u, %u, %u, %u],\n",
				actual[0], actual[1], actual[2], actual[3], actual[4], actual[5], actual[6], actual[7]);
		std::printf("  \"selected_offsets\": [%u, %u, %u, %u],\n", actual[8], actual[9], actual[10], actual[11]);
		std::printf("  \"passed\": %s\n", passed ? "true" : "false");
		std::printf("}\n");
		return passed ? 0 : 2;
	}
}
