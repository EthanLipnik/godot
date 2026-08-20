/**************************************************************************/
/*  test_path_tracing_scene_compiler.cpp                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"
#include "tests/test_utils.h"

TEST_FORCE_LINK(test_path_tracing_scene_compiler)

#include "editor/export/remote_windows_launch_plan.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/environment.h"
#include "scene/resources/path_tracing_resource_compiler.h"
#include "scene/resources/path_tracing_scene_state.h"
#include "servers/rendering/rendering_device.h"

#ifdef METAL_ENABLED
#include "drivers/metal/rendering_context_driver_metal.h"
#include "servers/rendering/renderer_rd/effects/metal_fx.h"
#include "servers/rendering/renderer_rd/effects/metal_path_tracing.h"
#endif
#include "servers/rendering/path_tracing/path_tracing_backend.h"
#include "servers/rendering/path_tracing/path_tracing_guide_validator.h"
#include "servers/rendering/path_tracing/path_tracing_scene_capture.h"
#include "servers/rendering/path_tracing/path_tracing_scene_compiler.h"

namespace TestPathTracingSceneCompiler {

using namespace RendererPathTracing;

static Matrix4 identity_matrix() {
	Matrix4 matrix = {};
	matrix.columns[0].x = 1.0f;
	matrix.columns[1].y = 1.0f;
	matrix.columns[2].z = 1.0f;
	matrix.columns[3].w = 1.0f;
	return matrix;
}

static ScenePacketInput make_input() {
	ScenePacketInput input;
	input.guide_contract.schema_version = SCENE_PACKET_VERSION;
	input.guide_contract.motion_direction = MOTION_CURRENT_TO_PREVIOUS;
	input.guide_contract.motion_units = MOTION_NORMALIZED_UV;
	input.guide_contract.depth_convention = DEPTH_REVERSED_ZERO_TO_ONE;
	input.guide_contract.normal_space = NORMAL_WORLD_SPACE;
	input.guide_contract.roughness_convention = ROUGHNESS_PERCEPTUAL_ZERO_TO_ONE;
	input.guide_contract.invalid_pixel_convention = INVALID_PIXEL_QUIET_NAN;
	input.guide_contract.uv_origin = UV_ORIGIN_TOP_LEFT;
	input.guide_contract.color_space = COLOR_LINEAR_REC709;
	input.guide_contract.enabled_guides = GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO |
			GUIDE_SPECULAR_ALBEDO | GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH | GUIDE_REACTIVE_MASK |
			GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY;
	input.guide_contract.motion_to_pixel_scale_x = 64.0f;
	input.guide_contract.motion_to_pixel_scale_y = 64.0f;

	for (uint32_t view = 0; view < 2; view++) {
		CameraRecord camera = {};
		camera.view_from_world = identity_matrix();
		camera.clip_from_view = identity_matrix();
		camera.previous_view_from_world = identity_matrix();
		camera.previous_clip_from_view = identity_matrix();
		camera.camera_relative_origin_and_exposure.x = view == 0 ? -0.032f : 0.032f;
		camera.camera_relative_origin_and_exposure.w = 1.0f;
		camera.view_index = view;
		camera.render_width = 64;
		camera.render_height = 64;
		camera.history_reset = 1;
		input.cameras.push_back(camera);
	}

	InstanceRecord instance = {};
	instance.world_from_object = identity_matrix();
	instance.previous_world_from_object = identity_matrix();
	instance.geometry_id = 1;
	instance.material_id = 1;
	instance.instance_id = 1;
	instance.visibility_mask = RAY_VISIBILITY_PRIMARY | RAY_VISIBILITY_SHADOW |
			RAY_VISIBILITY_REFLECTION_REFRACTION | RAY_VISIBILITY_DIFFUSE_INDIRECT;
	input.instances.push_back(instance);

	MaterialRecord material = {};
	material.base_color_and_opacity = { 0.8f, 0.2f, 0.1f, 1.0f };
	material.specular_f0_and_perceptual_roughness = { 0.04f, 0.04f, 0.04f, 0.5f };
	material.transmission_ior_alpha_cutoff_unused.y = 1.5f;
	input.materials.push_back(material);

	LightRecord light = {};
	light.position_or_direction_and_type = { 0.0f, 1.0f, 0.0f, 1.0f };
	light.linear_color_and_intensity = { 1.0f, 1.0f, 1.0f, 10.0f };
	light.light_id = 1;
	light.visibility_mask = 0xffffffff;
	input.lights.push_back(light);
	return input;
}

static SceneCaptureInput make_capture_input() {
	SceneCaptureInput input;
	input.scene = make_input();
	GeometryInput geometry;
	geometry.geometry_id = 1;
	geometry.flags = GEOMETRY_DYNAMIC | GEOMETRY_OPAQUE;
	geometry.bounds_min = { -1.0f, -1.0f, 0.0f, 0.0f };
	geometry.bounds_max = { 1.0f, 1.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < 3; i++) {
		GeometryVertex vertex = {};
		vertex.current_position = { i == 0 ? -1.0f : (i == 1 ? 1.0f : 0.0f), i == 2 ? 1.0f : -1.0f, 0.0f, 1.0f };
		vertex.previous_position = vertex.current_position;
		vertex.normal = { 0.0f, 0.0f, 1.0f, 0.0f };
		vertex.tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		vertex.uv = { i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, 0.0f, 0.0f };
		geometry.vertices.push_back(vertex);
	}
	geometry.indices.push_back(0);
	geometry.indices.push_back(1);
	geometry.indices.push_back(2);
	input.geometries.push_back(geometry);
	return input;
}

static SceneCaptureInput make_render_capture_input() {
	SceneCaptureInput input = make_capture_input();
	const float near_plane = 0.1f;
	const float far_plane = 100.0f;
	const float projection_a = near_plane / (far_plane - near_plane);
	const float projection_b = near_plane * far_plane / (far_plane - near_plane);
	for (uint32_t view = 0; view < 2; view++) {
		CameraRecord &camera = input.scene.cameras.write[view];
		const float eye_x = view == 0 ? -0.032f : 0.032f;
		camera.view_from_world = identity_matrix();
		camera.view_from_world.columns[3].x = -eye_x;
		camera.view_from_world.columns[3].z = -2.0f;
		camera.clip_from_view = {};
		camera.clip_from_view.columns[0].x = 1.0f;
		camera.clip_from_view.columns[1].y = 1.0f;
		camera.clip_from_view.columns[2].z = projection_a;
		camera.clip_from_view.columns[2].w = -1.0f;
		camera.clip_from_view.columns[3].z = projection_b;
		camera.previous_view_from_world = camera.view_from_world;
		camera.previous_clip_from_view = camera.clip_from_view;
		camera.camera_relative_origin_and_exposure.x = 0.0f;
	}
	return input;
}

static Ref<ArrayMesh> make_array_mesh(const Ref<Material> &p_material) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices;
	vertices.push_back(Vector3(-1, -1, 0));
	vertices.push_back(Vector3(1, -1, 0));
	vertices.push_back(Vector3(0, 1, 0));
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	PackedVector3Array normals;
	normals.push_back(Vector3(0, 0, 1));
	normals.push_back(Vector3(0, 0, 1));
	normals.push_back(Vector3(0, 0, 1));
	arrays[Mesh::ARRAY_NORMAL] = normals;
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, p_material);
	return mesh;
}

static Ref<ArrayMesh> make_skinned_blend_shape_mesh(const Ref<Material> &p_material) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_blend_shape("Silhouette");
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices;
	vertices.push_back(Vector3(-1, -1, 0));
	vertices.push_back(Vector3(1, -1, 0));
	vertices.push_back(Vector3(0, 1, 0));
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	PackedVector3Array normals;
	for (uint32_t vertex = 0; vertex < 3; vertex++) {
		normals.push_back(Vector3(0, 0, 1));
	}
	arrays[Mesh::ARRAY_NORMAL] = normals;
	PackedInt32Array bones;
	PackedFloat32Array weights;
	for (uint32_t vertex = 0; vertex < 3; vertex++) {
		for (uint32_t influence = 0; influence < 8; influence++) {
			bones.push_back(0);
			weights.push_back(influence == 0 ? 1.0f : 0.0f);
		}
	}
	arrays[Mesh::ARRAY_BONES] = bones;
	arrays[Mesh::ARRAY_WEIGHTS] = weights;
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	arrays[Mesh::ARRAY_INDEX] = indices;
	Array blend_arrays;
	blend_arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array blend_vertices;
	blend_vertices.push_back(Vector3(1, 0, 0));
	blend_vertices.push_back(Vector3());
	blend_vertices.push_back(Vector3());
	blend_arrays[Mesh::ARRAY_VERTEX] = blend_vertices;
	PackedVector3Array blend_normals;
	for (uint32_t vertex = 0; vertex < 3; vertex++) {
		blend_normals.push_back(Vector3());
	}
	blend_arrays[Mesh::ARRAY_NORMAL] = blend_normals;
	TypedArray<Array> blend_shapes;
	blend_shapes.push_back(blend_arrays);
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, blend_shapes, Dictionary(), Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS);
	mesh->surface_set_material(0, p_material);
	return mesh;
}

class TestBackend : public Backend {
	BackendCapabilities capabilities;

public:
	TestBackend(bool p_available, uint32_t p_max_views) {
		capabilities.available = p_available;
		capabilities.max_views = p_max_views;
		capabilities.unavailable_reason = "Deliberately unavailable test backend.";
	}

	StringName get_name() const override { return "test"; }
	BackendCapabilities get_capabilities() const override { return capabilities; }
	Error render(const FrameRequest &p_request, FrameResult &r_result, String *r_error) override {
		(void)r_result;
		return validate_request(p_request, r_error);
	}
};

TEST_CASE("[PathTracing] Scene compiler reproduces the frozen schema-1 packet") {
	PackedByteArray first;
	PackedByteArray second;
	String error;
	CHECK_EQ(SceneCompiler::compile(make_input(), first, &error), OK);
	CHECK(error.is_empty());
	CHECK_EQ(SceneCompiler::compile(make_input(), second, &error), OK);
	CHECK_EQ(first, second);
	CHECK_EQ(first.size(), 1024);

	ScenePacketHeader header = {};
	CHECK_EQ(SceneCompiler::read_record(first, 0, header), OK);
	CHECK_EQ(header.payload_hash, 0x1136ee6ec9ba3139ULL);
	CHECK_EQ(header.camera_count, 2);
	CHECK_EQ(header.guide_contract_offset, 80);
	CHECK_EQ(header.camera_offset, 144);
	CHECK_EQ(header.instance_offset, 720);
	CHECK_EQ(header.material_offset, 880);
	CHECK_EQ(header.light_offset, 960);
}

TEST_CASE("[PathTracing] Scene compiler rejects damaged and ambiguous packets") {
	PackedByteArray packet;
	String error;
	CHECK_EQ(SceneCompiler::compile(make_input(), packet, &error), OK);
	packet.write[packet.size() - 1] ^= 1;
	CHECK_EQ(SceneCompiler::validate(packet, &error), ERR_FILE_CORRUPT);
	CHECK(!error.is_empty());

	ScenePacketInput invalid_input = make_input();
	invalid_input.instances.write[0].material_id = 2;
	CHECK_EQ(SceneCompiler::compile(invalid_input, packet, &error), ERR_FILE_CORRUPT);
	CHECK(packet.is_empty());
}

TEST_CASE("[PathTracing] Scene compiler requires unique ordered stereo views") {
	ScenePacketInput input = make_input();
	input.cameras.write[1].view_index = 0;
	PackedByteArray packet;
	String error;
	CHECK_EQ(SceneCompiler::compile(input, packet, &error), ERR_FILE_CORRUPT);
	CHECK(packet.is_empty());
}

TEST_CASE("[PathTracing] Scene packets replay byte-identically") {
	PackedByteArray source;
	PackedByteArray replay;
	String error;
	CHECK_EQ(SceneCompiler::compile(make_input(), source, &error), OK);
	const String capture_path = TestUtils::get_temp_path("path_tracing_scene_packet.bin");
	CHECK_EQ(SceneCompiler::save_packet(capture_path, source, &error), OK);
	CHECK_EQ(SceneCompiler::load_packet(capture_path, replay, &error), OK);
	CHECK_EQ(replay, source);
}

TEST_CASE("[PathTracing] Self-contained scene captures preserve geometry and packet bytes") {
	PackedByteArray first;
	PackedByteArray second;
	PackedByteArray packet;
	String error;
	CHECK_EQ(SceneCapture::compile(make_capture_input(), first, &error), OK);
	CHECK_EQ(SceneCapture::compile(make_capture_input(), second, &error), OK);
	CHECK_EQ(first, second);
	CHECK_EQ(SceneCapture::validate(first, &error), OK);
	CHECK_EQ(SceneCapture::get_scene_packet(first, packet, &error), OK);
	CHECK_EQ(packet.size(), 1024);

	SceneCaptureHeader header = {};
	CHECK_EQ(SceneCompiler::read_record(first, 0, header), OK);
	CHECK_EQ(header.geometry_count, 1);
	CHECK_EQ(header.vertex_count, 3);
	CHECK_EQ(header.index_count, 3);

	const String capture_path = TestUtils::get_temp_path("path_tracing_scene_capture.ptc");
	CHECK_EQ(SceneCapture::save(capture_path, first, &error), OK);
	PackedByteArray replay;
	CHECK_EQ(SceneCapture::load(capture_path, replay, &error), OK);
	CHECK_EQ(replay, first);
}

TEST_CASE("[PathTracing] Scene captures reject missing geometry and damaged mesh data") {
	SceneCaptureInput missing_geometry = make_capture_input();
	missing_geometry.geometries.clear();
	PackedByteArray capture;
	String error;
	CHECK_EQ(SceneCapture::compile(missing_geometry, capture, &error), ERR_INVALID_PARAMETER);

	CHECK_EQ(SceneCapture::compile(make_capture_input(), capture, &error), OK);
	SceneCaptureHeader header = {};
	CHECK_EQ(SceneCompiler::read_record(capture, 0, header), OK);
	uint32_t invalid_index = 3;
	memcpy(capture.ptrw() + header.index_offset, &invalid_index, sizeof(invalid_index));
	header.payload_hash = SceneCompiler::hash_payload(capture.ptr() + sizeof(SceneCaptureHeader), capture.size() - sizeof(SceneCaptureHeader));
	memcpy(capture.ptrw(), &header, sizeof(header));
	CHECK_EQ(SceneCapture::validate(capture, &error), ERR_FILE_CORRUPT);
}

TEST_CASE("[PathTracing] Backend requests enforce capability and stereo target contracts") {
	FrameRequest request;
	String error;
	CHECK_EQ(SceneCapture::compile(make_capture_input(), request.capture, &error), OK);
	request.output_color.push_back(RID::from_uint64(1));
	request.output_color.push_back(RID::from_uint64(2));

	TestBackend stereo_backend(true, 2);
	FrameResult result;
	CHECK_EQ(stereo_backend.render(request, result, &error), OK);

	TestBackend mono_backend(true, 1);
	CHECK_EQ(mono_backend.render(request, result, &error), ERR_UNAVAILABLE);

	TestBackend unavailable_backend(false, 2);
	CHECK_EQ(unavailable_backend.render(request, result, &error), ERR_UNAVAILABLE);

	request.output_color.resize(1);
	CHECK_EQ(stereo_backend.render(request, result, &error), ERR_INVALID_PARAMETER);
}

TEST_CASE("[PathTracing] Godot mesh resources compile into self-contained captures") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.5f, 0.25f, 0.125f));
	material->set_metallic(0.75f);
	material->set_roughness(0.3f);
	material->set_emission(Color(0.1f, 0.2f, 0.3f));
	material->set_emission_energy_multiplier(2.0f);

	SceneCaptureInput input;
	input.scene = make_input();
	input.scene.instances.clear();
	input.scene.materials.clear();
	ResourceCompileResult result;
	String error;
	CHECK_EQ(ResourceCompiler::append_mesh(make_array_mesh(material), 42, 7, Transform3D(Basis(), Vector3(1, 2, 3)),
					 Transform3D(Basis(), Vector3(0, 2, 3)), input, result, &error),
			OK);
	CHECK_EQ(result.geometry_count, 1);
	CHECK_EQ(result.material_count, 1);
	CHECK_EQ(result.instance_count, 1);
	CHECK_FALSE(result.has_material_fallback);
	CHECK_EQ(input.geometries[0].geometry_id, 42);
	CHECK_EQ(input.scene.instances[0].material_id, 1);
	CHECK_EQ(input.scene.instances[0].world_from_object.columns[3].x, 1.0f);
	CHECK_EQ(input.scene.instances[0].previous_world_from_object.columns[3].x, 0.0f);

	PackedByteArray capture;
	CHECK_EQ(SceneCapture::compile(input, capture, &error), OK);
}

TEST_CASE("[PathTracing] Unsupported Godot materials use a visible diagnosed fallback") {
	SceneCaptureInput input;
	input.scene = make_input();
	input.scene.instances.clear();
	input.scene.materials.clear();
	ResourceCompileResult result;
	String error;
	CHECK_EQ(ResourceCompiler::append_mesh(make_array_mesh(Ref<Material>()), 1, 1, Transform3D(), Transform3D(), input, result, &error), OK);
	CHECK(result.has_material_fallback);
	CHECK_EQ(result.diagnostics.size(), 1);
	CHECK(result.diagnostics[0].material_fallback);
	CHECK_EQ(input.scene.materials[0].base_color_and_opacity.x, 1.0f);
	CHECK_EQ(input.scene.materials[0].base_color_and_opacity.y, 0.0f);
	CHECK_EQ(input.scene.materials[0].base_color_and_opacity.z, 1.0f);
}

TEST_CASE("[PathTracing] Deformed mesh compilation preserves current and previous positions") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	Ref<ArrayMesh> previous = make_array_mesh(material);
	Array arrays = previous->surface_get_arrays(0);
	PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
	positions.set(0, positions[0] + Vector3(2, 0, 0));
	arrays[Mesh::ARRAY_VERTEX] = positions;
	Ref<ArrayMesh> current;
	current.instantiate();
	current->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	current->surface_set_material(0, material);
	SceneCaptureInput input;
	input.scene = make_input();
	input.scene.instances.clear();
	input.scene.materials.clear();
	ResourceCompileResult result;
	String error;
	CHECK_EQ(ResourceCompiler::append_deformed_mesh(current, previous, 10, 20, Transform3D(), Transform3D(), input, result, &error), OK);
	CHECK((input.geometries[0].flags & GEOMETRY_DYNAMIC) != 0);
	CHECK_EQ(input.geometries[0].vertices[0].previous_position.x, -1.0f);
	CHECK_EQ(input.geometries[0].vertices[0].current_position.x, 1.0f);
}

TEST_CASE("[PathTracing] Godot analytic lights and color environment compile without backend types") {
	SceneCaptureInput input;
	OmniLight3D *omni = memnew(OmniLight3D);
	omni->set_position(Vector3(1, 2, 3));
	omni->set_color(Color(0.5f, 0.25f, 0.125f));
	omni->set_param(Light3D::PARAM_ENERGY, 4.0f);
	String error;
	CHECK_EQ(ResourceCompiler::append_light(omni, 3, input, &error), OK);
	CHECK_EQ(input.scene.lights[0].position_or_direction_and_type.w, float(LIGHT_POINT));
	CHECK_EQ(input.scene.lights[0].position_or_direction_and_type.x, 1.0f);
	CHECK_EQ(input.scene.lights[0].linear_color_and_intensity.w, 4.0f);
	memdelete(omni);

	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_bg_color(Color(0.2f, 0.3f, 0.4f));
	environment->set_bg_energy_multiplier(2.0f);
	CHECK_EQ(ResourceCompiler::append_environment(environment, 4, input, &error), OK);
	CHECK_EQ(input.scene.lights[1].position_or_direction_and_type.w, float(LIGHT_ENVIRONMENT));
	CHECK_EQ(input.scene.lights[1].linear_color_and_intensity.w, 2.0f);
}

TEST_CASE("[PathTracing] Unsupported spot lights fail visibly instead of changing semantics") {
	SceneCaptureInput capture;
	SpotLight3D spot;
	String error;
	CHECK_EQ(ResourceCompiler::append_light(&spot, 1, capture, &error), ERR_UNAVAILABLE);
	CHECK(error.contains("SpotLight3D"));
	CHECK(capture.scene.lights.is_empty());
}

TEST_CASE("[PathTracing] Remote Windows launch plans are deterministic and credential-free") {
	RemoteWindowsPreset preset;
	preset.host = "render-pc.local";
	preset.user = "godot-runner";
	preset.port = 2222;
	preset.remote_directory = "C:/GodotRemote/PathTracing";
	preset.export_preset = "Windows VR Validation";
	preset.artifact_path = "/tmp/path tracing validation.exe";
	preset.engine_commit = "0123456789abcdef0123456789abcdef01234567";
	preset.asset_manifest_sha256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	preset.export_template_available = true;

	RemoteWindowsCommandPreview first;
	RemoteWindowsCommandPreview second;
	Vector<String> diagnostics;
	CHECK_EQ(RemoteWindowsLaunchPlan::generate(preset, first, diagnostics), OK);
	CHECK(diagnostics.is_empty());
	CHECK_EQ(RemoteWindowsLaunchPlan::generate(preset, second, diagnostics), OK);
	CHECK_EQ(first.manifest_json, second.manifest_json);
	CHECK_EQ(first.transfer_argv, second.transfer_argv);
	CHECK_EQ(first.launch_argv, second.launch_argv);
	CHECK(first.manifest_json.contains(preset.engine_commit));
	CHECK(first.manifest_json.contains(preset.asset_manifest_sha256));
	CHECK_FALSE(first.manifest_json.contains(preset.user));
	CHECK_FALSE(first.manifest_json.contains(preset.host));
	CHECK_EQ(first.transfer_argv[0], "scp");
	CHECK_EQ(first.launch_argv[0], "ssh");
	CHECK_EQ(first.launch_argv[4], "powershell.exe");
}

TEST_CASE("[PathTracing] Remote Windows presets reject ambiguous or unsafe inputs") {
	RemoteWindowsPreset preset;
	preset.host = "runner@example.com";
	preset.user = "user;shutdown";
	preset.remote_directory = "../unsafe\npath";
	preset.engine_commit = "short";
	preset.asset_manifest_sha256 = "not-a-digest";
	preset.renderer_backend = "d3d12";
	Vector<String> diagnostics;
	CHECK_EQ(RemoteWindowsLaunchPlan::validate(preset, diagnostics), ERR_INVALID_PARAMETER);
	CHECK_GE(diagnostics.size(), 8);
}

TEST_CASE("[PathTracing] Mock remote Windows endpoint enforces the deployment lifecycle") {
	RemoteWindowsMockEndpoint endpoint;
	CHECK_EQ(endpoint.apply(RemoteWindowsMockEndpoint::EVENT_TRANSFER_OK), ERR_INVALID_PARAMETER);
	CHECK_EQ(endpoint.get_state(), RemoteWindowsMockEndpoint::STATE_IDLE);
	const RemoteWindowsMockEndpoint::Event events[] = {
		RemoteWindowsMockEndpoint::EVENT_VALIDATE_OK,
		RemoteWindowsMockEndpoint::EVENT_TRANSFER_OK,
		RemoteWindowsMockEndpoint::EVENT_ENDPOINT_STARTED,
		RemoteWindowsMockEndpoint::EVENT_XR_SYSTEM_READY,
		RemoteWindowsMockEndpoint::EVENT_ENGINE_STARTED,
		RemoteWindowsMockEndpoint::EVENT_CHANNELS_FORWARDED,
		RemoteWindowsMockEndpoint::EVENT_HEALTHY,
	};
	for (const RemoteWindowsMockEndpoint::Event event : events) {
		CHECK_EQ(endpoint.apply(event), OK);
	}
	CHECK_EQ(endpoint.get_state(), RemoteWindowsMockEndpoint::STATE_READY);
	CHECK_EQ(endpoint.get_transitions().size(), 7);
	CHECK_EQ(endpoint.apply(RemoteWindowsMockEndpoint::EVENT_HEALTHY), ERR_INVALID_PARAMETER);

	RemoteWindowsMockEndpoint failed;
	CHECK_EQ(failed.apply(RemoteWindowsMockEndpoint::EVENT_FAILURE), OK);
	CHECK_EQ(failed.get_state(), RemoteWindowsMockEndpoint::STATE_FAILED);
	CHECK_EQ(failed.apply(RemoteWindowsMockEndpoint::EVENT_VALIDATE_OK), ERR_INVALID_PARAMETER);
}

TEST_CASE("[PathTracing][SceneTree] Godot eight-weight skin and blend-shape deformation publishes previous vertices") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	Node3D *root = memnew(Node3D);
	Skeleton3D *skeleton = memnew(Skeleton3D);
	skeleton->set_name("Skeleton");
	skeleton->add_bone("Root");
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_name("Mesh");
	instance->set_mesh(make_skinned_blend_shape_mesh(material));
	Ref<Skin> skin;
	skin.instantiate();
	skin->set_bind_count(1);
	skin->set_bind_bone(0, 0);
	skin->set_bind_pose(0, Transform3D());
	instance->set_skin(skin);
	instance->set_skeleton_path(NodePath("../Skeleton"));
	root->add_child(skeleton);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	if (instance->get_skin_reference().is_null() || !instance->get_skin_reference()->get_skeleton().is_valid()) {
		MESSAGE("The unit-test RenderingServer has no skeleton storage; live deformation coverage runs in the M1 validation project.");
		CHECK(true);
		root->get_parent()->remove_child(root);
		memdelete(root);
		return;
	}
	skeleton->force_update_all_bone_transforms();
	Ref<ArrayMesh> previous_deformation = instance->bake_mesh_from_current_deformation();
	REQUIRE(previous_deformation.is_valid());

	instance->set_blend_shape_value(0, 1.0f);
	skeleton->set_bone_pose_position(0, Vector3(2, 0, 0));
	skeleton->force_update_all_bone_transforms();
	SceneCaptureInput capture_input;
	capture_input.scene = make_input();
	capture_input.scene.instances.clear();
	capture_input.scene.materials.clear();
	Ref<ArrayMesh> current_deformation;
	ResourceCompileResult result;
	String error;
	CHECK_EQ(ResourceCompiler::append_mesh_instance(instance, previous_deformation, 50, 80, Transform3D(), capture_input, current_deformation, result, &error), OK);
	REQUIRE(current_deformation.is_valid());
	CHECK_EQ(capture_input.geometries.size(), 1);
	CHECK((capture_input.geometries[0].flags & GEOMETRY_DYNAMIC) != 0);
	CHECK_EQ(capture_input.geometries[0].vertices[0].previous_position.x, -1.0f);
	CHECK_EQ(capture_input.geometries[0].vertices[0].current_position.x, 2.0f);
	CHECK_FALSE(result.has_material_fallback);
	root->get_parent()->remove_child(root);
	memdelete(root);
}

TEST_CASE("[PathTracing][SceneTree] Live scene state compiles deterministic current and previous frames") {
	Node3D *root = memnew(Node3D);
	root->set_name("PathTracingScene");
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_name("Triangle");
	Ref<StandardMaterial3D> material;
	material.instantiate();
	instance->set_mesh(make_array_mesh(material));
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	CameraViewInput view;
	view.world_from_view = Transform3D(Basis(), Vector3(0, 0, 2));
	view.clip_from_view = Projection::create_perspective(70.0f, 1.0f, 0.1f, 100.0f);
	view.width = 64;
	view.height = 64;
	Vector<CameraViewInput> views;
	views.push_back(view);
	PathTracingSceneState state;
	PackedByteArray capture;
	SceneStateResult result;
	String error;
	CHECK_EQ(state.compile(root, views, capture, result, &error), OK);
	CHECK_EQ(result.resources.instance_count, 1);
	CHECK_FALSE(result.resources.has_material_fallback);
	instance->set_position(Vector3(1, 0, 0));
	CHECK_EQ(state.compile(root, views, capture, result, &error), OK);
	PackedByteArray packet;
	CHECK_EQ(SceneCapture::get_scene_packet(capture, packet, &error), OK);
	ScenePacketHeader header = {};
	CHECK_EQ(SceneCompiler::read_record(packet, 0, header), OK);
	InstanceRecord compiled_instance = {};
	CHECK_EQ(SceneCompiler::read_record(packet, header.instance_offset, compiled_instance), OK);
	CHECK_EQ(compiled_instance.world_from_object.columns[3].x, 1.0f);
	CHECK_EQ(compiled_instance.previous_world_from_object.columns[3].x, 0.0f);
	root->get_parent()->remove_child(root);
	memdelete(root);
}

TEST_CASE("[PathTracing] Guide validator reports sentinels and semantic range failures") {
	GuidePlane normal;
	normal.guide = GUIDE_NORMAL;
	normal.width = 2;
	normal.height = 1;
	normal.components = 3;
	normal.values.push_back(0.0f);
	normal.values.push_back(0.0f);
	normal.values.push_back(1.0f);
	normal.values.push_back(Math::NaN);
	normal.values.push_back(Math::NaN);
	normal.values.push_back(Math::NaN);
	GuideValidationResult result;
	String error;
	CHECK_EQ(GuideValidator::validate_and_visualize(normal, result, &error), OK);
	CHECK(result.is_valid());
	CHECK_EQ(result.valid_pixels, 1);
	CHECK_EQ(result.invalid_sentinel_pixels, 1);
	CHECK_EQ(result.debug_rgba8.size(), 8);

	GuidePlane depth;
	depth.guide = GUIDE_DEPTH;
	depth.width = 1;
	depth.height = 1;
	depth.components = 1;
	depth.values.push_back(1.25f);
	CHECK_EQ(GuideValidator::validate_and_visualize(depth, result, &error), OK);
	CHECK_FALSE(result.is_valid());
	CHECK_EQ(result.out_of_range_pixels, 1);
}

#ifdef METAL_ENABLED
TEST_CASE("[PathTracing][Metal] Native capture backend writes a Godot texture") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RenderingContextDriverMetal *context = nullptr;
	if (!rd) {
		context = memnew(RenderingContextDriverMetal);
		rd = memnew(RenderingDevice);
		Error initialize_error = context->initialize();
		if (initialize_error == OK) {
			initialize_error = rd->initialize(context);
		}
		if (initialize_error != OK) {
			memdelete(rd);
			memdelete(context);
			CHECK_EQ(initialize_error, OK);
			return;
		}
	}
	const bool owns_device = context != nullptr;
	if (rd->get_device_api_name() != "Metal") {
		if (owns_device) {
			rd->finalize();
			memdelete(rd);
			memdelete(context);
		}
		return;
	}

	PackedByteArray capture;
	String error;
	CHECK_EQ(SceneCapture::compile(make_render_capture_input(), capture, &error), OK);
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.width = 64;
	format.height = 64;
	format.texture_type = RD::TEXTURE_TYPE_2D;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID left = rd->texture_create(format, RD::TextureView());
	RID right = rd->texture_create(format, RD::TextureView());
	REQUIRE(left.is_valid());
	REQUIRE(right.is_valid());
	Vector<RID> allocated_targets;
	allocated_targets.push_back(left);
	allocated_targets.push_back(right);
	auto create_stereo_targets = [&](RD::DataFormat p_format, Vector<RID> &r_targets) {
		RD::TextureFormat guide_format = format;
		guide_format.format = p_format;
		for (uint32_t view = 0; view < 2; view++) {
			RID target = rd->texture_create(guide_format, RD::TextureView());
			REQUIRE(target.is_valid());
			r_targets.push_back(target);
			allocated_targets.push_back(target);
		}
	};

	FrameRequest request;
	request.capture = capture;
	request.mode = RenderMode::PROGRESSIVE_REFERENCE;
	request.output_color.push_back(left);
	request.output_color.push_back(right);
	create_stereo_targets(RD::DATA_FORMAT_R32_SFLOAT, request.output_depth);
	create_stereo_targets(RD::DATA_FORMAT_R16G16_SFLOAT, request.output_motion);
	create_stereo_targets(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, request.output_normal);
	create_stereo_targets(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, request.output_diffuse_albedo);
	create_stereo_targets(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, request.output_specular_albedo);
	create_stereo_targets(RD::DATA_FORMAT_R16_SFLOAT, request.output_roughness);
	create_stereo_targets(RD::DATA_FORMAT_R8_UNORM, request.output_denoise_strength);
	create_stereo_targets(RD::DATA_FORMAT_R8_UNORM, request.output_reactive_mask);
	create_stereo_targets(RD::DATA_FORMAT_R16_SFLOAT, request.output_specular_hit_distance);
	create_stereo_targets(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, request.output_transparency_overlay);
	RendererRD::MetalPathTracingBackend backend;
	FrameResult result;
	const Error render_error = backend.render(request, result, &error);
	INFO(error);
	CHECK_EQ(render_error, OK);
	if (render_error != OK) {
		for (const RID &target : allocated_targets) {
			rd->free_rid(target);
		}
		if (owns_device) {
			memdelete(rd);
			memdelete(context);
		}
		return;
	}
	CHECK_EQ(result.rendered_views, 2);
	CHECK_EQ(result.emitted_guides, uint32_t(GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO | GUIDE_SPECULAR_ALBEDO | GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH | GUIDE_REACTIVE_MASK | GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY));
	RendererRD::MFXDenoisedEffect denoised_effect;
	REQUIRE(denoised_effect.is_supported());
	RendererRD::MFXDenoisedEffect::CreateParams denoised_create;
	denoised_create.input_size = Vector2i(64, 64);
	denoised_create.output_size = Vector2i(128, 128);
	denoised_create.color_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	denoised_create.depth_format = RD::DATA_FORMAT_R32_SFLOAT;
	denoised_create.motion_format = RD::DATA_FORMAT_R16G16_SFLOAT;
	denoised_create.normal_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	denoised_create.diffuse_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	denoised_create.specular_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	denoised_create.roughness_format = RD::DATA_FORMAT_R16_SFLOAT;
	denoised_create.denoise_strength_format = RD::DATA_FORMAT_R8_UNORM;
	denoised_create.reactive_format = RD::DATA_FORMAT_R8_UNORM;
	denoised_create.specular_distance_format = RD::DATA_FORMAT_R16_SFLOAT;
	denoised_create.transparency_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	denoised_create.output_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	RendererRD::MFXDenoisedContext *denoised_context = denoised_effect.create_context(denoised_create, &error);
	REQUIRE_MESSAGE(denoised_context != nullptr, error);
	RendererRD::MFXDenoisedContext *denoised_context_right = denoised_effect.create_context(denoised_create, &error);
	REQUIRE_MESSAGE(denoised_context_right != nullptr, error);
	RD::TextureFormat denoised_format = format;
	denoised_format.width = 128;
	denoised_format.height = 128;
	RID denoised_output = rd->texture_create(denoised_format, RD::TextureView());
	RID denoised_output_right = rd->texture_create(denoised_format, RD::TextureView());
	REQUIRE(denoised_output.is_valid());
	REQUIRE(denoised_output_right.is_valid());
	allocated_targets.push_back(denoised_output);
	allocated_targets.push_back(denoised_output_right);
	PackedByteArray packet;
	CHECK_EQ(SceneCapture::get_scene_packet(capture, packet, &error), OK);
	ScenePacketHeader packet_header = {};
	CHECK_EQ(SceneCompiler::read_record(packet, 0, packet_header), OK);
	CameraRecord left_camera = {};
	CHECK_EQ(SceneCompiler::read_record(packet, packet_header.camera_offset, left_camera), OK);
	RendererRD::MFXDenoisedEffect::Params denoised_params;
	denoised_params.color = left;
	denoised_params.depth = request.output_depth[0];
	denoised_params.motion = request.output_motion[0];
	denoised_params.normal = request.output_normal[0];
	denoised_params.diffuse = request.output_diffuse_albedo[0];
	denoised_params.specular = request.output_specular_albedo[0];
	denoised_params.roughness = request.output_roughness[0];
	denoised_params.denoise_strength = request.output_denoise_strength[0];
	denoised_params.reactive = request.output_reactive_mask[0];
	denoised_params.specular_distance = request.output_specular_hit_distance[0];
	denoised_params.transparency = request.output_transparency_overlay[0];
	denoised_params.output = denoised_output;
	denoised_params.view_from_world = left_camera.view_from_world;
	denoised_params.clip_from_view = left_camera.clip_from_view;
	denoised_params.motion_vector_scale = Vector2(64, 64);
	denoised_params.reset = true;
	CHECK_EQ(denoised_effect.process(denoised_context, denoised_params, &error), OK);
	CameraRecord right_camera = {};
	CHECK_EQ(SceneCompiler::read_record(packet, packet_header.camera_offset + sizeof(CameraRecord), right_camera), OK);
	RendererRD::MFXDenoisedEffect::Params denoised_right_params = denoised_params;
	denoised_right_params.color = right;
	denoised_right_params.depth = request.output_depth[1];
	denoised_right_params.motion = request.output_motion[1];
	denoised_right_params.normal = request.output_normal[1];
	denoised_right_params.diffuse = request.output_diffuse_albedo[1];
	denoised_right_params.specular = request.output_specular_albedo[1];
	denoised_right_params.roughness = request.output_roughness[1];
	denoised_right_params.denoise_strength = request.output_denoise_strength[1];
	denoised_right_params.reactive = request.output_reactive_mask[1];
	denoised_right_params.specular_distance = request.output_specular_hit_distance[1];
	denoised_right_params.transparency = request.output_transparency_overlay[1];
	denoised_right_params.output = denoised_output_right;
	denoised_right_params.view_from_world = right_camera.view_from_world;
	denoised_right_params.clip_from_view = right_camera.clip_from_view;
	CHECK_EQ(denoised_effect.process(denoised_context_right, denoised_right_params, &error), OK);
	rd->submit();
	rd->sync();
	Vector<StageTiming> timings;
	CHECK_EQ(backend.collect_completed_timings(timings, &error), ERR_UNAVAILABLE);
	CHECK(timings.is_empty());
	CHECK(error.contains("timestamp queries"));
	const PackedByteArray left_data = rd->texture_get_data(left, 0);
	const PackedByteArray right_data = rd->texture_get_data(right, 0);
	CHECK_EQ(left_data.size(), 64 * 64 * 8);
	CHECK_EQ(right_data.size(), left_data.size());
	bool any_radiance = false;
	for (uint8_t byte : left_data) {
		any_radiance |= byte != 0;
	}
	CHECK(any_radiance);
	CHECK_NE(left_data, right_data);
	const PackedByteArray denoised_data = rd->texture_get_data(denoised_output, 0);
	const PackedByteArray denoised_right_data = rd->texture_get_data(denoised_output_right, 0);
	CHECK_EQ(denoised_data.size(), 128 * 128 * 8);
	CHECK_EQ(denoised_right_data.size(), denoised_data.size());
	bool any_denoised_radiance = false;
	for (uint8_t byte : denoised_data) {
		any_denoised_radiance |= byte != 0;
	}
	CHECK(any_denoised_radiance);
	CHECK_NE(denoised_data, denoised_right_data);
	const PackedByteArray depth_data = rd->texture_get_data(request.output_depth[0], 0);
	const PackedByteArray normal_data = rd->texture_get_data(request.output_normal[0], 0);
	const PackedByteArray denoise_data = rd->texture_get_data(request.output_denoise_strength[0], 0);
	CHECK_EQ(depth_data.size(), 64 * 64 * 4);
	CHECK_EQ(normal_data.size(), 64 * 64 * 8);
	CHECK_EQ(denoise_data.size(), 64 * 64);
	bool any_normal = false;
	bool any_denoise_strength = false;
	for (uint8_t byte : normal_data) {
		any_normal |= byte != 0;
	}
	for (uint8_t byte : denoise_data) {
		any_denoise_strength |= byte != 0;
	}
	CHECK(any_normal);
	CHECK_FALSE(any_denoise_strength);
	SceneCaptureInput deformed_input = make_render_capture_input();
	for (GeometryVertex &vertex : deformed_input.geometries.write[0].vertices) {
		vertex.previous_position = vertex.current_position;
		vertex.current_position.x += 4.0f;
	}
	CHECK_EQ(SceneCapture::compile(deformed_input, request.capture, &error), OK);
	request.sample_index = 0;
	FrameResult deformed_result;
	CHECK_EQ(backend.render(request, deformed_result, &error), OK);
	CHECK_EQ(deformed_result.blas_rebuilt, 0);
	CHECK_EQ(deformed_result.blas_refit, 1);
	CHECK_EQ(deformed_result.blas_reused, 0);
	rd->submit();
	rd->sync();
	const PackedByteArray deformed_left_data = rd->texture_get_data(left, 0);
	CHECK_NE(deformed_left_data, left_data);
	memdelete(denoised_context);
	memdelete(denoised_context_right);
	for (const RID &target : allocated_targets) {
		rd->free_rid(target);
	}
	if (owns_device) {
		memdelete(rd);
		memdelete(context);
	}
}
#endif

} // namespace TestPathTracingSceneCompiler
