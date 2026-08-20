#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <simd/simd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using Half = _Float16;

struct Half2 { Half x; Half y; };
struct Half4 { Half x; Half y; Half z; Half w; };

static void fail(NSString *message) {
	std::fprintf(stderr, "%s\n", message.UTF8String);
	exit(1);
}

static id<MTLTexture> make_texture(id<MTLDevice> device, MTLPixelFormat format,
		NSUInteger width, NSUInteger height, MTLTextureUsage usage, NSString *label) {
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
	descriptor.usage = usage;
	descriptor.storageMode = MTLStorageModeShared;
	id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
	if (texture == nil) {
		fail([NSString stringWithFormat:@"failed to create texture %@", label]);
	}
	texture.label = label;
	return texture;
}

template <typename T>
static void upload(id<MTLTexture> texture, const std::vector<T> &values, NSUInteger width, NSUInteger height) {
	[texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
		mipmapLevel:0
		withBytes:values.data()
		bytesPerRow:width * sizeof(T)];
}

static uint32_t hash_uint(uint32_t value) {
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	return value ^ (value >> 16);
}

static float hash_noise(uint32_t x, uint32_t y, uint32_t frame, uint32_t eye) {
	const uint32_t value = hash_uint(x ^ (y * 0x9e3779b9U) ^ (frame * 0x85ebca6bU) ^ (eye * 0xc2b2ae35U));
	return float(value & 0x00ffffffU) / float(0x01000000U) * 2.0f - 1.0f;
}

static float segment_distance(float x, float y, float ax, float ay, float bx, float by) {
	const float vx = bx - ax;
	const float vy = by - ay;
	const float denominator = std::max(1e-8f, vx * vx + vy * vy);
	const float t = std::clamp(((x - ax) * vx + (y - ay) * vy) / denominator, 0.0f, 1.0f);
	return std::hypot(x - (ax + vx * t), y - (ay + vy * t));
}

struct Sample {
	float r;
	float g;
	float b;
	float depth;
	float motion_x;
	float motion_y;
	float normal_x;
	float normal_y;
	float normal_z;
	float diffuse_r;
	float diffuse_g;
	float diffuse_b;
	float specular;
	float roughness;
	float denoise;
	float reactive;
	float hit_distance;
	float overlay_r;
	float overlay_g;
	float overlay_b;
	float overlay_a;
};

