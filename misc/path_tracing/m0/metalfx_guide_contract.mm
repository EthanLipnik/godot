// Standalone MetalFX path-tracing guide-contract viability spike.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <simd/simd.h>

#include <cstdio>

static void fail(NSString *message) {
	fprintf(stderr, "%s\n", message.UTF8String);
	exit(1);
}

static id<MTLTexture> make_texture(
		id<MTLDevice> device,
		NSString *label,
		MTLPixelFormat format,
		NSUInteger width,
		NSUInteger height,
		MTLTextureUsage usage,
		MTLStorageMode storage_mode) {
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
			width:width
			height:height
			mipmapped:NO];
	descriptor.usage = usage;
	descriptor.storageMode = storage_mode;
	id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
	if (texture == nil) {
		fail([NSString stringWithFormat:@"failed to allocate %@", label]);
	}
	texture.label = label;
	return texture;
}

int main() {
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			fail(@"no Metal device");
		}
		if (![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device]) {
			fail(@"selected device does not support MTLFXTemporalDenoisedScaler");
		}

		const NSUInteger input_width = 64;
		const NSUInteger input_height = 64;
		const NSUInteger output_width = 128;
		const NSUInteger output_height = 128;

		MTLFXTemporalDenoisedScalerDescriptor *descriptor = [MTLFXTemporalDenoisedScalerDescriptor new];
		descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.depthTextureFormat = MTLPixelFormatDepth32Float;
		descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
		descriptor.normalTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.roughnessTextureFormat = MTLPixelFormatR16Float;
		descriptor.denoiseStrengthMaskTextureFormat = MTLPixelFormatR8Unorm;
		descriptor.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
		descriptor.specularHitDistanceTextureFormat = MTLPixelFormatR16Float;
		descriptor.transparencyOverlayTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.inputWidth = input_width;
		descriptor.inputHeight = input_height;
		descriptor.outputWidth = output_width;
		descriptor.outputHeight = output_height;
		descriptor.autoExposureEnabled = YES;
		descriptor.reactiveMaskTextureEnabled = YES;
		descriptor.denoiseStrengthMaskTextureEnabled = YES;
		descriptor.specularHitDistanceTextureEnabled = YES;
		descriptor.transparencyOverlayTextureEnabled = YES;
		descriptor.requiresSynchronousInitialization = YES;

		id<MTLFXTemporalDenoisedScaler> scaler = [descriptor newTemporalDenoisedScalerWithDevice:device];
		if (scaler == nil) {
			fail(@"MetalFX rejected the complete guide descriptor");
		}

		scaler.colorTexture = make_texture(device, @"M0 radiance", descriptor.colorTextureFormat,
				input_width, input_height, scaler.colorTextureUsage, MTLStorageModePrivate);
		scaler.depthTexture = make_texture(device, @"M0 depth", descriptor.depthTextureFormat,
				input_width, input_height, scaler.depthTextureUsage, MTLStorageModePrivate);
		scaler.motionTexture = make_texture(device, @"M0 motion", descriptor.motionTextureFormat,
				input_width, input_height, scaler.motionTextureUsage, MTLStorageModePrivate);
		scaler.normalTexture = make_texture(device, @"M0 normal", descriptor.normalTextureFormat,
				input_width, input_height, scaler.normalTextureUsage, MTLStorageModePrivate);
		scaler.diffuseAlbedoTexture = make_texture(device, @"M0 diffuse albedo", descriptor.diffuseAlbedoTextureFormat,
				input_width, input_height, scaler.diffuseAlbedoTextureUsage, MTLStorageModePrivate);
		scaler.specularAlbedoTexture = make_texture(device, @"M0 specular albedo", descriptor.specularAlbedoTextureFormat,
				input_width, input_height, scaler.specularAlbedoTextureUsage, MTLStorageModePrivate);
		scaler.roughnessTexture = make_texture(device, @"M0 roughness", descriptor.roughnessTextureFormat,
				input_width, input_height, scaler.roughnessTextureUsage, MTLStorageModePrivate);
		scaler.denoiseStrengthMaskTexture = make_texture(device, @"M0 denoise strength", descriptor.denoiseStrengthMaskTextureFormat,
				input_width, input_height, scaler.denoiseStrengthMaskTextureUsage, MTLStorageModePrivate);
		scaler.reactiveMaskTexture = make_texture(device, @"M0 reactive mask", descriptor.reactiveMaskTextureFormat,
				input_width, input_height, scaler.reactiveMaskTextureUsage, MTLStorageModePrivate);
		scaler.specularHitDistanceTexture = make_texture(device, @"M0 specular hit distance", descriptor.specularHitDistanceTextureFormat,
				input_width, input_height, scaler.specularHitDistanceTextureUsage, MTLStorageModePrivate);
		scaler.transparencyOverlayTexture = make_texture(device, @"M0 transparency overlay", descriptor.transparencyOverlayTextureFormat,
				input_width, input_height, scaler.transparencyOverlayTextureUsage, MTLStorageModePrivate);
		scaler.outputTexture = make_texture(device, @"M0 output", descriptor.outputTextureFormat,
				output_width, output_height, scaler.outputTextureUsage, MTLStorageModePrivate);

		scaler.motionVectorScaleX = (float)input_width;
		scaler.motionVectorScaleY = (float)input_height;
		scaler.jitterOffsetX = 0.0f;
		scaler.jitterOffsetY = 0.0f;
		scaler.preExposure = 1.0f;
		scaler.shouldResetHistory = YES;
		scaler.depthReversed = YES;
		scaler.worldToViewMatrix = matrix_identity_float4x4;
		scaler.viewToClipMatrix = matrix_identity_float4x4;

		id<MTLCommandQueue> queue = [device newCommandQueue];
		id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
		[scaler encodeToCommandBuffer:command_buffer];
		[command_buffer commit];
		[command_buffer waitUntilCompleted];
		if (command_buffer.status != MTLCommandBufferStatusCompleted) {
			fail([NSString stringWithFormat:@"MetalFX encode failed: %@", command_buffer.error.localizedDescription]);
		}

		double gpu_ms = (command_buffer.GPUEndTime - command_buffer.GPUStartTime) * 1000.0;
		printf("{\n");
		printf("  \"experiment\": \"M0-B\",\n");
		printf("  \"device\": \"%s\",\n", device.name.UTF8String);
		printf("  \"supports_denoised_scaler\": true,\n");
		printf("  \"apple_family_9\": %s,\n", [device supportsFamily:MTLGPUFamilyApple9] ? "true" : "false");
		printf("  \"input_size\": [64, 64],\n");
		printf("  \"output_size\": [128, 128],\n");
		printf("  \"enabled_guides\": [\"depth\", \"motion\", \"normal\", \"diffuse_albedo\", \"specular_albedo\", \"roughness\", \"denoise_strength\", \"reactive_mask\", \"specular_hit_distance\", \"transparency_overlay\"],\n");
		printf("  \"motion_convention\": \"current_to_previous_scaled_to_pixels\",\n");
		printf("  \"depth_reversed\": true,\n");
		printf("  \"gpu_ms\": %.6f,\n", gpu_ms > 0.0 ? gpu_ms : 0.0);
		printf("  \"passed\": true\n");
		printf("}\n");
		return 0;
	}
}
