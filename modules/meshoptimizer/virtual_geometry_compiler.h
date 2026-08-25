/**************************************************************************/
/*  virtual_geometry_compiler.h                                           */
/**************************************************************************/

#pragma once

#include "servers/rendering/virtual_geometry/virtual_geometry_format.h"

class VirtualGeometryCompiler {
public:
	struct Input {
		uint64_t source_asset_identity = 0;
		uint64_t source_primitive_identity = 0;
		PackedVector3Array positions;
		PackedInt32Array indices;
		PackedVector3Array normals;
		PackedFloat32Array tangents; // xyzw per vertex.
		PackedVector2Array uv0;
		PackedVector2Array uv1;
		PackedColorArray colors;
		PackedInt32Array joints; // four influences per vertex.
		PackedFloat32Array weights; // four influences per vertex.
		PackedInt32Array triangle_materials; // optional, one semantic ID per triangle.
		bool opaque = true;
		bool double_sided = false;
		bool immutable = true;
		bool conventional_path_only = false;
		String conventional_path_reason;
	};

	struct Settings {
		uint32_t max_vertices_per_cluster = 64;
		uint32_t max_triangles_per_cluster = 124;
		uint32_t clusters_per_group = 4;
		uint64_t max_decoded_page_bytes = 64 * 1024;
		uint32_t compiler_semantic_generation = RendererVirtualGeometry::COMPILER_SEMANTIC_GENERATION;
	};

	Error compile(const Input &p_input, const Settings &p_settings, RendererVirtualGeometry::Package &r_package);
	const Vector<String> &get_diagnostics() const { return diagnostics; }

private:
	Vector<String> diagnostics;
};
