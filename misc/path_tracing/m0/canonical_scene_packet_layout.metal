#include <metal_stdlib>

using namespace metal;

struct alignas(16) PacketFloat4 { float x; float y; float z; float w; };
struct alignas(16) PacketMatrix4 { PacketFloat4 columns[4]; };
struct alignas(16) PacketGuideContract {
	uint schema_version; uint motion_direction; uint motion_units; uint depth_convention;
	uint normal_space; uint roughness_convention; uint invalid_pixel_convention; uint uv_origin;
	uint color_space; uint enabled_guides; float motion_to_pixel_scale_x; float motion_to_pixel_scale_y;
	uint reserved[4];
};
struct alignas(16) PacketCameraRecord {
	PacketMatrix4 view_from_world; PacketMatrix4 clip_from_view;
	PacketMatrix4 previous_view_from_world; PacketMatrix4 previous_clip_from_view;
	PacketFloat4 camera_relative_origin_and_exposure;
	uint view_index; uint render_width; uint render_height; uint history_reset;
};
struct alignas(16) PacketInstanceRecord {
	PacketMatrix4 world_from_object; PacketMatrix4 previous_world_from_object;
	ulong geometry_id; ulong material_id;
	uint instance_id; uint visibility_mask; uint flags; uint reserved;
};
struct alignas(16) PacketMaterialRecord {
	PacketFloat4 base_color_and_opacity; PacketFloat4 emission_and_strength;
	PacketFloat4 specular_f0_and_perceptual_roughness; PacketFloat4 transmission_ior_alpha_cutoff_unused;
	uint base_color_texture; uint normal_texture; uint metallic_roughness_texture; uint emission_texture;
};
struct alignas(16) PacketLightRecord {
	PacketFloat4 position_or_direction_and_type; PacketFloat4 linear_color_and_intensity; PacketFloat4 shape_parameters;
	uint light_id; uint visibility_mask; uint flags; uint reserved;
};
struct alignas(16) PacketSceneHeader {
	uint magic; uint schema_version; uint endian_tag; uint header_size;
	ulong total_size; ulong payload_hash;
	uint guide_contract_offset; uint camera_count; uint camera_offset; uint instance_count;
	uint instance_offset; uint material_count; uint material_offset; uint light_count; uint light_offset;
	uint reserved[3];
};

kernel void report_packet_layout(device uint *values [[buffer(0)]]) {
	values[0] = sizeof(PacketFloat4);
	values[1] = sizeof(PacketMatrix4);
	values[2] = sizeof(PacketGuideContract);
	values[3] = sizeof(PacketCameraRecord);
	values[4] = sizeof(PacketInstanceRecord);
	values[5] = sizeof(PacketMaterialRecord);
	values[6] = sizeof(PacketLightRecord);
	values[7] = sizeof(PacketSceneHeader);
	values[8] = __builtin_offsetof(PacketCameraRecord, view_index);
	values[9] = __builtin_offsetof(PacketInstanceRecord, geometry_id);
	values[10] = __builtin_offsetof(PacketSceneHeader, total_size);
	values[11] = __builtin_offsetof(PacketSceneHeader, guide_contract_offset);
}