static Sample evaluate_clean(float x, float y, uint32_t frame, uint32_t eye) {
	const float phase = float(frame) / 11.0f;
	const float eye_shift = eye == 0 ? -0.018f : 0.018f;
	const float camera_pan = 0.035f * std::sin(phase * 6.2831853f);
	const float previous_camera_pan = 0.035f * std::sin(std::max(0.0f, float(frame - (frame > 0 ? 1 : 0))) / 11.0f * 6.2831853f);
	const float world_x = x + eye_shift + camera_pan;
	Sample sample = {};
	const float checker = std::fmod(std::floor((world_x + 1.0f) * 18.0f) + std::floor((y + 1.0f) * 18.0f), 2.0f);
	sample.r = 0.055f + checker * 0.018f;
	sample.g = 0.075f + checker * 0.018f;
	sample.b = 0.11f + checker * 0.025f;
	sample.depth = 0.12f;
	sample.motion_x = previous_camera_pan - camera_pan;
	sample.normal_z = 1.0f;
	sample.diffuse_r = 0.12f;
	sample.diffuse_g = 0.16f;
	sample.diffuse_b = 0.22f;
	sample.specular = 0.04f;
	sample.roughness = 0.8f;
	sample.denoise = 1.0f;

	const float previous_phase = float(frame > 0 ? frame - 1 : 0) / 11.0f;
	const float center_x = -0.55f + phase * 1.10f;
	const float previous_center_x = -0.55f + previous_phase * 1.10f;
	const float center_y = 0.04f * std::sin(phase * 12.56637f);
	const float radius_x = 0.22f + 0.07f * std::sin(phase * 6.2831853f);
	const float radius_y = 0.28f - 0.05f * std::sin(phase * 6.2831853f);
	const float nx = (world_x - center_x) / radius_x;
	const float ny = (y - center_y) / radius_y;
	const float disc = nx * nx + ny * ny;
	if (disc <= 1.0f) {
		const float nz = std::sqrt(std::max(0.0f, 1.0f - disc));
		sample.r = 0.08f + 0.12f * nz;
		sample.g = 0.20f + 0.18f * nz;
		sample.b = 0.72f + 0.22f * nz;
		sample.depth = 0.78f;
		sample.motion_x = (previous_center_x - center_x) + (previous_camera_pan - camera_pan);
		sample.normal_x = nx;
		sample.normal_y = ny;
		sample.normal_z = nz;
		sample.diffuse_r = 0.08f;
		sample.diffuse_g = 0.20f;
		sample.diffuse_b = 0.72f;
		sample.specular = 0.72f;
		sample.roughness = 0.16f + 0.18f * std::abs(ny);
		sample.hit_distance = 0.4f + 1.8f * (1.0f - nz);
		const float highlight_x = nx - 0.28f * std::sin(phase * 18.0f);
		const float highlight_y = ny + 0.22f;
		const float highlight = std::exp(-(highlight_x * highlight_x + highlight_y * highlight_y) * 95.0f);
		sample.r += highlight * 2.5f;
		sample.g += highlight * 2.2f;
		sample.b += highlight * 1.8f;
		sample.reactive = std::clamp(highlight, 0.0f, 1.0f);
	}

	const float curve_y = 0.58f + 0.10f * std::sin((world_x + phase * 0.6f) * 15.0f);
	if (std::abs(y - curve_y) < 0.012f) {
		sample.r = 1.8f;
		sample.g = 1.65f;
		sample.b = 1.35f;
		sample.depth = 0.70f;
		sample.motion_x = -0.6f / 11.0f + (previous_camera_pan - camera_pan);
		sample.diffuse_r = 0.9f;
		sample.diffuse_g = 0.85f;
		sample.diffuse_b = 0.7f;
		sample.roughness = 0.3f;
		sample.reactive = 1.0f;
		sample.overlay_r = 0.25f;
		sample.overlay_g = 0.2f;
		sample.overlay_b = 0.1f;
		sample.overlay_a = 0.25f;
	}

	const float joint_x = -0.58f;
	const float joint_y = -0.48f;
	const float angle = -0.8f + phase * 1.6f;
	const float elbow_x = joint_x + 0.30f * std::cos(angle);
	const float elbow_y = joint_y + 0.30f * std::sin(angle);
	const float hand_x = elbow_x + 0.28f * std::cos(angle * 1.7f + 0.5f);
	const float hand_y = elbow_y + 0.28f * std::sin(angle * 1.7f + 0.5f);
	if (segment_distance(world_x, y, joint_x, joint_y, elbow_x, elbow_y) < 0.025f ||
			segment_distance(world_x, y, elbow_x, elbow_y, hand_x, hand_y) < 0.020f) {
		sample.r = 0.9f;
		sample.g = 0.18f;
		sample.b = 0.08f;
		sample.depth = 0.68f;
		sample.motion_x = -0.05f * std::cos(angle);
		sample.motion_y = -0.05f * std::sin(angle);
		sample.diffuse_r = 0.9f;
		sample.diffuse_g = 0.18f;
		sample.diffuse_b = 0.08f;
		sample.roughness = 0.42f;
		sample.reactive = 0.8f;
	}

	if (world_x > 0.45f && y < -0.35f) {
		const float relief = 0.5f + 0.5f * std::sin((world_x * 150.0f) + phase * 4.0f);
		sample.r += relief * 0.12f;
		sample.g += relief * 0.10f;
		sample.b += relief * 0.08f;
		sample.normal_x = (relief - 0.5f) * 0.35f;
		sample.normal_z = std::sqrt(std::max(0.0f, 1.0f - sample.normal_x * sample.normal_x));
		sample.roughness = 0.38f;
	}
	return sample;
}

struct EyeResources {
	id<MTLFXTemporalDenoisedScaler> scaler;
	id<MTLTexture> color;
	id<MTLTexture> depth;
	id<MTLTexture> motion;
	id<MTLTexture> normal;
	id<MTLTexture> diffuse;
	id<MTLTexture> specular;
	id<MTLTexture> roughness;
	id<MTLTexture> denoise;
	id<MTLTexture> reactive;
	id<MTLTexture> hit_distance;
	id<MTLTexture> overlay;
	id<MTLTexture> output;
};

