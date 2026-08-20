/**************************************************************************/
/*  path_tracing_editor_plugin.cpp                                        */
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

#include "path_tracing_editor_plugin.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererPathTracing;

static uint64_t _hash_capture_current_positions(const PackedByteArray &p_capture) {
	SceneCaptureHeader header = {};
	if (SceneCompiler::read_record(p_capture, 0, header) != OK) {
		return 0;
	}
	uint64_t hash = 1469598103934665603ULL;
	for (uint32_t vertex = 0; vertex < header.vertex_count; vertex++) {
		GeometryVertex geometry_vertex = {};
		if (SceneCompiler::read_record(p_capture, header.vertex_offset + vertex * sizeof(GeometryVertex), geometry_vertex) != OK) {
			return 0;
		}
		const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&geometry_vertex.current_position);
		for (uint32_t byte = 0; byte < sizeof(Float4); byte++) {
			hash ^= bytes[byte];
			hash *= 1099511628211ULL;
		}
	}
	return hash;
}

Camera3D *PathTracingEditorPlugin::_find_camera(Node *p_node) {
	if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		return camera;
	}
	for (int child = 0; child < p_node->get_child_count(); child++) {
		if (Camera3D *camera = _find_camera(p_node->get_child(child))) {
			return camera;
		}
	}
	return nullptr;
}

void PathTracingEditorPlugin::_free_targets() {
#ifdef METAL_ENABLED
	if (denoised_context) {
		memdelete(denoised_context);
		denoised_context = nullptr;
	}
#endif
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		return;
	}
	if (preview_texture.is_valid()) {
		preview_texture->set_texture_rd_rid(RID());
	}
	if (color_target.is_valid()) {
		rd->free_rid(color_target);
		color_target = RID();
	}
	if (reconstructed_target.is_valid()) {
		rd->free_rid(reconstructed_target);
		reconstructed_target = RID();
	}
	for (Vector<RID> &targets : guide_targets) {
		for (const RID &target : targets) {
			if (target.is_valid()) {
				rd->free_rid(target);
			}
		}
		targets.clear();
	}
	target_size = Size2i();
	reconstructed_size = Size2i();
}

Error PathTracingEditorPlugin::_ensure_targets(const Size2i &p_input_size, const Size2i &p_output_size, bool p_use_metalfx, String *r_error) {
	if (color_target.is_valid() && target_size == p_input_size && reconstructed_size == (p_use_metalfx ? p_output_size : Size2i())) {
		return OK;
	}
	_free_targets();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		if (r_error) {
			*r_error = "RenderingDevice is unavailable.";
		}
		return ERR_UNAVAILABLE;
	}
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.width = p_input_size.x;
	format.height = p_input_size.y;
	format.texture_type = RD::TEXTURE_TYPE_2D;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	color_target = rd->texture_create(format, RD::TextureView());
	const RD::DataFormat guide_formats[10] = {
		RD::DATA_FORMAT_R32_SFLOAT,
		RD::DATA_FORMAT_R16G16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
		RD::DATA_FORMAT_R16_SFLOAT,
		RD::DATA_FORMAT_R8_UNORM,
		RD::DATA_FORMAT_R8_UNORM,
		RD::DATA_FORMAT_R16_SFLOAT,
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
	};
	for (uint32_t guide_index = 0; guide_index < 10; guide_index++) {
		format.format = guide_formats[guide_index];
		RID target = rd->texture_create(format, RD::TextureView());
		if (!target.is_valid()) {
			_free_targets();
			if (r_error) {
				*r_error = "A path-tracing guide target could not be allocated.";
			}
			return ERR_CANT_CREATE;
		}
		guide_targets[guide_index].push_back(target);
	}
	if (!color_target.is_valid()) {
		_free_targets();
		return ERR_CANT_CREATE;
	}
	target_size = p_input_size;
