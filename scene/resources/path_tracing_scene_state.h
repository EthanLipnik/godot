/**************************************************************************/
/*  path_tracing_scene_state.h                                            */
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

#pragma once

#include "path_tracing_resource_compiler.h"

#include "core/math/projection.h"
#include "core/templates/hash_map.h"

class Node;

namespace RendererPathTracing {

struct CameraViewInput {
	Transform3D world_from_view;
	Projection clip_from_view;
	uint32_t width = 0;
	uint32_t height = 0;
	float exposure = 1.0f;
	bool history_reset = false;
};

struct SceneStateResult {
	ResourceCompileResult resources;
	uint32_t light_count = 0;
	uint32_t environment_count = 0;
	uint32_t view_count = 0;
};

class PathTracingSceneState {
	struct PreviousMeshState {
		Ref<ArrayMesh> deformation;
		Transform3D transform;
	};

	HashMap<String, PreviousMeshState> previous_meshes;
	HashMap<uint32_t, CameraRecord> previous_cameras;

	static Matrix4 _matrix_from_transform(const Transform3D &p_transform);
	static Matrix4 _matrix_from_projection(const Projection &p_projection);
	static uint64_t _stable_id(const String &p_path);
	static void _collect_nodes(Node *p_node, Vector<Node *> &r_nodes);

public:
	Error compile(Node *p_root, const Vector<CameraViewInput> &p_views, PackedByteArray &r_capture, SceneStateResult &r_result, String *r_error = nullptr);
	void reset_history();
};

} // namespace RendererPathTracing