static EyeResources make_eye(id<MTLDevice> device, MTLFXTemporalDenoisedScalerDescriptor *descriptor, NSString *label) {
	EyeResources eye = {};
	eye.scaler = [descriptor newTemporalDenoisedScalerWithDevice:device];
	if (eye.scaler == nil) {
		fail(@"MetalFX rejected temporal quality descriptor");
	}
	eye.color = make_texture(device, descriptor.colorTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.colorTextureUsage, [label stringByAppendingString:@" color"]);
	eye.depth = make_texture(device, descriptor.depthTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.depthTextureUsage, [label stringByAppendingString:@" depth"]);
	eye.motion = make_texture(device, descriptor.motionTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.motionTextureUsage, [label stringByAppendingString:@" motion"]);
	eye.normal = make_texture(device, descriptor.normalTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.normalTextureUsage, [label stringByAppendingString:@" normal"]);
	eye.diffuse = make_texture(device, descriptor.diffuseAlbedoTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.diffuseAlbedoTextureUsage, [label stringByAppendingString:@" diffuse"]);
	eye.specular = make_texture(device, descriptor.specularAlbedoTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.specularAlbedoTextureUsage, [label stringByAppendingString:@" specular"]);
	eye.roughness = make_texture(device, descriptor.roughnessTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.roughnessTextureUsage, [label stringByAppendingString:@" roughness"]);
	eye.denoise = make_texture(device, descriptor.denoiseStrengthMaskTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.denoiseStrengthMaskTextureUsage, [label stringByAppendingString:@" denoise"]);
	eye.reactive = make_texture(device, descriptor.reactiveMaskTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.reactiveMaskTextureUsage, [label stringByAppendingString:@" reactive"]);
	eye.hit_distance = make_texture(device, descriptor.specularHitDistanceTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.specularHitDistanceTextureUsage, [label stringByAppendingString:@" hit distance"]);
	eye.overlay = make_texture(device, descriptor.transparencyOverlayTextureFormat, descriptor.inputWidth, descriptor.inputHeight, eye.scaler.transparencyOverlayTextureUsage, [label stringByAppendingString:@" overlay"]);
	eye.output = make_texture(device, descriptor.outputTextureFormat, descriptor.outputWidth, descriptor.outputHeight, eye.scaler.outputTextureUsage, [label stringByAppendingString:@" output"]);
	eye.scaler.colorTexture = eye.color;
	eye.scaler.depthTexture = eye.depth;
	eye.scaler.motionTexture = eye.motion;
	eye.scaler.normalTexture = eye.normal;
	eye.scaler.diffuseAlbedoTexture = eye.diffuse;
	eye.scaler.specularAlbedoTexture = eye.specular;
	eye.scaler.roughnessTexture = eye.roughness;
	eye.scaler.denoiseStrengthMaskTexture = eye.denoise;
	eye.scaler.reactiveMaskTexture = eye.reactive;
	eye.scaler.specularHitDistanceTexture = eye.hit_distance;
	eye.scaler.transparencyOverlayTexture = eye.overlay;
	eye.scaler.outputTexture = eye.output;
	eye.scaler.motionVectorScaleX = descriptor.inputWidth;
	eye.scaler.motionVectorScaleY = descriptor.inputHeight;
	eye.scaler.preExposure = 1.0f;
	eye.scaler.depthReversed = YES;
	eye.scaler.viewToClipMatrix = matrix_identity_float4x4;
	return eye;
}

struct GeneratedFrame {
	std::vector<Half4> noisy;
	std::vector<float> depth;
	std::vector<Half2> motion;
	std::vector<Half4> normal;
	std::vector<Half4> diffuse;
	std::vector<Half4> specular;
	std::vector<Half> roughness;
	std::vector<uint8_t> denoise;
	std::vector<uint8_t> reactive;
	std::vector<Half> hit_distance;
	std::vector<Half4> overlay;
	std::vector<float> reference;
};