#ifdef METAL_ENABLED
	if (p_use_metalfx) {
		if (!denoised_effect.is_supported()) {
			_free_targets();
			if (r_error) {
				*r_error = "Temporal denoised MetalFX scaling is unavailable on this device.";
			}
			return ERR_UNAVAILABLE;
		}
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.width = p_output_size.x;
		format.height = p_output_size.y;
		reconstructed_target = rd->texture_create(format, RD::TextureView());
		RendererRD::MFXDenoisedEffect::CreateParams create;
		create.input_size = p_input_size;
		create.output_size = p_output_size;
		create.color_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		create.depth_format = RD::DATA_FORMAT_R32_SFLOAT;
		create.motion_format = RD::DATA_FORMAT_R16G16_SFLOAT;
		create.normal_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		create.diffuse_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		create.specular_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		create.roughness_format = RD::DATA_FORMAT_R16_SFLOAT;
		create.denoise_strength_format = RD::DATA_FORMAT_R8_UNORM;
		create.reactive_format = RD::DATA_FORMAT_R8_UNORM;
		create.specular_distance_format = RD::DATA_FORMAT_R16_SFLOAT;
		create.transparency_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		create.output_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		denoised_context = denoised_effect.create_context(create, r_error);
		if (!reconstructed_target.is_valid() || !denoised_context) {
			_free_targets();
			return ERR_CANT_CREATE;
		}
		reconstructed_size = p_output_size;
	}
#endif
	return OK;
}

void PathTracingEditorPlugin::_update_preview_texture() {
	const int selected = guide->get_selected();
	RID selected_target = reconstructed_target.is_valid() ? reconstructed_target : color_target;
	if (selected > 0 && selected <= 10 && !guide_targets[selected - 1].is_empty()) {
		selected_target = guide_targets[selected - 1][0];
	}
	preview_texture->set_texture_rd_rid(selected_target);
}

