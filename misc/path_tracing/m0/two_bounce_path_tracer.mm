#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

struct PackedFloat3 { float x; float y; float z; };
struct PathMaterial { simd_float4 base_color; simd_float4 emission; simd_float4 parameters; };
struct FrameParameters {
	simd_float4 camera_position;
	simd_float4 camera_forward_and_tan_half_fov;
	simd_float4 camera_right;
	simd_float4 camera_up;
	simd_uint4 dimensions_seed;
	simd_float4 dynamic_motion_and_light_area;
};

static void fail(NSString *message) {
	std::fprintf(stderr, "%s\n", message.UTF8String);
	exit(1);
}

static double gpu_milliseconds(id<MTLCommandBuffer> command_buffer) {
	const double elapsed = command_buffer.GPUEndTime - command_buffer.GPUStartTime;
	return elapsed > 0.0 ? elapsed * 1000.0 : 0.0;
}

static void complete(id<MTLCommandBuffer> command_buffer, NSString *stage) {
	[command_buffer commit];
	[command_buffer waitUntilCompleted];
	if (command_buffer.status != MTLCommandBufferStatusCompleted) {
		fail([NSString stringWithFormat:@"%@: %@", stage, command_buffer.error.localizedDescription ?: @"command failed"]);
	}
}

static uint64_t fnv1a64(const uint8_t *bytes, size_t size) {
	uint64_t hash = 14695981039346656037ULL;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void add_triangle(std::vector<PackedFloat3> &vertices, std::vector<uint32_t> &materials,
		PackedFloat3 a, PackedFloat3 b, PackedFloat3 c, uint32_t material) {
	vertices.push_back(a);
	vertices.push_back(b);
	vertices.push_back(c);
	materials.push_back(material);
}

static void add_quad(std::vector<PackedFloat3> &vertices, std::vector<uint32_t> &materials,
		PackedFloat3 a, PackedFloat3 b, PackedFloat3 c, PackedFloat3 d, uint32_t material) {
	add_triangle(vertices, materials, a, b, c, material);
	add_triangle(vertices, materials, a, c, d, material);
}

static id<MTLCommandBuffer> encode_as_build(id<MTLCommandQueue> queue,
		id<MTLAccelerationStructure> acceleration_structure,
		MTLAccelerationStructureDescriptor *descriptor,
		id<MTLBuffer> scratch_buffer) {
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLAccelerationStructureCommandEncoder> encoder = [command_buffer accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:acceleration_structure descriptor:descriptor scratchBuffer:scratch_buffer scratchBufferOffset:0];
	[encoder endEncoding];
	complete(command_buffer, @"acceleration-structure build");
	return command_buffer;
}

static id<MTLCommandBuffer> encode_trace(id<MTLCommandQueue> queue,
		id<MTLComputePipelineState> pipeline,
		id<MTLAccelerationStructure> tlas,
		id<MTLBuffer> vertex_buffer,
		id<MTLBuffer> primitive_material_buffer,
		id<MTLBuffer> material_buffer,
		id<MTLBuffer> parameter_buffer,
		const std::vector<id<MTLBuffer>> &outputs,
		NSUInteger width,
		NSUInteger height) {
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
	[encoder setComputePipelineState:pipeline];
	[encoder setAccelerationStructure:tlas atBufferIndex:0];
	[encoder setBuffer:vertex_buffer offset:0 atIndex:1];
	[encoder setBuffer:primitive_material_buffer offset:0 atIndex:2];
	[encoder setBuffer:material_buffer offset:0 atIndex:3];
	[encoder setBuffer:parameter_buffer offset:0 atIndex:4];
	for (NSUInteger i = 0; i < outputs.size(); i++) {
		[encoder setBuffer:outputs[i] offset:0 atIndex:5 + i];
	}
	const MTLSize threads = MTLSizeMake(8, 8, 1);
	[encoder dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:threads];
	[encoder endEncoding];
	complete(command_buffer, @"two-bounce trace");
	return command_buffer;
}

static void write_ppm(const std::string &path, const simd_float4 *pixels, uint32_t width, uint32_t height) {
	std::ofstream output(path, std::ios::binary);
	output << "P6\n" << width << " " << height << "\n255\n";
	for (uint32_t i = 0; i < width * height; i++) {
		unsigned char rgb[3];
		for (uint32_t channel = 0; channel < 3; channel++) {
			const float mapped = std::pow(std::clamp(pixels[i][channel], 0.0f, 1.0f), 1.0f / 2.2f);
			rgb[channel] = static_cast<unsigned char>(std::round(mapped * 255.0f));
		}
		output.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
	}
	if (!output.good()) {
		fail(@"failed to write PPM capture");
	}
}

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		if (argc != 3) {
			fail(@"usage: two_bounce_path_tracer <metallib> <output-directory>");
		}
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil || !device.supportsRaytracing) {
			fail(@"selected Metal device does not support ray tracing");
		}
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
		if (library == nil) {
			fail([NSString stringWithFormat:@"failed to load path-tracing library: %@", error.localizedDescription]);
		}
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"trace_two_bounce_scene"] error:&error];
		if (pipeline == nil) {
			fail([NSString stringWithFormat:@"failed to create path-tracing pipeline: %@", error.localizedDescription]);
		}
		id<MTLCommandQueue> queue = [device newCommandQueue];

		std::vector<PackedFloat3> vertices;
		std::vector<uint32_t> primitive_materials;
		add_quad(vertices, primitive_materials, {-2,-1,0}, {2,-1,0}, {2,-1,-4}, {-2,-1,-4}, 0);
		add_quad(vertices, primitive_materials, {-2,2,-4}, {2,2,-4}, {2,2,0}, {-2,2,0}, 0);
		add_quad(vertices, primitive_materials, {-2,-1,-4}, {2,-1,-4}, {2,2,-4}, {-2,2,-4}, 0);
		add_quad(vertices, primitive_materials, {-2,-1,0}, {-2,-1,-4}, {-2,2,-4}, {-2,2,0}, 1);
		add_quad(vertices, primitive_materials, {2,-1,-4}, {2,-1,0}, {2,2,0}, {2,2,-4}, 2);
		add_quad(vertices, primitive_materials, {-0.45f,1.90f,-1.85f}, {0.45f,1.90f,-1.85f}, {0.45f,1.90f,-2.65f}, {-0.45f,1.90f,-2.65f}, 4);
		add_quad(vertices, primitive_materials, {-1.5f,-0.8f,-3.92f}, {1.5f,-0.8f,-3.92f}, {1.5f,1.5f,-3.92f}, {-1.5f,1.5f,-3.92f}, 5);
		const size_t dynamic_vertex_offset = vertices.size();
		add_triangle(vertices, primitive_materials, {0.35f,-0.65f,-1.55f}, {1.05f,-0.65f,-1.55f}, {0.70f,0.55f,-1.55f}, 6);

		PathMaterial materials[] = {
			{ {0.72f,0.72f,0.72f,1}, {0,0,0,0}, {0.65f,0,0.04f,0} },
			{ {0.70f,0.08f,0.06f,1}, {0,0,0,0}, {0.7f,0,0.04f,0} },
			{ {0.06f,0.60f,0.12f,1}, {0,0,0,0}, {0.7f,0,0.04f,0} },
			{ {0.15f,0.20f,0.75f,1}, {0,0,0,0}, {0.3f,0,0.04f,0} },
			{ {1,1,1,1}, {1.0f,0.82f,0.62f,14.0f}, {0,0,0,0} },
			{ {0.92f,0.92f,0.95f,1}, {0,0,0,0}, {0.0f,1.0f,0.98f,0} },
			{ {0.10f,0.24f,0.92f,1}, {0,0,0,0}, {0.28f,0.25f,0.1f,0} },
		};

		id<MTLBuffer> vertex_buffer = [device newBufferWithBytes:vertices.data() length:vertices.size() * sizeof(PackedFloat3) options:MTLResourceStorageModeShared];
		id<MTLBuffer> primitive_material_buffer = [device newBufferWithBytes:primitive_materials.data() length:primitive_materials.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];
		id<MTLBuffer> material_buffer = [device newBufferWithBytes:materials length:sizeof(materials) options:MTLResourceStorageModeShared];

		MTLAccelerationStructureTriangleGeometryDescriptor *geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		geometry.vertexBuffer = vertex_buffer;
		geometry.vertexFormat = MTLAttributeFormatFloat3;
		geometry.vertexStride = sizeof(PackedFloat3);
		geometry.triangleCount = primitive_materials.size();
		geometry.opaque = YES;
		MTLPrimitiveAccelerationStructureDescriptor *blas_descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		blas_descriptor.geometryDescriptors = @[ geometry ];
		blas_descriptor.usage = MTLAccelerationStructureUsageRefit | MTLAccelerationStructureUsagePreferFastIntersection;
		const MTLAccelerationStructureSizes blas_sizes = [device accelerationStructureSizesWithDescriptor:blas_descriptor];
		id<MTLAccelerationStructure> blas = [device newAccelerationStructureWithSize:blas_sizes.accelerationStructureSize];
		id<MTLBuffer> blas_scratch = [device newBufferWithLength:std::max(blas_sizes.buildScratchBufferSize, blas_sizes.refitScratchBufferSize) options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> blas_build = encode_as_build(queue, blas, blas_descriptor, blas_scratch);

		MTLAccelerationStructureInstanceDescriptor instance = {};
		instance.transformationMatrix.columns[0] = MTLPackedFloat3{1,0,0};
		instance.transformationMatrix.columns[1] = MTLPackedFloat3{0,1,0};
		instance.transformationMatrix.columns[2] = MTLPackedFloat3{0,0,1};
		instance.options = MTLAccelerationStructureInstanceOptionOpaque;
		instance.mask = 0xff;
		id<MTLBuffer> instance_buffer = [device newBufferWithBytes:&instance length:sizeof(instance) options:MTLResourceStorageModeShared];
		MTLInstanceAccelerationStructureDescriptor *tlas_descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
		tlas_descriptor.instancedAccelerationStructures = @[ blas ];
		tlas_descriptor.instanceCount = 1;
		tlas_descriptor.instanceDescriptorBuffer = instance_buffer;
		const MTLAccelerationStructureSizes tlas_sizes = [device accelerationStructureSizesWithDescriptor:tlas_descriptor];
		id<MTLAccelerationStructure> tlas = [device newAccelerationStructureWithSize:tlas_sizes.accelerationStructureSize];
		id<MTLBuffer> tlas_scratch = [device newBufferWithLength:tlas_sizes.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> tlas_build = encode_as_build(queue, tlas, tlas_descriptor, tlas_scratch);

		constexpr uint32_t width = 192;
		constexpr uint32_t height = 128;
		const size_t pixel_count = width * height;
		FrameParameters parameters = {};
		parameters.camera_position = {0,0.25f,2.5f,0};
		parameters.camera_forward_and_tan_half_fov = {0,0,-1,std::tan(55.0f * 0.5f * float(M_PI) / 180.0f)};
		parameters.camera_right = {1,0,0,0};
		parameters.camera_up = {0,1,0,0};
		parameters.dimensions_seed = {width,height,0x4d305054u,0};
		parameters.dynamic_motion_and_light_area = {0.08f,0,0,0.36f};
		id<MTLBuffer> parameter_buffer = [device newBufferWithBytes:&parameters length:sizeof(parameters) options:MTLResourceStorageModeShared];
		std::vector<id<MTLBuffer>> outputs = {
			[device newBufferWithLength:pixel_count * sizeof(simd_float4) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(float) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(simd_float2) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(simd_float4) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(simd_float4) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(simd_float4) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(float) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(float) options:MTLResourceStorageModeShared],
			[device newBufferWithLength:pixel_count * sizeof(uint32_t) options:MTLResourceStorageModeShared],
		};

		id<MTLCommandBuffer> initial_trace = encode_trace(queue, pipeline, tlas, vertex_buffer, primitive_material_buffer, material_buffer, parameter_buffer, outputs, width, height);
		const size_t radiance_bytes = pixel_count * sizeof(simd_float4);
		const uint64_t initial_hash = fnv1a64(static_cast<const uint8_t *>(outputs[0].contents), radiance_bytes);
		std::vector<uint8_t> initial_copy(radiance_bytes);
		std::copy_n(static_cast<const uint8_t *>(outputs[0].contents), radiance_bytes, initial_copy.data());
		uint32_t initial_reflection_pixels = 0;
		for (size_t i = 0; i < pixel_count; i++) {
			initial_reflection_pixels += static_cast<const uint32_t *>(outputs[8].contents)[i] != 0 ? 1U : 0U;
		}
		write_ppm(std::string(argv[2]) + "/initial.ppm", static_cast<const simd_float4 *>(outputs[0].contents), width, height);

		id<MTLCommandBuffer> repeat_trace = encode_trace(queue, pipeline, tlas, vertex_buffer, primitive_material_buffer, material_buffer, parameter_buffer, outputs, width, height);
		const bool deterministic = std::equal(initial_copy.begin(), initial_copy.end(), static_cast<const uint8_t *>(outputs[0].contents));

		PackedFloat3 *updated_vertices = static_cast<PackedFloat3 *>(vertex_buffer.contents);
		for (size_t i = dynamic_vertex_offset; i < dynamic_vertex_offset + 3; i++) {
			updated_vertices[i].x -= 1.4f;
		}
		parameters.dynamic_motion_and_light_area.x = -0.08f;
		std::memcpy(parameter_buffer.contents, &parameters, sizeof(parameters));
		id<MTLCommandBuffer> refit_command = [queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> refit_encoder = [refit_command accelerationStructureCommandEncoder];
		[refit_encoder refitAccelerationStructure:blas descriptor:blas_descriptor destination:blas scratchBuffer:blas_scratch scratchBufferOffset:0];
		[refit_encoder endEncoding];
		complete(refit_command, @"BLAS refit");
		id<MTLCommandBuffer> deformed_trace = encode_trace(queue, pipeline, tlas, vertex_buffer, primitive_material_buffer, material_buffer, parameter_buffer, outputs, width, height);
		const uint64_t deformed_hash = fnv1a64(static_cast<const uint8_t *>(outputs[0].contents), radiance_bytes);
		uint32_t deformed_reflection_pixels = 0;
		for (size_t i = 0; i < pixel_count; i++) {
			deformed_reflection_pixels += static_cast<const uint32_t *>(outputs[8].contents)[i] != 0 ? 1U : 0U;
		}
		write_ppm(std::string(argv[2]) + "/deformed.ppm", static_cast<const simd_float4 *>(outputs[0].contents), width, height);

		const bool passed = deterministic && initial_hash != deformed_hash &&
				initial_reflection_pixels > 0 && deformed_reflection_pixels > 0;
		std::printf("{\n");
		std::printf("  \"experiment\": \"M0-two-bounce-metal-corpus\",\n");
		std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
		std::printf("  \"resolution\": [%u, %u],\n", width, height);
		std::printf("  \"samples_per_pixel\": 1,\n");
		std::printf("  \"maximum_surface_bounces\": 2,\n");
		std::printf("  \"lighting\": [\"emissive_geometry\", \"environment\"],\n");
		std::printf("  \"guides\": [\"depth\", \"motion\", \"normal\", \"diffuse_albedo\", \"specular_albedo\", \"roughness\", \"specular_hit_distance\"],\n");
		std::printf("  \"initial_radiance_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(initial_hash));
		std::printf("  \"deformed_radiance_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(deformed_hash));
		std::printf("  \"repeat_deterministic\": %s,\n", deterministic ? "true" : "false");
		std::printf("  \"reflected_dynamic_pixels\": [%u, %u],\n", initial_reflection_pixels, deformed_reflection_pixels);
		std::printf("  \"gpu_ms\": {\"blas_build\": %.6f, \"tlas_build\": %.6f, \"initial_trace\": %.6f, \"repeat_trace\": %.6f, \"blas_refit\": %.6f, \"deformed_trace\": %.6f},\n",
				gpu_milliseconds(blas_build), gpu_milliseconds(tlas_build), gpu_milliseconds(initial_trace),
				gpu_milliseconds(repeat_trace), gpu_milliseconds(refit_command), gpu_milliseconds(deformed_trace));
		std::printf("  \"passed\": %s\n", passed ? "true" : "false");
		std::printf("}\n");
		return passed ? 0 : 2;
	}
}
