/**************************************************************************/
/*  path_tracing_resource_compiler.cpp                                    */
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

#include "path_tracing_resource_compiler.h"

#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/environment.h"

namespace RendererPathTracing {

static Error _resource_fail(Error p_error, const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

Matrix4 ResourceCompiler::_matrix_from_transform(const Transform3D &p_transform) {
	Matrix4 matrix = {};
	for (uint32_t column = 0; column < 3; column++) {
		const Vector3 axis = p_transform.basis.get_column(column);
		matrix.columns[column] = { (float)axis.x, (float)axis.y, (float)axis.z, 0.0f };
	}
	matrix.columns[3] = { (float)p_transform.origin.x, (float)p_transform.origin.y, (float)p_transform.origin.z, 1.0f };
	return matrix;
}

MaterialRecord ResourceCompiler::_compile_material(const Ref<Material> &p_material, uint32_t p_surface, ResourceCompileResult &r_result) {
	MaterialRecord output = {};
	output.base_color_and_opacity = { 1.0f, 0.0f, 1.0f, 1.0f };
	output.specular_f0_and_perceptual_roughness = { 0.04f, 0.04f, 0.04f, 0.5f };
	output.transmission_ior_alpha_cutoff_unused.y = 1.5f;

	Ref<BaseMaterial3D> material = p_material;
	if (material.is_null()) {
		ResourceCompileDiagnostic diagnostic;
		diagnostic.surface = p_surface;
		diagnostic.message = "Surface has no StandardMaterial3D; using the visible magenta fallback closure.";
		diagnostic.material_fallback = true;
		r_result.diagnostics.push_back(diagnostic);
		r_result.has_material_fallback = true;
		return output;
	}

	bool unsupported = material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED;
	for (uint32_t texture = 0; texture < BaseMaterial3D::TEXTURE_MAX; texture++) {
		unsupported |= material->get_texture((BaseMaterial3D::TextureParam)texture).is_valid();
	}
	for (uint32_t feature = BaseMaterial3D::FEATURE_NORMAL_MAPPING; feature < BaseMaterial3D::FEATURE_MAX; feature++) {
		unsupported |= material->get_feature((BaseMaterial3D::Feature)feature);
	}
	if (unsupported) {
		ResourceCompileDiagnostic diagnostic;
		diagnostic.surface = p_surface;
		diagnostic.message = "Surface uses a StandardMaterial3D feature outside the initial scalar diffuse/metallic/roughness/emission closure; using the visible magenta fallback closure.";
		diagnostic.material_fallback = true;
		r_result.diagnostics.push_back(diagnostic);
		r_result.has_material_fallback = true;
		return output;
	}

	const Color albedo = material->get_albedo().srgb_to_linear();
	const Color emission = material->get_emission().srgb_to_linear();
	const float metallic = material->get_metallic();
	const float specular = material->get_specular();
	output.base_color_and_opacity = { albedo.r, albedo.g, albedo.b, albedo.a };
	output.emission_and_strength = { emission.r, emission.g, emission.b, material->get_emission_energy_multiplier() };
	output.specular_f0_and_perceptual_roughness = {
		Math::lerp(0.08f * specular, albedo.r, metallic),
		Math::lerp(0.08f * specular, albedo.g, metallic),
		Math::lerp(0.08f * specular, albedo.b, metallic),
		material->get_roughness()
	};
	output.transmission_ior_alpha_cutoff_unused.x = metallic;
	return output;
}

Error ResourceCompiler::append_mesh(const Ref<Mesh> &p_mesh, uint64_t p_first_geometry_id, uint32_t p_first_instance_id,
		const Transform3D &p_current_transform, const Transform3D &p_previous_transform,
		SceneCaptureInput &r_capture, ResourceCompileResult &r_result, String *r_error) {
	return append_deformed_mesh(p_mesh, Ref<Mesh>(), p_first_geometry_id, p_first_instance_id, p_current_transform, p_previous_transform, r_capture, r_result, r_error);
}

Error ResourceCompiler::append_deformed_mesh(const Ref<Mesh> &p_current_mesh, const Ref<Mesh> &p_previous_mesh,
		uint64_t p_first_geometry_id, uint32_t p_first_instance_id,
		const Transform3D &p_current_transform, const Transform3D &p_previous_transform,
		SceneCaptureInput &r_capture, ResourceCompileResult &r_result, String *r_error) {
	const Ref<Mesh> &p_mesh = p_current_mesh;
	if (p_mesh.is_null() || p_first_geometry_id == 0 || p_first_instance_id == 0) {
		return _resource_fail(ERR_INVALID_PARAMETER, "A path-tracing mesh compile request is invalid.", r_error);
	}
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			ResourceCompileDiagnostic diagnostic;
			diagnostic.surface = surface;
			diagnostic.message = "Non-triangle surface omitted from path tracing.";
			r_result.diagnostics.push_back(diagnostic);
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(surface);
		if (arrays.size() != Mesh::ARRAY_MAX) {
			return _resource_fail(ERR_INVALID_DATA, "A path-tracing mesh surface has an invalid array layout.", r_error);
		}
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		PackedVector3Array previous_positions;
		if (p_previous_mesh.is_valid() && surface < p_previous_mesh->get_surface_count()) {
			const Array previous_arrays = p_previous_mesh->surface_get_arrays(surface);
			if (previous_arrays.size() == Mesh::ARRAY_MAX) {
				previous_positions = previous_arrays[Mesh::ARRAY_VERTEX];
			}
		}
		if (previous_positions.size() != positions.size()) {
			previous_positions = positions;
		}
		const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
		const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
		const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		if (positions.is_empty() || (!indices.is_empty() && indices.size() % 3 != 0) || (indices.is_empty() && positions.size() % 3 != 0)) {
			return _resource_fail(ERR_INVALID_DATA, "A path-tracing triangle surface has invalid vertex or index counts.", r_error);
		}

		GeometryInput geometry;
		geometry.geometry_id = p_first_geometry_id + surface;
		geometry.flags = GEOMETRY_OPAQUE | (p_previous_mesh.is_valid() ? GEOMETRY_DYNAMIC : 0);
		const AABB bounds = p_mesh->get_aabb();
		geometry.bounds_min = { (float)bounds.position.x, (float)bounds.position.y, (float)bounds.position.z, 0.0f };
		const Vector3 bounds_end = bounds.get_end();
		geometry.bounds_max = { (float)bounds_end.x, (float)bounds_end.y, (float)bounds_end.z, 0.0f };
		for (int vertex_index = 0; vertex_index < positions.size(); vertex_index++) {
			const Vector3 position = positions[vertex_index];
			const Vector3 normal = normals.size() == positions.size() ? normals[vertex_index] : Vector3();
			const Vector2 uv = uvs.size() == positions.size() ? uvs[vertex_index] : Vector2();
			GeometryVertex vertex = {};
			vertex.current_position = { (float)position.x, (float)position.y, (float)position.z, 1.0f };
			const Vector3 previous_position = previous_positions[vertex_index];
			vertex.previous_position = { (float)previous_position.x, (float)previous_position.y, (float)previous_position.z, 1.0f };
			vertex.normal = { (float)normal.x, (float)normal.y, (float)normal.z, 0.0f };
			if (tangents.size() == positions.size() * 4) {
				vertex.tangent = { tangents[vertex_index * 4], tangents[vertex_index * 4 + 1], tangents[vertex_index * 4 + 2], tangents[vertex_index * 4 + 3] };
			}
			vertex.uv = { (float)uv.x, (float)uv.y, 0.0f, 0.0f };
			geometry.vertices.push_back(vertex);
		}
		if (indices.is_empty()) {
			for (uint32_t index = 0; index < (uint32_t)positions.size(); index++) {
				geometry.indices.push_back(index);
			}
		} else {
			for (int32_t index : indices) {
				if (index < 0) {
					return _resource_fail(ERR_INVALID_DATA, "A path-tracing mesh index is negative.", r_error);
				}
				geometry.indices.push_back(index);
			}
		}
		r_capture.geometries.push_back(geometry);

		const MaterialRecord material = _compile_material(p_mesh->surface_get_material(surface), surface, r_result);
		r_capture.scene.materials.push_back(material);
		InstanceRecord instance = {};
		instance.world_from_object = _matrix_from_transform(p_current_transform);
		instance.previous_world_from_object = _matrix_from_transform(p_previous_transform);
		instance.geometry_id = geometry.geometry_id;
		instance.material_id = r_capture.scene.materials.size();
		instance.instance_id = p_first_instance_id + surface;
		instance.visibility_mask = RAY_VISIBILITY_PRIMARY | RAY_VISIBILITY_SHADOW |
				RAY_VISIBILITY_REFLECTION_REFRACTION | RAY_VISIBILITY_DIFFUSE_INDIRECT | RAY_VISIBILITY_EDITOR_DEBUG;
		r_capture.scene.instances.push_back(instance);
		r_result.geometry_count++;
		r_result.material_count++;
		r_result.instance_count++;
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error ResourceCompiler::append_mesh_instance(MeshInstance3D *p_instance, const Ref<ArrayMesh> &p_previous_deformation,
		uint64_t p_first_geometry_id, uint32_t p_first_instance_id, const Transform3D &p_previous_transform,
		SceneCaptureInput &r_capture, Ref<ArrayMesh> &r_current_deformation,
		ResourceCompileResult &r_result, String *r_error) {
	if (!p_instance) {
		return _resource_fail(ERR_INVALID_PARAMETER, "A path-tracing mesh instance is required.", r_error);
	}
	r_current_deformation = p_instance->bake_mesh_from_current_deformation();
	if (r_current_deformation.is_null()) {
		return _resource_fail(ERR_CANT_CREATE, "The current Godot mesh deformation could not be baked for path tracing.", r_error);
	}
	return append_deformed_mesh(r_current_deformation, p_previous_deformation, p_first_geometry_id, p_first_instance_id,
			p_instance->get_global_transform(), p_previous_transform, r_capture, r_result, r_error);
}

Error ResourceCompiler::append_light(const Light3D *p_light, uint32_t p_light_id, SceneCaptureInput &r_capture, String *r_error) {
	if (!p_light || p_light_id == 0) {
		return _resource_fail(ERR_INVALID_PARAMETER, "A path-tracing light compile request is invalid.", r_error);
	}
	LightRecord light = {};
	const Transform3D transform = p_light->is_inside_tree() ? p_light->get_global_transform() : p_light->get_transform();
	if (p_light->get_light_type() == RSE::LIGHT_SPOT) {
		return _resource_fail(ERR_UNAVAILABLE, "SpotLight3D is outside the initial path-tracing analytic-light subset because schema 1 does not encode its direction.", r_error);
	}
	uint32_t type = LIGHT_POINT;
	if (p_light->get_light_type() == RSE::LIGHT_DIRECTIONAL) {
		type = LIGHT_DIRECTIONAL;
		const Vector3 direction = -transform.basis.get_column(Vector3::AXIS_Z).normalized();
		light.position_or_direction_and_type = { (float)direction.x, (float)direction.y, (float)direction.z, float(type) };
	} else {
		type = LIGHT_POINT;
		light.position_or_direction_and_type = { (float)transform.origin.x, (float)transform.origin.y, (float)transform.origin.z, float(type) };
	}
	const Color color = (p_light->get_color() * p_light->get_correlated_color()).srgb_to_linear();
	light.linear_color_and_intensity = { color.r, color.g, color.b, (float)p_light->get_param(Light3D::PARAM_ENERGY) };
	light.shape_parameters = { (float)p_light->get_param(Light3D::PARAM_RANGE), (float)p_light->get_param(Light3D::PARAM_ATTENUATION),
		(float)p_light->get_param(Light3D::PARAM_SPOT_ANGLE), (float)p_light->get_param(Light3D::PARAM_SPOT_ATTENUATION) };
	light.light_id = p_light_id;
	light.visibility_mask = p_light->get_cull_mask();
	r_capture.scene.lights.push_back(light);
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error ResourceCompiler::append_environment(const Ref<Environment> &p_environment, uint32_t p_light_id, SceneCaptureInput &r_capture, String *r_error) {
	if (p_environment.is_null() || p_light_id == 0) {
		return _resource_fail(ERR_INVALID_PARAMETER, "A path-tracing environment compile request is invalid.", r_error);
	}
	if (p_environment->get_background() != Environment::BG_COLOR) {
		return _resource_fail(ERR_UNAVAILABLE, "The initial path-tracing environment compiler supports color backgrounds only.", r_error);
	}
	const Color color = p_environment->get_bg_color().srgb_to_linear();
	LightRecord light = {};
	light.position_or_direction_and_type.w = LIGHT_ENVIRONMENT;
	light.linear_color_and_intensity = { color.r, color.g, color.b, p_environment->get_bg_energy_multiplier() };
	light.light_id = p_light_id;
	light.visibility_mask = UINT32_MAX;
	r_capture.scene.lights.push_back(light);
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

} // namespace RendererPathTracing
