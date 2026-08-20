/**************************************************************************/
/*  path_tracing_resource_compiler.h                                      */
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

#include "scene/resources/mesh.h"
#include "servers/rendering/path_tracing/path_tracing_scene_capture.h"

class MeshInstance3D;
class Light3D;
class Environment;

namespace RendererPathTracing {

struct ResourceCompileDiagnostic {
	uint32_t surface = 0;
	String message;
	bool material_fallback = false;
};

struct ResourceCompileResult {
	Vector<ResourceCompileDiagnostic> diagnostics;
	uint32_t geometry_count = 0;
	uint32_t material_count = 0;
	uint32_t instance_count = 0;
	bool has_material_fallback = false;
};

class ResourceCompiler {
	static Matrix4 _matrix_from_transform(const Transform3D &p_transform);
	static MaterialRecord _compile_material(const Ref<Material> &p_material, uint32_t p_surface, ResourceCompileResult &r_result);

public:
	static Error append_mesh(const Ref<Mesh> &p_mesh, uint64_t p_first_geometry_id, uint32_t p_first_instance_id,
			const Transform3D &p_current_transform, const Transform3D &p_previous_transform,
			SceneCaptureInput &r_capture, ResourceCompileResult &r_result, String *r_error = nullptr);
	static Error append_deformed_mesh(const Ref<Mesh> &p_current_mesh, const Ref<Mesh> &p_previous_mesh,
			uint64_t p_first_geometry_id, uint32_t p_first_instance_id,
			const Transform3D &p_current_transform, const Transform3D &p_previous_transform,
			SceneCaptureInput &r_capture, ResourceCompileResult &r_result, String *r_error = nullptr);
	static Error append_mesh_instance(MeshInstance3D *p_instance, const Ref<ArrayMesh> &p_previous_deformation,
			uint64_t p_first_geometry_id, uint32_t p_first_instance_id, const Transform3D &p_previous_transform,
			SceneCaptureInput &r_capture, Ref<ArrayMesh> &r_current_deformation,
			ResourceCompileResult &r_result, String *r_error = nullptr);
	static Error append_light(const Light3D *p_light, uint32_t p_light_id, SceneCaptureInput &r_capture, String *r_error = nullptr);
	static Error append_environment(const Ref<Environment> &p_environment, uint32_t p_light_id, SceneCaptureInput &r_capture, String *r_error = nullptr);
};

} // namespace RendererPathTracing
