/**************************************************************************/
/*  path_tracing_scene_state.cpp                                          */
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

#include "path_tracing_scene_state.h"

#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/main/node.h"

namespace RendererPathTracing {

struct PathTracingNodePathComparator {
	bool operator()(const Node *p_left, const Node *p_right) const {
		return String(p_left->get_path()) < String(p_right->get_path());
	}
};

static Error _state_fail(Error p_error, const String &p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

Matrix4 PathTracingSceneState::_matrix_from_transform(const Transform3D &p_transform) {
	Matrix4 output = {};
	for (uint32_t column = 0; column < 3; column++) {
		const Vector3 axis = p_transform.basis.get_column(column);
		output.columns[column] = { (float)axis.x, (float)axis.y, (float)axis.z, 0.0f };
	}
	output.columns[3] = { (float)p_transform.origin.x, (float)p_transform.origin.y, (float)p_transform.origin.z, 1.0f };
	return output;
}

Matrix4 PathTracingSceneState::_matrix_from_projection(const Projection &p_projection) {
	Matrix4 output = {};
	for (uint32_t column = 0; column < 4; column++) {
		output.columns[column] = { (float)p_projection.columns[column].x, (float)p_projection.columns[column].y,
			(float)p_projection.columns[column].z, (float)p_projection.columns[column].w };
	}
	return output;
}

uint64_t PathTracingSceneState::_stable_id(const String &p_path) {
	const CharString utf8 = p_path.utf8();
	const uint64_t hash = SceneCompiler::hash_payload(reinterpret_cast<const uint8_t *>(utf8.ptr()), utf8.length());
	return hash == 0 ? 1 : hash;
}

void PathTracingSceneState::_collect_nodes(Node *p_node, Vector<Node *> &r_nodes) {
	r_nodes.push_back(p_node);
	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_nodes(p_node->get_child(child), r_nodes);
	}
}

Error PathTracingSceneState::compile(Node *p_root, const Vector<CameraViewInput> &p_views, PackedByteArray &r_capture, SceneStateResult &r_result, String *r_error) {
	r_result = {};
	if (!p_root || p_views.is_empty() || p_views.size() > int(SCENE_PACKET_MAX_VIEWS)) {
		return _state_fail(ERR_INVALID_PARAMETER, "Path-tracing scene compilation requires a root and one or two views.", r_error);
	}
	SceneCaptureInput input;
	input.scene.guide_contract.schema_version = SCENE_PACKET_VERSION;
	input.scene.guide_contract.motion_direction = MOTION_CURRENT_TO_PREVIOUS;
	input.scene.guide_contract.motion_units = MOTION_NORMALIZED_UV;
	input.scene.guide_contract.depth_convention = DEPTH_REVERSED_ZERO_TO_ONE;
	input.scene.guide_contract.normal_space = NORMAL_WORLD_SPACE;
	input.scene.guide_contract.roughness_convention = ROUGHNESS_PERCEPTUAL_ZERO_TO_ONE;
	input.scene.guide_contract.invalid_pixel_convention = INVALID_PIXEL_QUIET_NAN;
	input.scene.guide_contract.uv_origin = UV_ORIGIN_TOP_LEFT;
	input.scene.guide_contract.color_space = COLOR_LINEAR_REC709;
	input.scene.guide_contract.enabled_guides = GUIDE_DEPTH | GUIDE_MOTION | GUIDE_NORMAL | GUIDE_DIFFUSE_ALBEDO |
			GUIDE_SPECULAR_ALBEDO | GUIDE_ROUGHNESS | GUIDE_DENOISE_STRENGTH | GUIDE_REACTIVE_MASK |
			GUIDE_SPECULAR_HIT_DISTANCE | GUIDE_TRANSPARENCY_OVERLAY;
	for (uint32_t view = 0; view < (uint32_t)p_views.size(); view++) {
		const CameraViewInput &source = p_views[view];
		if (source.width == 0 || source.height == 0) {
			return _state_fail(ERR_INVALID_PARAMETER, "A path-tracing view has zero dimensions.", r_error);
		}
		CameraRecord camera = {};
		camera.view_from_world = _matrix_from_transform(source.world_from_view.affine_inverse());
		camera.clip_from_view = _matrix_from_projection(source.clip_from_view);
		const CameraRecord *previous = previous_cameras.getptr(view);
		camera.previous_view_from_world = previous ? previous->view_from_world : camera.view_from_world;
		camera.previous_clip_from_view = previous ? previous->clip_from_view : camera.clip_from_view;
		camera.camera_relative_origin_and_exposure = { (float)source.world_from_view.origin.x, (float)source.world_from_view.origin.y, (float)source.world_from_view.origin.z, source.exposure };
		camera.view_index = view;
		camera.render_width = source.width;
		camera.render_height = source.height;
		camera.history_reset = source.history_reset || !previous;
		input.scene.cameras.push_back(camera);
		previous_cameras.insert(view, camera);
	}
	input.scene.guide_contract.motion_to_pixel_scale_x = p_views[0].width;
	input.scene.guide_contract.motion_to_pixel_scale_y = p_views[0].height;

	Vector<Node *> nodes;
	_collect_nodes(p_root, nodes);
	nodes.sort_custom<PathTracingNodePathComparator>();
	for (Node *node : nodes) {
		const String path = String(node->get_path());
		const uint64_t stable_id = _stable_id(path);
		if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(node)) {
			if (!mesh_instance->is_visible_in_tree() || mesh_instance->get_mesh().is_null()) {
				continue;
			}
			const PreviousMeshState *previous = previous_meshes.getptr(path);
			Ref<ArrayMesh> current_deformation;
			const uint32_t instance_id = uint32_t(stable_id) == 0 ? 1 : uint32_t(stable_id);
			const Error mesh_error = ResourceCompiler::append_mesh_instance(mesh_instance, previous ? previous->deformation : Ref<ArrayMesh>(), stable_id,
					instance_id, previous ? previous->transform : mesh_instance->get_global_transform(), input, current_deformation, r_result.resources, r_error);
			if (mesh_error != OK) {
				return mesh_error;
			}
			PreviousMeshState current;
			current.deformation = current_deformation;
			current.transform = mesh_instance->get_global_transform();
			previous_meshes.insert(path, current);
		} else if (Light3D *light = Object::cast_to<Light3D>(node)) {
			if (light->is_visible_in_tree()) {
				const Error light_error = ResourceCompiler::append_light(light, uint32_t(stable_id), input, r_error);
				if (light_error != OK) {
					return light_error;
				}
				r_result.light_count++;
			}
		} else if (WorldEnvironment *world_environment = Object::cast_to<WorldEnvironment>(node)) {
			if (world_environment->get_environment().is_valid()) {
				const Error environment_error = ResourceCompiler::append_environment(world_environment->get_environment(), uint32_t(stable_id), input, r_error);
				if (environment_error != OK) {
					return environment_error;
				}
				r_result.environment_count++;
			}
		}
	}
	r_result.view_count = p_views.size();
	return SceneCapture::compile(input, r_capture, r_error);
}

void PathTracingSceneState::reset_history() {
	previous_meshes.clear();
	previous_cameras.clear();
}

} // namespace RendererPathTracing