void PathTracingEditorPlugin::_render_scene() {
	last_render_ok = false;
	if (mode->get_selected() == 0) {
		diagnostics->set_text("Raster mode uses the normal 3D editor viewport.");
		return;
	}
#ifndef METAL_ENABLED
	diagnostics->set_text("Native path tracing is unavailable in this non-Metal editor build.");
	return;
#else
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	if (!root) {
		diagnostics->set_text("Open a 3D scene before rendering.");
		return;
	}
	Camera3D *camera = _find_camera(root);
	if (!camera) {
		diagnostics->set_text("The path-tracing panel requires a Camera3D in the edited scene.");
		return;
	}
	const Size2i sizes[] = { Size2i(320, 180), Size2i(640, 360), Size2i(960, 540) };
	const Size2i output_size = sizes[resolution->get_selected()];
	const bool use_metalfx = metalfx->is_pressed();
	const Size2i size = use_metalfx ? Size2i(MAX(1, output_size.x / 2), MAX(1, output_size.y / 2)) : output_size;
	String error;
	if (_ensure_targets(size, output_size, use_metalfx, &error) != OK) {
		diagnostics->set_text(error);
		return;
	}
	CameraViewInput view;
	view.world_from_view = camera->get_camera_transform();
	view.clip_from_view = camera->get_camera_projection();
	view.width = size.x;
	view.height = size.y;
	view.history_reset = sample_index == 0;
	Vector<CameraViewInput> views;
	views.push_back(view);
	PackedByteArray capture;
	SceneStateResult scene_result;
	if (scene_state.compile(root, views, capture, scene_result, &error) != OK) {
		diagnostics->set_text("Scene compile failed: " + error);
		return;
	}
	last_geometry_hash = _hash_capture_current_positions(capture);
	FrameRequest request;
	request.capture = capture;
	request.mode = mode->get_selected() == 2 ? RenderMode::PROGRESSIVE_REFERENCE : RenderMode::INTERACTIVE;
	request.output_color.push_back(color_target);
	request.output_depth = guide_targets[0];
	request.output_motion = guide_targets[1];
	request.output_normal = guide_targets[2];
	request.output_diffuse_albedo = guide_targets[3];
	request.output_specular_albedo = guide_targets[4];
	request.output_roughness = guide_targets[5];
	request.output_denoise_strength = guide_targets[6];
	request.output_reactive_mask = guide_targets[7];
	request.output_specular_hit_distance = guide_targets[8];
	request.output_transparency_overlay = guide_targets[9];
	request.sample_index = request.mode == RenderMode::PROGRESSIVE_REFERENCE ? sample_index : 0;
	request.max_bounces = bounces->get_value();
	FrameResult frame_result;
	if (backend.render(request, frame_result, &error) != OK) {
		diagnostics->set_text("Metal render failed: " + error);
		return;
	}
	if (use_metalfx) {
		PackedByteArray scene_packet;
		SceneCapture::get_scene_packet(capture, scene_packet, &error);
		ScenePacketHeader scene_header = {};
		SceneCompiler::read_record(scene_packet, 0, scene_header);
		CameraRecord camera_record = {};
		SceneCompiler::read_record(scene_packet, scene_header.camera_offset, camera_record);
		RendererRD::MFXDenoisedEffect::Params parameters;
		parameters.color = color_target;
		parameters.depth = guide_targets[0][0];
		parameters.motion = guide_targets[1][0];
		parameters.normal = guide_targets[2][0];
		parameters.diffuse = guide_targets[3][0];
		parameters.specular = guide_targets[4][0];
		parameters.roughness = guide_targets[5][0];
		parameters.denoise_strength = guide_targets[6][0];
		parameters.reactive = guide_targets[7][0];
		parameters.specular_distance = guide_targets[8][0];
		parameters.transparency = guide_targets[9][0];
		parameters.output = reconstructed_target;
		parameters.view_from_world = camera_record.view_from_world;
		parameters.clip_from_view = camera_record.clip_from_view;
		parameters.motion_vector_scale = Vector2(size.x, size.y);
		parameters.pre_exposure = 1.0f;
		parameters.reset = camera_record.history_reset != 0;
		if (denoised_effect.process(denoised_context, parameters, &error) != OK) {
			diagnostics->set_text("MetalFX reconstruction failed: " + error);
			return;
		}
	}
	last_render_ok = true;
	last_blas_refit = frame_result.blas_refit;
	last_material_fallback = scene_result.resources.has_material_fallback;
	if (request.mode == RenderMode::PROGRESSIVE_REFERENCE && !freeze_accumulation->is_pressed()) {
		sample_index++;
	}
	String status = vformat("Metal reference | %dx%d%s | sample %d | BLAS build %d, refit %d, reuse %d",
			size.x, size.y, use_metalfx ? vformat(" -> %dx%d MetalFX", output_size.x, output_size.y) : String(), sample_index,
			frame_result.blas_rebuilt, frame_result.blas_refit, frame_result.blas_reused);
	if (scene_result.resources.has_material_fallback) {
		status += "\nWARNING: unsupported materials are using the visible magenta fallback.";
	}
	Vector<StageTiming> timings;
	if (backend.collect_completed_timings(timings) == OK) {
		for (const StageTiming &timing : timings) {
			status += vformat(" | %s %.3f ms", String(timing.stage), timing.milliseconds);
		}
	}
	diagnostics->set_text(status);
	_update_preview_texture();
#endif
}

void PathTracingEditorPlugin::_reset_history() {
	sample_index = 0;
	scene_state.reset_history();
	diagnostics->set_text("Path-tracing accumulation and motion history reset.");
}

void PathTracingEditorPlugin::_mode_changed(int p_mode) {
	_reset_history();
	_render_scene();
}

void PathTracingEditorPlugin::_guide_changed(int p_guide) {
	_update_preview_texture();
}

void PathTracingEditorPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		if (validation_pending && EditorNode::get_singleton()->get_edited_scene()) {
			if (!validation_render_scheduled) {
				validation_wait_frames++;
				if (validation_wait_frames < 30) {
					return;
				}
				_render_scene();
				validation_render_scheduled = true;
				return;
			}
			validation_render_scheduled = false;
			RenderingDevice *rd = RenderingDevice::get_singleton();
			const RID validation_target = reconstructed_target.is_valid() ? reconstructed_target : color_target;
			const PackedByteArray data = rd && validation_target.is_valid() ? rd->texture_get_data(validation_target, 0) : PackedByteArray();
			const PackedByteArray raw_data = rd && color_target.is_valid() ? rd->texture_get_data(color_target, 0) : PackedByteArray();
			const PackedByteArray specular_distance = rd && !guide_targets[8].is_empty() ? rd->texture_get_data(guide_targets[8][0], 0) : PackedByteArray();
			bool any_radiance = false;
			for (uint8_t byte : data) {
				any_radiance |= byte != 0;
			}
			const uint64_t radiance_hash = raw_data.is_empty() ? 0 : SceneCompiler::hash_payload(raw_data.ptr(), raw_data.size());
			uint32_t finite_specular_distance_pixels = 0;
			for (int64_t offset = 0; offset + 1 < specular_distance.size(); offset += 2) {
				const uint16_t half = uint16_t(specular_distance[offset]) | (uint16_t(specular_distance[offset + 1]) << 8);
				const uint16_t exponent = (half >> 10) & 0x1f;
				finite_specular_distance_pixels += exponent != 0x1f && (half & 0x7fff) != 0;
			}
			if (validation_stage == 0 && last_render_ok) {
				validation_first_hash = radiance_hash;
				validation_first_geometry_hash = last_geometry_hash;
				validation_first_specular_pixels = finite_specular_distance_pixels;
				Node *edited_root = EditorNode::get_singleton()->get_edited_scene();
				edited_root->set_process(false);
				if (MeshInstance3D *character = Object::cast_to<MeshInstance3D>(edited_root->get_node_or_null(NodePath("Character")))) {
					validation_morph_before = character->get_blend_shape_value(0);
					validation_morph_after = validation_morph_before < 0.5f ? 1.0f : 0.0f;
					character->set_blend_shape_value(0, validation_morph_after);
				}
				if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(edited_root->get_node_or_null(NodePath("Skeleton")))) {
					skeleton->set_bone_pose_position(0, Vector3(4.0f, 0.0f, 0.0f));
					skeleton->force_update_all_bone_transforms();
				}
				validation_stage = 1;
				validation_wait_frames = 0;
				return;
			}
			validation_pending = false;
			Dictionary report;
			report["schema"] = 1;
			Node *edited_root = EditorNode::get_singleton()->get_edited_scene();
			report["scene"] = edited_root ? edited_root->get_scene_file_path().get_file().get_basename() : String();
			report["backend"] = "metal_reference";
			report["bytes"] = data.size();
			report["nonzero_radiance"] = any_radiance;
			report["finite_specular_distance_pixels"] = finite_specular_distance_pixels;
			report["first_finite_specular_distance_pixels"] = validation_first_specular_pixels;
			report["animated_geometry_changed"] = validation_first_geometry_hash != last_geometry_hash;
			report["raw_frame_changed"] = validation_first_hash != radiance_hash;
			report["raw_hash_first"] = String::num_uint64(validation_first_hash, 16);
			report["raw_hash_second"] = String::num_uint64(radiance_hash, 16);
			report["morph_before"] = validation_morph_before;
			report["morph_after"] = validation_morph_after;
			if (MeshInstance3D *character = Object::cast_to<MeshInstance3D>(edited_root->get_node_or_null(NodePath("Character")))) {
				report["morph_at_readback"] = character->get_blend_shape_value(0);
			}
			report["dynamic_blas_refit"] = last_blas_refit;
			report["material_fallback"] = last_material_fallback;
			report["metalfx_temporal_denoised"] = reconstructed_target.is_valid();
			Dictionary timing_report;
			Vector<StageTiming> completed_timings;
			if (backend.collect_completed_timings(completed_timings) == OK) {
				for (const StageTiming &timing : completed_timings) {
					timing_report[String(timing.stage)] = timing.milliseconds;
				}
			}
			report["gpu_stage_ms"] = timing_report;
			print_line(JSON::stringify(report));
			const bool passed = last_render_ok && any_radiance && finite_specular_distance_pixels > 0 && validation_first_specular_pixels > 0 &&
					validation_first_geometry_hash != last_geometry_hash && last_blas_refit > 0 && !last_material_fallback;
			SceneTree::get_singleton()->quit(passed ? 0 : 1);
			return;
		}
		if (!panel->is_visible_in_tree() || !live_update->is_pressed() || freeze_accumulation->is_pressed()) {
			return;
		}
		update_accumulator += get_process_delta_time();
		if (update_accumulator >= 0.1) {
			update_accumulator = 0.0;
			_render_scene();
		}
	}
}

