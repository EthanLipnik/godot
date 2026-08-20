#include "canonical_scene_packet.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace GodotPathTracingM0;

static constexpr uint64_t GOLDEN_PAYLOAD_HASH = 0x1136ee6ec9ba3139ULL;

static Matrix4 identity_matrix() {
	Matrix4 matrix = {};
	matrix.columns[0].x = 1.0f;
	matrix.columns[1].y = 1.0f;
	matrix.columns[2].z = 1.0f;
	matrix.columns[3].w = 1.0f;
	return matrix;
}

template <typename T>
static uint32_t append_record(std::vector<uint8_t> &packet, const T &record) {
	const uint32_t offset = static_cast<uint32_t>(packet.size());
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
	packet.insert(packet.end(), bytes, bytes + sizeof(T));
	return offset;
}

static uint64_t fnv1a64(const uint8_t *bytes, size_t size) {
	uint64_t hash = 14695981039346656037ULL;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

template <typename T>
static bool read_record(const std::vector<uint8_t> &packet, size_t offset, T &record) {
	if (offset > packet.size() || sizeof(T) > packet.size() - offset) {
		return false;
	}
	std::memcpy(&record, packet.data() + offset, sizeof(T));
	return true;
}

static bool finite_float4(const Float4 &value) {
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

static bool finite_matrix(const Matrix4 &matrix) {
	for (const Float4 &column : matrix.columns) {
		if (!finite_float4(column)) {
			return false;
		}
	}
	return true;
}

static bool checked_section(const std::vector<uint8_t> &packet, uint32_t offset, uint32_t count, size_t stride) {
	if ((offset & 15U) != 0 || offset < sizeof(ScenePacketHeader)) {
		return false;
	}
	if (count != 0 && stride > (std::numeric_limits<size_t>::max() / count)) {
		return false;
	}
	const size_t bytes = static_cast<size_t>(count) * stride;
	return offset <= packet.size() && bytes <= packet.size() - offset;
}

static bool validate_packet(const std::vector<uint8_t> &packet, std::string &failure) {
	ScenePacketHeader header = {};
	if (!read_record(packet, 0, header)) {
		failure = "truncated header";
		return false;
	}
	if (header.magic != SCENE_PACKET_MAGIC || header.schema_version != SCENE_PACKET_VERSION ||
			header.endian_tag != SCENE_PACKET_ENDIAN_TAG || header.header_size != sizeof(ScenePacketHeader)) {
		failure = "invalid header contract";
		return false;
	}
	if (header.total_size != packet.size()) {
		failure = "invalid total size";
		return false;
	}
	if (header.payload_hash != fnv1a64(packet.data() + sizeof(ScenePacketHeader), packet.size() - sizeof(ScenePacketHeader))) {
		failure = "payload hash mismatch";
		return false;
	}
	if (!checked_section(packet, header.guide_contract_offset, 1, sizeof(GuideContract)) ||
			!checked_section(packet, header.camera_offset, header.camera_count, sizeof(CameraRecord)) ||
			!checked_section(packet, header.instance_offset, header.instance_count, sizeof(InstanceRecord)) ||
			!checked_section(packet, header.material_offset, header.material_count, sizeof(MaterialRecord)) ||
			!checked_section(packet, header.light_offset, header.light_count, sizeof(LightRecord))) {
		failure = "invalid section bounds or alignment";
		return false;
	}
	if (header.camera_count == 0 || header.camera_count > 2) {
		failure = "unsupported camera count";
		return false;
	}

	GuideContract guides = {};
	if (!read_record(packet, header.guide_contract_offset, guides)) {
		failure = "missing guide contract";
		return false;
	}
	constexpr uint32_t known_guides = (1U << 10) - 1;
	if (guides.schema_version != 1 || guides.motion_direction != MOTION_CURRENT_TO_PREVIOUS ||
			guides.motion_units != MOTION_NORMALIZED_UV || guides.depth_convention != DEPTH_REVERSED_ZERO_TO_ONE ||
			guides.normal_space != NORMAL_WORLD_SPACE || guides.roughness_convention != ROUGHNESS_PERCEPTUAL_ZERO_TO_ONE ||
			guides.invalid_pixel_convention != INVALID_PIXEL_QUIET_NAN || guides.uv_origin != UV_ORIGIN_TOP_LEFT ||
			guides.color_space != COLOR_LINEAR_REC709 || (guides.enabled_guides & ~known_guides) != 0 ||
			!std::isfinite(guides.motion_to_pixel_scale_x) || !std::isfinite(guides.motion_to_pixel_scale_y) ||
			guides.motion_to_pixel_scale_x <= 0.0f || guides.motion_to_pixel_scale_y <= 0.0f) {
		failure = "invalid guide semantics";
		return false;
	}

	bool seen_views[2] = { false, false };
	for (uint32_t i = 0; i < header.camera_count; i++) {
		CameraRecord camera = {};
		if (!read_record(packet, header.camera_offset + static_cast<size_t>(i) * sizeof(CameraRecord), camera) ||
				camera.view_index >= header.camera_count || seen_views[camera.view_index] ||
				camera.render_width == 0 || camera.render_height == 0 || camera.history_reset > 1 ||
				!finite_matrix(camera.view_from_world) || !finite_matrix(camera.clip_from_view) ||
				!finite_matrix(camera.previous_view_from_world) || !finite_matrix(camera.previous_clip_from_view) ||
				!finite_float4(camera.camera_relative_origin_and_exposure)) {
			failure = "invalid camera record";
			return false;
		}
		seen_views[camera.view_index] = true;
	}

	for (uint32_t i = 0; i < header.material_count; i++) {
		MaterialRecord material = {};
		if (!read_record(packet, header.material_offset + static_cast<size_t>(i) * sizeof(MaterialRecord), material) ||
				!finite_float4(material.base_color_and_opacity) || !finite_float4(material.emission_and_strength) ||
				!finite_float4(material.specular_f0_and_perceptual_roughness) ||
				!finite_float4(material.transmission_ior_alpha_cutoff_unused) ||
				material.specular_f0_and_perceptual_roughness.w < 0.0f ||
				material.specular_f0_and_perceptual_roughness.w > 1.0f) {
			failure = "invalid material record";
			return false;
		}
	}
	return true;
}

static void refresh_payload_hash(std::vector<uint8_t> &packet) {
	ScenePacketHeader header = {};
	std::memcpy(&header, packet.data(), sizeof(header));
	header.payload_hash = fnv1a64(packet.data() + sizeof(ScenePacketHeader), packet.size() - sizeof(ScenePacketHeader));
	std::memcpy(packet.data(), &header, sizeof(header));
}

static std::vector<uint8_t> make_packet() {
	std::vector<uint8_t> packet(sizeof(ScenePacketHeader), 0);
	GuideContract guides = {};
	guides.schema_version = 1;
	guides.motion_direction = MOTION_CURRENT_TO_PREVIOUS;
	guides.motion_units = MOTION_NORMALIZED_UV;
	guides.depth_convention = DEPTH_REVERSED_ZERO_TO_ONE;
	guides.normal_space = NORMAL_WORLD_SPACE;
	guides.roughness_convention = ROUGHNESS_PERCEPTUAL_ZERO_TO_ONE;
	guides.invalid_pixel_convention = INVALID_PIXEL_QUIET_NAN;
	guides.uv_origin = UV_ORIGIN_TOP_LEFT;
	guides.color_space = COLOR_LINEAR_REC709;
	guides.enabled_guides = GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO |
			GUIDE_SPECULAR_ALBEDO | GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH |
			GUIDE_REACTIVE_MASK | GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY;
	guides.motion_to_pixel_scale_x = 64.0f;
	guides.motion_to_pixel_scale_y = 64.0f;
	const uint32_t guide_offset = append_record(packet, guides);

	CameraRecord cameras[2] = {};
	for (uint32_t view = 0; view < 2; view++) {
		cameras[view].view_from_world = identity_matrix();
		cameras[view].clip_from_view = identity_matrix();
		cameras[view].previous_view_from_world = identity_matrix();
		cameras[view].previous_clip_from_view = identity_matrix();
		cameras[view].camera_relative_origin_and_exposure.x = view == 0 ? -0.032f : 0.032f;
		cameras[view].camera_relative_origin_and_exposure.w = 1.0f;
		cameras[view].view_index = view;
		cameras[view].render_width = 64;
		cameras[view].render_height = 64;
		cameras[view].history_reset = 1;
	}
	const uint32_t camera_offset = append_record(packet, cameras);

	InstanceRecord instance = {};
	instance.world_from_object = identity_matrix();
	instance.previous_world_from_object = identity_matrix();
	instance.geometry_id = 1;
	instance.material_id = 1;
	instance.instance_id = 1;
	instance.visibility_mask = RAY_VISIBILITY_PRIMARY | RAY_VISIBILITY_SHADOW |
			RAY_VISIBILITY_REFLECTION_REFRACTION | RAY_VISIBILITY_DIFFUSE_INDIRECT;
	const uint32_t instance_offset = append_record(packet, instance);

	MaterialRecord material = {};
	material.base_color_and_opacity = { 0.8f, 0.2f, 0.1f, 1.0f };
	material.specular_f0_and_perceptual_roughness = { 0.04f, 0.04f, 0.04f, 0.5f };
	material.transmission_ior_alpha_cutoff_unused.y = 1.5f;
	const uint32_t material_offset = append_record(packet, material);

	LightRecord light = {};
	light.position_or_direction_and_type = { 0.0f, 1.0f, 0.0f, 1.0f };
	light.linear_color_and_intensity = { 1.0f, 1.0f, 1.0f, 10.0f };
	light.light_id = 1;
	light.visibility_mask = 0xffffffff;
	const uint32_t light_offset = append_record(packet, light);

	ScenePacketHeader header = {};
	header.magic = SCENE_PACKET_MAGIC;
	header.schema_version = SCENE_PACKET_VERSION;
	header.endian_tag = SCENE_PACKET_ENDIAN_TAG;
	header.header_size = sizeof(ScenePacketHeader);
	header.total_size = packet.size();
	header.guide_contract_offset = guide_offset;
	header.camera_count = 2;
	header.camera_offset = camera_offset;
	header.instance_count = 1;
	header.instance_offset = instance_offset;
	header.material_count = 1;
	header.material_offset = material_offset;
	header.light_count = 1;
	header.light_offset = light_offset;
	header.payload_hash = fnv1a64(packet.data() + sizeof(ScenePacketHeader), packet.size() - sizeof(ScenePacketHeader));
	std::memcpy(packet.data(), &header, sizeof(header));
	return packet;
}

static bool corruption_is_rejected(const std::vector<uint8_t> &source, const char *kind) {
	std::vector<uint8_t> packet = source;
	ScenePacketHeader header = {};
	std::memcpy(&header, packet.data(), sizeof(header));
	if (std::strcmp(kind, "magic") == 0) {
		packet[0] ^= 0xff;
	} else if (std::strcmp(kind, "payload") == 0) {
		packet.back() ^= 0x01;
	} else if (std::strcmp(kind, "total_size") == 0) {
		header.total_size++;
		std::memcpy(packet.data(), &header, sizeof(header));
	} else if (std::strcmp(kind, "misaligned_camera") == 0) {
		header.camera_offset++;
		std::memcpy(packet.data(), &header, sizeof(header));
	} else if (std::strcmp(kind, "camera_count") == 0) {
		header.camera_count = std::numeric_limits<uint32_t>::max();
		std::memcpy(packet.data(), &header, sizeof(header));
	} else if (std::strcmp(kind, "guide_enum") == 0) {
		GuideContract guides = {};
		std::memcpy(&guides, packet.data() + header.guide_contract_offset, sizeof(guides));
		guides.motion_direction = 0xffffffff;
		std::memcpy(packet.data() + header.guide_contract_offset, &guides, sizeof(guides));
		refresh_payload_hash(packet);
	} else if (std::strcmp(kind, "truncated") == 0) {
		packet.resize(packet.size() - 1);
	}
	std::string failure;
	return !validate_packet(packet, failure);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: validate_scene_packet <output-packet>\n");
		return 1;
	}
	const std::vector<uint8_t> first = make_packet();
	const std::vector<uint8_t> second = make_packet();
	ScenePacketHeader header = {};
	std::memcpy(&header, first.data(), sizeof(header));
	std::string failure;
	const bool valid = validate_packet(first, failure);
	const bool deterministic = first == second;
	const char *corruptions[] = { "magic", "payload", "total_size", "misaligned_camera", "camera_count", "guide_enum", "truncated" };
	uint32_t rejected = 0;
	for (const char *corruption : corruptions) {
		rejected += corruption_is_rejected(first, corruption) ? 1U : 0U;
	}
	const bool golden_hash_matches = GOLDEN_PAYLOAD_HASH != 0 && header.payload_hash == GOLDEN_PAYLOAD_HASH;
	const bool invalid_is_nan = std::isnan(std::numeric_limits<float>::quiet_NaN());
	const bool passed = valid && deterministic && golden_hash_matches && invalid_is_nan &&
			rejected == (sizeof(corruptions) / sizeof(corruptions[0]));

	std::ofstream output(argv[1], std::ios::binary);
	output.write(reinterpret_cast<const char *>(first.data()), first.size());
	if (!output.good()) {
		std::fprintf(stderr, "failed to write scene packet\n");
		return 1;
	}
	std::printf("{\n");
	std::printf("  \"experiment\": \"M0-J-scene-packet-abi\",\n");
	std::printf("  \"schema_version\": %u,\n", header.schema_version);
	std::printf("  \"packet_bytes\": %llu,\n", static_cast<unsigned long long>(first.size()));
	std::printf("  \"payload_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(header.payload_hash));
	std::printf("  \"golden_hash_matches\": %s,\n", golden_hash_matches ? "true" : "false");
	std::printf("  \"camera_count\": %u,\n", header.camera_count);
	std::printf("  \"corruptions_rejected\": %u,\n", rejected);
	std::printf("  \"corruption_cases\": %llu,\n", static_cast<unsigned long long>(sizeof(corruptions) / sizeof(corruptions[0])));
	std::printf("  \"motion\": \"previous_uv_minus_current_uv\",\n");
	std::printf("  \"depth\": \"reversed_zero_to_one\",\n");
	std::printf("  \"deterministic\": %s,\n", deterministic ? "true" : "false");
	std::printf("  \"validation_error\": \"%s\",\n", failure.c_str());
	std::printf("  \"passed\": %s\n", passed ? "true" : "false");
	std::printf("}\n");
	return passed ? 0 : 2;
}