static GeneratedFrame generate_frame(uint32_t input_width, uint32_t input_height,
		uint32_t output_width, uint32_t output_height, uint32_t frame, uint32_t eye) {
	GeneratedFrame result;
	const size_t input_pixels = input_width * input_height;
	result.noisy.resize(input_pixels);
	result.depth.resize(input_pixels);
	result.motion.resize(input_pixels);
	result.normal.resize(input_pixels);
	result.diffuse.resize(input_pixels);
	result.specular.resize(input_pixels);
	result.roughness.resize(input_pixels);
	result.denoise.resize(input_pixels);
	result.reactive.resize(input_pixels);
	result.hit_distance.resize(input_pixels);
	result.overlay.resize(input_pixels);
	result.reference.resize(size_t(output_width) * output_height * 3);
	for (uint32_t y = 0; y < input_height; y++) {
		for (uint32_t x = 0; x < input_width; x++) {
			const size_t index = size_t(y) * input_width + x;
			const float nx = (float(x) + 0.5f) / input_width * 2.0f - 1.0f;
			const float ny = 1.0f - (float(y) + 0.5f) / input_height * 2.0f;
			const Sample sample = evaluate_clean(nx, ny, frame, eye);
			const float noise = hash_noise(x, y, frame, eye) * (0.22f + sample.specular * 0.18f);
			result.noisy[index] = { Half(std::max(0.0f, sample.r + noise)), Half(std::max(0.0f, sample.g + noise)), Half(std::max(0.0f, sample.b + noise)), Half(1.0f) };
			result.depth[index] = sample.depth;
			result.motion[index] = { Half(sample.motion_x), Half(sample.motion_y) };
			result.normal[index] = { Half(sample.normal_x), Half(sample.normal_y), Half(sample.normal_z), Half(0.0f) };
			result.diffuse[index] = { Half(sample.diffuse_r), Half(sample.diffuse_g), Half(sample.diffuse_b), Half(1.0f) };
			result.specular[index] = { Half(sample.specular), Half(sample.specular), Half(sample.specular), Half(1.0f) };
			result.roughness[index] = Half(sample.roughness);
			result.denoise[index] = uint8_t(std::round(std::clamp(sample.denoise, 0.0f, 1.0f) * 255.0f));
			result.reactive[index] = uint8_t(std::round(std::clamp(sample.reactive, 0.0f, 1.0f) * 255.0f));
			result.hit_distance[index] = Half(sample.hit_distance);
			result.overlay[index] = { Half(sample.overlay_r), Half(sample.overlay_g), Half(sample.overlay_b), Half(sample.overlay_a) };
		}
	}
	for (uint32_t y = 0; y < output_height; y++) {
		for (uint32_t x = 0; x < output_width; x++) {
			const float nx = (float(x) + 0.5f) / output_width * 2.0f - 1.0f;
			const float ny = 1.0f - (float(y) + 0.5f) / output_height * 2.0f;
			const Sample sample = evaluate_clean(nx, ny, frame, eye);
			const size_t index = (size_t(y) * output_width + x) * 3;
			result.reference[index + 0] = sample.r + sample.overlay_r * sample.overlay_a;
			result.reference[index + 1] = sample.g + sample.overlay_g * sample.overlay_a;
			result.reference[index + 2] = sample.b + sample.overlay_b * sample.overlay_a;
		}
	}
	return result;
}

static bool validate_guides(const GeneratedFrame &frame) {
	for (size_t i = 0; i < frame.depth.size(); i++) {
		const float depth = frame.depth[i];
		const float motion_x = float(frame.motion[i].x);
		const float motion_y = float(frame.motion[i].y);
		const float nx = float(frame.normal[i].x);
		const float ny = float(frame.normal[i].y);
		const float nz = float(frame.normal[i].z);
		const float normal_length = std::sqrt(nx * nx + ny * ny + nz * nz);
		if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f ||
				!std::isfinite(motion_x) || !std::isfinite(motion_y) ||
				!std::isfinite(normal_length) || normal_length < 0.95f || normal_length > 1.05f ||
				float(frame.roughness[i]) < 0.0f || float(frame.roughness[i]) > 1.0f ||
				float(frame.hit_distance[i]) < 0.0f) {
			return false;
		}
	}
	return true;
}

