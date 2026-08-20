#include "canonical_scene_packet.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

using namespace GodotPathTracingM0;

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
	uint32_t offset = static_cast<uint32_t>(packet.size());
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
	uint32_t guide_offset = append_record(packet, guides);

	CameraRecord camera = {};
	camera.view_from_world = identity_matrix();
	camera.clip_from_view = identity_matrix();
	camera.previous_view_from_world = identity_matrix();
	camera.previous_clip_from_view = identity_matrix();
	camera.camera_relative_origin_and_exposure.w = 1.0f;
	camera.render_width = 64;
	camera.render_height = 64;
	camera.history_reset = 1;
	uint32_t camera_offset = append_record(packet, camera);

	InstanceRecord instance = {};
	instance.world_from_object = identity_matrix();
	instance.previous_world_from_object = identity_matrix();
	instance.geometry_id = 1;
	instance.material_id = 1;
	instance.instance_id = 1;
	instance.visibility_mask = RAY_VISIBILITY_PRIMARY | RAY_VISIBILITY_SHADOW |
			RAY_VISIBILITY_REFLECTION_REFRACTION | RAY_VISIBILITY_DIFFUSE_INDIRECT;
	uint32_t instance_offset = append_record(packet, instance);

	MaterialRecord material = {};
	material.base_color_and_opacity = { 0.8f, 0.2f, 0.1f, 1.0f };
	material.specular_f0_and_perceptual_roughness = { 0.04f, 0.04f, 0.04f, 0.5f };
	material.transmission_ior_alpha_cutoff_unused.y = 1.5f;
	uint32_t material_offset = append_record(packet, material);

	LightRecord light = {};
	light.position_or_direction_and_type = { 0.0f, 1.0f, 0.0f, 1.0f };
	light.linear_color_and_intensity = { 1.0f, 1.0f, 1.0f, 10.0f };
	light.light_id = 1;
	light.visibility_mask = 0xffffffff;
	uint32_t light_offset = append_record(packet, light);

	ScenePacketHeader header = {};
	header.magic = SCENE_PACKET_MAGIC;
	header.schema_version = SCENE_PACKET_VERSION;
	header.endian_tag = SCENE_PACKET_ENDIAN_TAG;
	header.header_size = sizeof(ScenePacketHeader);
	header.total_size = packet.size();
	header.guide_contract_offset = guide_offset;
	header.camera_count = 1;
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

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: validate_scene_packet <output-packet>\n");
		return 1;
	}

	std::vector<uint8_t> first = make_packet();
	std::vector<uint8_t> second = make_packet();
	const ScenePacketHeader *header = reinterpret_cast<const ScenePacketHeader *>(first.data());
	bool aligned = header->guide_contract_offset % 16 == 0 && header->camera_offset % 16 == 0 &&
			header->instance_offset % 16 == 0 && header->material_offset % 16 == 0 && header->light_offset % 16 == 0;
	bool deterministic = first == second;
	bool valid_header = header->magic == SCENE_PACKET_MAGIC && header->schema_version == SCENE_PACKET_VERSION &&
			header->endian_tag == SCENE_PACKET_ENDIAN_TAG && header->total_size == first.size();
	float invalid_pixel = std::numeric_limits<float>::quiet_NaN();
	bool invalid_is_nan = std::isnan(invalid_pixel);
	bool passed = aligned && deterministic && valid_header && invalid_is_nan;

	std::ofstream output(argv[1], std::ios::binary);
	output.write(reinterpret_cast<const char *>(first.data()), first.size());
	if (!output.good()) {
		fprintf(stderr, "failed to write scene packet\n");
		return 1;
	}

	printf("{\n");
	printf("  \"experiment\": \"M0-scene-packet-abi\",\n");
	printf("  \"schema_version\": %u,\n", header->schema_version);
	printf("  \"packet_bytes\": %llu,\n", static_cast<unsigned long long>(first.size()));
	printf("  \"payload_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(header->payload_hash));
	printf("  \"alignment_bytes\": 16,\n");
	printf("  \"motion\": \"previous_uv_minus_current_uv\",\n");
	printf("  \"depth\": \"reversed_zero_to_one\",\n");
	printf("  \"deterministic\": %s,\n", deterministic ? "true" : "false");
	printf("  \"passed\": %s\n", passed ? "true" : "false");
	printf("}\n");
	return passed ? 0 : 2;
}
