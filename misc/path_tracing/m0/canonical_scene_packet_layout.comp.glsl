#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

struct PacketMatrix4 { vec4 columns[4]; };
struct PacketGuideContract {
	uint schema_version; uint motion_direction; uint motion_units; uint depth_convention;
	uint normal_space; uint roughness_convention; uint invalid_pixel_convention; uint uv_origin;
	uint color_space; uint enabled_guides; float motion_to_pixel_scale_x; float motion_to_pixel_scale_y;
	uint reserved[4];
};
struct PacketCameraRecord {
	PacketMatrix4 view_from_world; PacketMatrix4 clip_from_view;
	PacketMatrix4 previous_view_from_world; PacketMatrix4 previous_clip_from_view;
	vec4 camera_relative_origin_and_exposure;
	uint view_index; uint render_width; uint render_height; uint history_reset;
};
struct PacketInstanceRecord {
	PacketMatrix4 world_from_object; PacketMatrix4 previous_world_from_object;
	uint64_t geometry_id; uint64_t material_id;
	uint instance_id; uint visibility_mask; uint flags; uint reserved;
};
struct PacketMaterialRecord {
	vec4 base_color_and_opacity; vec4 emission_and_strength;
	vec4 specular_f0_and_perceptual_roughness; vec4 transmission_ior_alpha_cutoff_unused;
	uint base_color_texture; uint normal_texture; uint metallic_roughness_texture; uint emission_texture;
};
struct PacketLightRecord {
	vec4 position_or_direction_and_type; vec4 linear_color_and_intensity; vec4 shape_parameters;
	uint light_id; uint visibility_mask; uint flags; uint reserved;
};
struct PacketSceneHeader {
	uint magic; uint schema_version; uint endian_tag; uint header_size;
	uint64_t total_size; uint64_t payload_hash;
	uint guide_contract_offset; uint camera_count; uint camera_offset; uint instance_count;
	uint instance_offset; uint material_count; uint material_offset; uint light_count; uint light_offset;
	uint reserved[3];
};

layout(std430, set = 0, binding = 0) buffer PacketLayoutProbe {
	PacketSceneHeader header;
	PacketGuideContract guides;
	PacketCameraRecord camera;
	PacketInstanceRecord instance_record;
	PacketMaterialRecord material;
	PacketLightRecord light;
	uint result;
} packet;

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main() {
	packet.result = packet.header.magic + packet.guides.schema_version + packet.camera.view_index +
		packet.instance_record.instance_id + packet.material.base_color_texture + packet.light.light_id;
}