static void upload_frame(EyeResources &eye, const GeneratedFrame &frame, uint32_t width, uint32_t height) {
	upload(eye.color, frame.noisy, width, height);
	upload(eye.depth, frame.depth, width, height);
	upload(eye.motion, frame.motion, width, height);
	upload(eye.normal, frame.normal, width, height);
	upload(eye.diffuse, frame.diffuse, width, height);
	upload(eye.specular, frame.specular, width, height);
	upload(eye.roughness, frame.roughness, width, height);
	upload(eye.denoise, frame.denoise, width, height);
	upload(eye.reactive, frame.reactive, width, height);
	upload(eye.hit_distance, frame.hit_distance, width, height);
	upload(eye.overlay, frame.overlay, width, height);
}

static std::vector<Half4> read_output(id<MTLTexture> texture, uint32_t width, uint32_t height) {
	std::vector<Half4> pixels(size_t(width) * height);
	[texture getBytes:pixels.data() bytesPerRow:width * sizeof(Half4) fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
	return pixels;
}

static uint64_t hash_bytes(const uint8_t *bytes, size_t size) {
	uint64_t hash = 14695981039346656037ULL;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static double rmse_output(const std::vector<Half4> &output, const std::vector<float> &reference) {
	double error = 0.0;
	for (size_t i = 0; i < output.size(); i++) {
		const float values[3] = { float(output[i].x), float(output[i].y), float(output[i].z) };
		for (uint32_t channel = 0; channel < 3; channel++) {
			const double difference = double(values[channel]) - reference[i * 3 + channel];
			error += difference * difference;
		}
	}
	return std::sqrt(error / double(output.size() * 3));
}

static bool equal_pixels(const std::vector<Half4> &first, const std::vector<Half4> &second) {
	return first.size() == second.size() &&
			std::memcmp(first.data(), second.data(), first.size() * sizeof(Half4)) == 0;
}

static double rmse_nearest(const GeneratedFrame &frame, uint32_t input_width, uint32_t input_height, uint32_t output_width, uint32_t output_height) {
	double error = 0.0;
	for (uint32_t y = 0; y < output_height; y++) {
		for (uint32_t x = 0; x < output_width; x++) {
			const uint32_t source_x = std::min(input_width - 1, x * input_width / output_width);
			const uint32_t source_y = std::min(input_height - 1, y * input_height / output_height);
			const Half4 source = frame.noisy[size_t(source_y) * input_width + source_x];
			const float values[3] = { float(source.x), float(source.y), float(source.z) };
			const size_t target = (size_t(y) * output_width + x) * 3;
			for (uint32_t channel = 0; channel < 3; channel++) {
				const double difference = double(values[channel]) - frame.reference[target + channel];
				error += difference * difference;
			}
		}
	}
	return std::sqrt(error / double(size_t(output_width) * output_height * 3));
}

static float display_value(float linear_value) {
	return std::pow(std::clamp(linear_value, 0.0f, 1.0f), 1.0f / 2.2f);
}

static double display_rmse_output(const std::vector<Half4> &output, const std::vector<float> &reference) {
	double error = 0.0;
	for (size_t i = 0; i < output.size(); i++) {
		const float values[3] = { float(output[i].x), float(output[i].y), float(output[i].z) };
		for (uint32_t channel = 0; channel < 3; channel++) {
			const double difference = double(display_value(values[channel])) - display_value(reference[i * 3 + channel]);
			error += difference * difference;
		}
	}
	return std::sqrt(error / double(output.size() * 3));
}

static double display_rmse_nearest(const GeneratedFrame &frame, uint32_t input_width, uint32_t input_height, uint32_t output_width, uint32_t output_height) {
	double error = 0.0;
	for (uint32_t y = 0; y < output_height; y++) {
		for (uint32_t x = 0; x < output_width; x++) {
			const uint32_t source_x = std::min(input_width - 1, x * input_width / output_width);
			const uint32_t source_y = std::min(input_height - 1, y * input_height / output_height);
			const Half4 source = frame.noisy[size_t(source_y) * input_width + source_x];
			const float values[3] = { float(source.x), float(source.y), float(source.z) };
			const size_t target = (size_t(y) * output_width + x) * 3;
			for (uint32_t channel = 0; channel < 3; channel++) {
				const double difference = double(display_value(values[channel])) - display_value(frame.reference[target + channel]);
				error += difference * difference;
			}
		}
	}
	return std::sqrt(error / double(size_t(output_width) * output_height * 3));
}

static void write_ppm(const std::string &path, const std::vector<Half4> &pixels, uint32_t width, uint32_t height) {
	std::ofstream output(path, std::ios::binary);
	output << "P6\n" << width << " " << height << "\n255\n";
	for (const Half4 &pixel : pixels) {
		const float values[3] = { float(pixel.x), float(pixel.y), float(pixel.z) };
		unsigned char rgb[3];
		for (uint32_t channel = 0; channel < 3; channel++) {
			rgb[channel] = uint8_t(std::round(std::pow(std::clamp(values[channel], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f));
		}
		output.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
	}
}

static void write_reference_ppm(const std::string &path, const std::vector<float> &pixels, uint32_t width, uint32_t height) {
	std::ofstream output(path, std::ios::binary);
	output << "P6\n" << width << " " << height << "\n255\n";
	for (size_t i = 0; i < size_t(width) * height; i++) {
		unsigned char rgb[3];
		for (uint32_t channel = 0; channel < 3; channel++) {
			rgb[channel] = uint8_t(std::round(std::pow(std::clamp(pixels[i * 3 + channel], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f));
		}
		output.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
	}
}

static void write_nearest_ppm(const std::string &path, const GeneratedFrame &frame,
		uint32_t input_width, uint32_t input_height, uint32_t output_width, uint32_t output_height) {
	std::vector<Half4> pixels(size_t(output_width) * output_height);
	for (uint32_t y = 0; y < output_height; y++) {
		for (uint32_t x = 0; x < output_width; x++) {
			const uint32_t source_x = std::min(input_width - 1, x * input_width / output_width);
			const uint32_t source_y = std::min(input_height - 1, y * input_height / output_height);
			pixels[size_t(y) * output_width + x] = frame.noisy[size_t(source_y) * input_width + source_x];
		}
	}
	write_ppm(path, pixels, output_width, output_height);
}

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		if (argc != 2) {
			fail(@"usage: metalfx_temporal_stereo_quality <output-directory>");
		}
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil || ![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device]) {
			fail(@"selected device lacks MetalFX temporal denoised scaling");
		}
		constexpr uint32_t input_width = 96;
		constexpr uint32_t input_height = 64;
		constexpr uint32_t output_width = 192;
		constexpr uint32_t output_height = 128;
		constexpr uint32_t frame_count = 12;
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
		descriptor.autoExposureEnabled = NO;
		descriptor.reactiveMaskTextureEnabled = YES;
		descriptor.denoiseStrengthMaskTextureEnabled = YES;
		descriptor.specularHitDistanceTextureEnabled = YES;
		descriptor.transparencyOverlayTextureEnabled = YES;
		descriptor.requiresSynchronousInitialization = YES;

		EyeResources left = make_eye(device, descriptor, @"left");
		EyeResources right = make_eye(device, descriptor, @"right");
		EyeResources right_control = make_eye(device, descriptor, @"right control");
		simd_float4x4 left_view = matrix_identity_float4x4;
		simd_float4x4 right_view = matrix_identity_float4x4;
		left_view.columns[3].x = 0.032f;
		right_view.columns[3].x = -0.032f;
		left.scaler.worldToViewMatrix = left_view;
		right.scaler.worldToViewMatrix = right_view;
		right_control.scaler.worldToViewMatrix = right_view;
		id<MTLCommandQueue> queue = [device newCommandQueue];
		double left_gpu_ms = 0.0;
		double right_gpu_ms = 0.0;
		double control_gpu_ms = 0.0;
		double generation_cpu_ms = 0.0;
		double display_noisy_rmse_sum = 0.0;
		double display_output_rmse_sum = 0.0;
		bool guides_valid = true;
		bool right_history_independent = true;
		bool stereo_distinct = true;
		GeneratedFrame last_left_frame;
		GeneratedFrame last_right_frame;
		std::vector<Half4> last_left_output;
		std::vector<Half4> last_right_output;

		for (uint32_t frame = 0; frame < frame_count; frame++) {
			const auto generation_start = std::chrono::steady_clock::now();
			GeneratedFrame left_frame = generate_frame(input_width, input_height, output_width, output_height, frame, 0);
			GeneratedFrame right_frame = generate_frame(input_width, input_height, output_width, output_height, frame, 1);
			generation_cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - generation_start).count();
			guides_valid = guides_valid && validate_guides(left_frame) && validate_guides(right_frame);
			upload_frame(left, left_frame, input_width, input_height);
			upload_frame(right, right_frame, input_width, input_height);
			upload_frame(right_control, right_frame, input_width, input_height);
			const float jitter_x = (frame % 2 == 0 ? -0.25f : 0.25f);
			const float jitter_y = ((frame / 2) % 2 == 0 ? -0.25f : 0.25f);
			for (id<MTLFXTemporalDenoisedScaler> scaler in @[left.scaler, right.scaler, right_control.scaler]) {
				scaler.jitterOffsetX = jitter_x;
				scaler.jitterOffsetY = jitter_y;
			}
			left.scaler.shouldResetHistory = frame == 0 || frame == 6;
			right.scaler.shouldResetHistory = frame == 0;
			right_control.scaler.shouldResetHistory = frame == 0;

			id<MTLCommandBuffer> left_command = [queue commandBuffer];
			[left.scaler encodeToCommandBuffer:left_command];
			[left_command commit];
			[left_command waitUntilCompleted];
			if (left_command.status != MTLCommandBufferStatusCompleted) {
				fail([NSString stringWithFormat:@"left MetalFX frame failed: %@", left_command.error.localizedDescription]);
			}
			left_gpu_ms += (left_command.GPUEndTime - left_command.GPUStartTime) * 1000.0;

			id<MTLCommandBuffer> right_command = [queue commandBuffer];
			[right.scaler encodeToCommandBuffer:right_command];
			[right_command commit];
			[right_command waitUntilCompleted];
			if (right_command.status != MTLCommandBufferStatusCompleted) {
				fail([NSString stringWithFormat:@"right MetalFX frame failed: %@", right_command.error.localizedDescription]);
			}
			right_gpu_ms += (right_command.GPUEndTime - right_command.GPUStartTime) * 1000.0;

			id<MTLCommandBuffer> control_command = [queue commandBuffer];
			[right_control.scaler encodeToCommandBuffer:control_command];
			[control_command commit];
			[control_command waitUntilCompleted];
			if (control_command.status != MTLCommandBufferStatusCompleted) {
				fail([NSString stringWithFormat:@"control MetalFX frame failed: %@", control_command.error.localizedDescription]);
			}
			control_gpu_ms += (control_command.GPUEndTime - control_command.GPUStartTime) * 1000.0;

			const std::vector<Half4> left_output = read_output(left.output, output_width, output_height);
			const std::vector<Half4> right_output = read_output(right.output, output_width, output_height);
			const std::vector<Half4> control_output = read_output(right_control.output, output_width, output_height);
			right_history_independent = right_history_independent && equal_pixels(right_output, control_output);
			stereo_distinct = stereo_distinct && !equal_pixels(left_output, right_output);
			display_noisy_rmse_sum += display_rmse_nearest(left_frame, input_width, input_height, output_width, output_height);
			display_noisy_rmse_sum += display_rmse_nearest(right_frame, input_width, input_height, output_width, output_height);
			display_output_rmse_sum += display_rmse_output(left_output, left_frame.reference);
			display_output_rmse_sum += display_rmse_output(right_output, right_frame.reference);
			if (frame == frame_count - 1) {
				last_left_frame = std::move(left_frame);
				last_right_frame = std::move(right_frame);
				last_left_output = left_output;
				last_right_output = right_output;
			}
		}

		bool output_finite = true;
		for (const Half4 &pixel : last_left_output) {
			output_finite = output_finite && std::isfinite(float(pixel.x)) && std::isfinite(float(pixel.y)) && std::isfinite(float(pixel.z));
		}
		const double left_noisy_rmse = rmse_nearest(last_left_frame, input_width, input_height, output_width, output_height);
		const double right_noisy_rmse = rmse_nearest(last_right_frame, input_width, input_height, output_width, output_height);
		const double left_output_rmse = rmse_output(last_left_output, last_left_frame.reference);
		const double right_output_rmse = rmse_output(last_right_output, last_right_frame.reference);
		const double display_noisy_rmse = display_noisy_rmse_sum / double(frame_count * 2);
		const double display_output_rmse = display_output_rmse_sum / double(frame_count * 2);
		write_ppm(std::string(argv[1]) + "/left_output.ppm", last_left_output, output_width, output_height);
		write_ppm(std::string(argv[1]) + "/right_output.ppm", last_right_output, output_width, output_height);
		write_reference_ppm(std::string(argv[1]) + "/left_reference.ppm", last_left_frame.reference, output_width, output_height);
		write_nearest_ppm(std::string(argv[1]) + "/left_noisy_nearest.ppm", last_left_frame, input_width, input_height, output_width, output_height);
		const uint64_t left_hash = hash_bytes(reinterpret_cast<const uint8_t *>(last_left_output.data()), last_left_output.size() * sizeof(Half4));
		const uint64_t right_hash = hash_bytes(reinterpret_cast<const uint8_t *>(last_right_output.data()), last_right_output.size() * sizeof(Half4));
		const bool linear_hdr_improved = left_output_rmse < left_noisy_rmse && right_output_rmse < right_noisy_rmse;
		const bool quality_improved = display_output_rmse < display_noisy_rmse * 0.8;
		const bool passed = guides_valid && output_finite && quality_improved && stereo_distinct && right_history_independent && left_hash != right_hash;
		std::printf("{\n");
		std::printf("  \"experiment\": \"M0-G-metalfx-temporal-stereo\",\n");
		std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
		std::printf("  \"input_size\": [%u, %u],\n", input_width, input_height);
		std::printf("  \"output_size\": [%u, %u],\n", output_width, output_height);
		std::printf("  \"frames\": %u,\n", frame_count);
		std::printf("  \"challenges\": [\"disocclusion\", \"thin_curves\", \"glossy_motion\", \"articulated_motion\", \"camera_rotation\", \"subpixel_relief\", \"morph_motion\"],\n");
		std::printf("  \"guides_valid\": %s,\n", guides_valid ? "true" : "false");
		std::printf("  \"output_finite\": %s,\n", output_finite ? "true" : "false");
		std::printf("  \"left_only_history_reset_frame\": 6,\n");
		std::printf("  \"right_history_matches_control\": %s,\n", right_history_independent ? "true" : "false");
		std::printf("  \"stereo_outputs_distinct\": %s,\n", stereo_distinct ? "true" : "false");
		std::printf("  \"output_fnv1a64\": [\"%016llx\", \"%016llx\"],\n", static_cast<unsigned long long>(left_hash), static_cast<unsigned long long>(right_hash));
		std::printf("  \"nearest_noisy_rmse\": [%.8f, %.8f],\n", left_noisy_rmse, right_noisy_rmse);
		std::printf("  \"metalfx_rmse\": [%.8f, %.8f],\n", left_output_rmse, right_output_rmse);
		std::printf("  \"linear_hdr_rmse_improved\": %s,\n", linear_hdr_improved ? "true" : "false");
		std::printf("  \"display_sequence_rmse\": {\"nearest_noisy\": %.8f, \"metalfx\": %.8f},\n", display_noisy_rmse, display_output_rmse);
		std::printf("  \"known_quality_issue\": \"%s\",\n", linear_hdr_improved ? "none" : "small_moving_hdr_highlight_energy");
		std::printf("  \"quality_improved\": %s,\n", quality_improved ? "true" : "false");
		std::printf("  \"cpu_ms\": {\"guide_generation_total\": %.6f},\n", generation_cpu_ms);
		std::printf("  \"gpu_ms\": {\"left_total\": %.6f, \"right_total\": %.6f, \"right_control_total\": %.6f},\n", left_gpu_ms, right_gpu_ms, control_gpu_ms);
		std::printf("  \"passed\": %s\n", passed ? "true" : "false");
		std::printf("}\n");
		return passed ? 0 : 2;
	}
}