PathTracingEditorPlugin::PathTracingEditorPlugin() {
	for (const String &argument : OS::get_singleton()->get_cmdline_user_args()) {
		validation_pending |= argument == "--validate-path-tracing-editor";
	}
	panel = memnew(VBoxContainer);
	HBoxContainer *toolbar = memnew(HBoxContainer);
	panel->add_child(toolbar);
	mode = memnew(OptionButton);
	mode->add_item("Raster");
	mode->add_item("Path Traced Interactive");
	mode->add_item("Path Traced Progressive");
	mode->select(1);
	toolbar->add_child(mode);
	resolution = memnew(OptionButton);
	resolution->add_item("320 x 180");
	resolution->add_item("640 x 360");
	resolution->add_item("960 x 540");
	resolution->select(1);
	toolbar->add_child(resolution);
	bounces = memnew(SpinBox);
	bounces->set_min(1);
	bounces->set_max(16);
	bounces->set_value(2);
	bounces->set_tooltip_text("Path bounce limit");
	toolbar->add_child(bounces);
	live_update = memnew(CheckBox);
	live_update->set_text("Live");
	toolbar->add_child(live_update);
	freeze_accumulation = memnew(CheckBox);
	freeze_accumulation->set_text("Freeze");
	toolbar->add_child(freeze_accumulation);
	metalfx = memnew(CheckBox);
	metalfx->set_text("MetalFX");
#ifdef METAL_ENABLED
	metalfx->set_pressed(denoised_effect.is_supported());
	metalfx->set_disabled(!denoised_effect.is_supported());
#else
	metalfx->set_disabled(true);
#endif
	toolbar->add_child(metalfx);
	Button *render_button = memnew(Button);
	render_button->set_text("Render");
	toolbar->add_child(render_button);
	Button *reset_button = memnew(Button);
	reset_button->set_text("Reset History");
	toolbar->add_child(reset_button);
	guide = memnew(OptionButton);
	const char *guide_names[] = { "Radiance", "Depth", "Motion", "Normal", "Diffuse Albedo", "Specular Albedo", "Roughness", "Denoise Strength", "Reactive Mask", "Specular Distance", "Transparency" };
	for (const char *name : guide_names) {
		guide->add_item(name);
	}
	toolbar->add_child(guide);
	diagnostics = memnew(Label);
	diagnostics->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	panel->add_child(diagnostics);
	preview = memnew(TextureRect);
	preview->set_custom_minimum_size(Size2(640, 360));
	preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	panel->add_child(preview);
	preview_texture.instantiate();
	preview->set_texture(preview_texture);
	add_control_to_bottom_panel(panel, "Path Tracer");
	render_button->connect(SceneStringName(pressed), callable_mp(this, &PathTracingEditorPlugin::_render_scene));
	reset_button->connect(SceneStringName(pressed), callable_mp(this, &PathTracingEditorPlugin::_reset_history));
	mode->connect(SceneStringName(item_selected), callable_mp(this, &PathTracingEditorPlugin::_mode_changed));
	guide->connect(SceneStringName(item_selected), callable_mp(this, &PathTracingEditorPlugin::_guide_changed));
	metalfx->connect(SceneStringName(toggled), callable_mp(this, &PathTracingEditorPlugin::_reset_history).unbind(1));
	set_process(true);
#ifdef METAL_ENABLED
	const BackendCapabilities capabilities = backend.get_capabilities();
	diagnostics->set_text(capabilities.available ? "Metal reference backend available." : capabilities.unavailable_reason);
#else
	diagnostics->set_text("This editor was built without Metal path tracing.");
#endif
}

PathTracingEditorPlugin::~PathTracingEditorPlugin() {
	_free_targets();
	remove_control_from_bottom_panel(panel);
	memdelete(panel);
}
