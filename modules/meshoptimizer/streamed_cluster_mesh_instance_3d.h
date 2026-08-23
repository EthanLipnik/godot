/**************************************************************************/
/*  streamed_cluster_mesh_instance_3d.h                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "streamed_cluster_mesh.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/path_tracing/streamed_cluster_runtime.h"

#include "core/templates/hash_set.h"

class MeshInstance3D;

class StreamedClusterMeshInstance3D : public Node3D {
	GDCLASS(StreamedClusterMeshInstance3D, Node3D);

	Ref<StreamedClusterMesh> streamed_mesh;
	RendererPathTracing::StreamedClusterResidencyManager residency;
	HashMap<uint64_t, Ref<ArrayMesh>> resident_meshes;
	HashMap<uint64_t, MeshInstance3D *> active_instances;
	Vector<AABB> external_eye_regions;
	uint64_t frame = 0;
	float streaming_radius = 250.0f;
	float maximum_geometric_error = 0.25f;
	uint64_t maximum_resident_bytes = 512ull * 1024ull * 1024ull;
	uint64_t maximum_load_bytes_per_frame = 32ull * 1024ull * 1024ull;
	uint32_t maximum_load_tasks_per_frame = 8;

	Error _rebuild_manifest();
	Ref<ArrayMesh> _decode_page_mesh(int p_page, Error &r_error) const;
	void _update_streaming();
	void _reconcile_instances(const Vector<RendererPathTracing::StreamedClusterSurface> &p_surfaces);
	void _clear_instances();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	StreamedClusterMeshInstance3D();
	~StreamedClusterMeshInstance3D();

	void set_streamed_mesh(const Ref<StreamedClusterMesh> &p_mesh);
	Ref<StreamedClusterMesh> get_streamed_mesh() const { return streamed_mesh; }
	void set_streaming_radius(float p_radius) { streaming_radius = MAX(0.01f, p_radius); }
	float get_streaming_radius() const { return streaming_radius; }
	void set_maximum_geometric_error(float p_error) { maximum_geometric_error = MAX(0.0f, p_error); }
	float get_maximum_geometric_error() const { return maximum_geometric_error; }
	void set_maximum_resident_bytes(uint64_t p_bytes);
	uint64_t get_maximum_resident_bytes() const { return maximum_resident_bytes; }
	void set_maximum_load_bytes_per_frame(uint64_t p_bytes);
	uint64_t get_maximum_load_bytes_per_frame() const { return maximum_load_bytes_per_frame; }
	void set_maximum_load_tasks_per_frame(uint32_t p_tasks);
	uint32_t get_maximum_load_tasks_per_frame() const { return maximum_load_tasks_per_frame; }
	void set_eye_regions(const Array &p_regions);
	Array get_eye_regions() const;
	Dictionary get_streaming_diagnostics() const;
};
