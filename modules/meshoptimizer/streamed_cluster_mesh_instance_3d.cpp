/**************************************************************************/
/*  streamed_cluster_mesh_instance_3d.cpp                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "streamed_cluster_mesh_instance_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"

using namespace RendererPathTracing;

StreamedClusterMeshInstance3D::StreamedClusterMeshInstance3D() {
	set_process_internal(true);
}

StreamedClusterMeshInstance3D::~StreamedClusterMeshInstance3D() {
}

void StreamedClusterMeshInstance3D::set_streamed_mesh(const Ref<StreamedClusterMesh> &p_mesh) {
	if (streamed_mesh == p_mesh) {
		return;
	}
	streamed_mesh = p_mesh;
	_clear_instances();
	resident_meshes.clear();
	residency.clear();
	frame = 0;
	if (streamed_mesh.is_valid()) {
		const Error err = _rebuild_manifest();
		if (err != OK) {
			ERR_PRINT(vformat("StreamedClusterMeshInstance3D rejected its mesh manifest (error %d).", err));
		}
	}
	notify_property_list_changed();
}

void StreamedClusterMeshInstance3D::set_maximum_resident_bytes(uint64_t p_bytes) {
	maximum_resident_bytes = MAX(uint64_t(1), p_bytes);
	StreamedClusterBudgets value = residency.get_budgets();
	value.maximum_resident_bytes = maximum_resident_bytes;
	residency.set_budgets(value);
}

void StreamedClusterMeshInstance3D::set_maximum_load_bytes_per_frame(uint64_t p_bytes) {
	maximum_load_bytes_per_frame = MAX(uint64_t(1), p_bytes);
	StreamedClusterBudgets value = residency.get_budgets();
	value.maximum_load_bytes_per_frame = maximum_load_bytes_per_frame;
	residency.set_budgets(value);
}

void StreamedClusterMeshInstance3D::set_maximum_load_tasks_per_frame(uint32_t p_tasks) {
	maximum_load_tasks_per_frame = MAX(1u, p_tasks);
	StreamedClusterBudgets value = residency.get_budgets();
	value.maximum_load_tasks_per_frame = maximum_load_tasks_per_frame;
	residency.set_budgets(value);
}

void StreamedClusterMeshInstance3D::set_eye_regions(const Array &p_regions) {
	external_eye_regions.clear();
	for (int i = 0; i < p_regions.size(); i++) {
		ERR_CONTINUE_MSG(p_regions[i].get_type() != Variant::AABB, "Streamed cluster eye regions must contain AABBs in local mesh space.");
		external_eye_regions.push_back(p_regions[i]);
	}
}

Array StreamedClusterMeshInstance3D::get_eye_regions() const {
	Array result;
	for (const AABB &region : external_eye_regions) {
		result.push_back(region);
	}
	return result;
}

Error StreamedClusterMeshInstance3D::_rebuild_manifest() {
	ERR_FAIL_COND_V(streamed_mesh.is_null() || streamed_mesh->get_format_version() != StreamedClusterMesh::FORMAT_VERSION, ERR_INVALID_DATA);
	Vector<StreamedClusterPageDescriptor> descriptors;
	for (int i = 0; i < streamed_mesh->get_page_count(); i++) {
		const StreamedClusterMesh::Page *page = streamed_mesh->get_page(i);
		ERR_FAIL_NULL_V(page, ERR_INVALID_DATA);
		StreamedClusterPageDescriptor descriptor;
		descriptor.stable_id = page->stable_id;
		descriptor.revision = page->revision;
		descriptor.parent_id = page->parent_id;
		for (int64_t child_id : page->child_ids) {
			descriptor.child_ids.push_back(uint64_t(child_id));
		}
		descriptor.bounds = page->bounds;
		descriptor.geometric_error = page->geometric_error;
		descriptor.blob_bytes = page->blob_length;
		ERR_FAIL_COND_V_MSG(uint64_t(page->vertex_count) * 12ull > uint64_t(UINT32_MAX) || uint64_t(page->index_count) * 4ull > uint64_t(UINT32_MAX), ERR_INVALID_DATA, "Streamed cluster page GPU data exceeds uint32 limits.");
		descriptor.vertex_bytes = page->vertex_count * 12;
		descriptor.index_bytes = page->index_count * 4;
		descriptor.vertex_count = page->vertex_count;
		descriptor.index_count = page->index_count;
		descriptor.material_index = page->material_index;
		descriptor.lod_level = page->lod_level;
		descriptor.persistent = page->persistent;
		descriptors.push_back(descriptor);
	}
	StreamedClusterBudgets budget;
	budget.maximum_resident_bytes = maximum_resident_bytes;
	budget.maximum_load_bytes_per_frame = maximum_load_bytes_per_frame;
	budget.maximum_load_tasks_per_frame = maximum_load_tasks_per_frame;
	residency.set_budgets(budget);
	return residency.set_manifest(descriptors);
}

Ref<ArrayMesh> StreamedClusterMeshInstance3D::_decode_page_mesh(int p_page, Error &r_error) const {
	r_error = OK;
	const StreamedClusterMesh::Page *page = streamed_mesh->get_page(p_page);
	ERR_FAIL_NULL_V(page, Ref<ArrayMesh>());
	const PackedByteArray payload = streamed_mesh->load_page_payload(p_page, &r_error);
	if (r_error != OK) {
		return Ref<ArrayMesh>();
	}
	const uint64_t vertex_bytes = uint64_t(page->vertex_count) * 12ull;
	const uint64_t index_bytes = uint64_t(page->index_count) * 4ull;
	if (uint64_t(payload.size()) != vertex_bytes + index_bytes) {
		r_error = ERR_FILE_CORRUPT;
		return Ref<ArrayMesh>();
	}
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedInt32Array indices;
	if (vertices.resize(page->vertex_count) != OK || normals.resize(page->vertex_count) != OK || indices.resize(page->index_count) != OK) {
		r_error = ERR_OUT_OF_MEMORY;
		return Ref<ArrayMesh>();
	}
	const uint8_t *read = payload.ptr();
	for (uint32_t vertex = 0; vertex < page->vertex_count; vertex++) {
		float packed[3];
		memcpy(packed, read + uint64_t(vertex) * 12ull, sizeof(packed));
		vertices.set(vertex, Vector3(packed[0], packed[1], packed[2]));
		normals.set(vertex, Vector3());
	}
	for (uint32_t index = 0; index < page->index_count; index++) {
		uint32_t packed;
		memcpy(&packed, read + vertex_bytes + uint64_t(index) * 4ull, sizeof(packed));
		if (packed >= page->vertex_count) {
			r_error = ERR_FILE_CORRUPT;
			return Ref<ArrayMesh>();
		}
		indices.set(index, int32_t(packed));
	}
	for (uint32_t triangle = 0; triangle < page->index_count; triangle += 3) {
		const int32_t a = indices[triangle + 0];
		const int32_t b = indices[triangle + 1];
		const int32_t c = indices[triangle + 2];
		const Vector3 normal = (vertices[b] - vertices[a]).cross(vertices[c] - vertices[a]);
		normals.set(a, normals[a] + normal);
		normals.set(b, normals[b] + normal);
		normals.set(c, normals[c] + normal);
	}
	for (uint32_t vertex = 0; vertex < page->vertex_count; vertex++) {
		normals.set(vertex, normals[vertex].normalized());
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	// ArrayMesh exposes this operation as void, so use its observable result as
	// the failure signal rather than continuing with a silently empty mesh.
	if (mesh->get_surface_count() != 1) {
		r_error = FAILED;
		return Ref<ArrayMesh>();
	}
	const TypedArray<Material> materials = streamed_mesh->get_materials();
	if (page->material_index < uint32_t(materials.size())) {
		const Ref<Material> material = materials[page->material_index];
		if (material.is_valid()) {
			mesh->surface_set_material(0, material);
		} else {
			WARN_PRINT_ONCE("StreamedClusterMesh contains an empty material slot; the page will use Godot's default material.");
		}
	} else {
		WARN_PRINT_ONCE("StreamedClusterMesh page references a missing material slot; the page will use Godot's default material.");
	}
	return mesh;
}

void StreamedClusterMeshInstance3D::_reconcile_instances(const Vector<StreamedClusterSurface> &p_surfaces) {
	HashSet<uint64_t> active_ids;
	for (const StreamedClusterSurface &surface : p_surfaces) {
		active_ids.insert(surface.stable_id);
		if (active_instances.has(surface.stable_id)) {
			continue;
		}
		const Ref<ArrayMesh> *mesh = resident_meshes.getptr(surface.stable_id);
		if (!mesh || mesh->is_null()) {
			continue;
		}
		MeshInstance3D *instance = memnew(MeshInstance3D);
		instance->set_name("ClusterPage_" + String::num_uint64(surface.stable_id, 16).pad_zeros(16));
		instance->set_mesh(*mesh);
		add_child(instance, false, INTERNAL_MODE_BACK);
		active_instances.insert(surface.stable_id, instance);
	}
	Vector<uint64_t> removed;
	for (const KeyValue<uint64_t, MeshInstance3D *> &pair : active_instances) {
		if (!active_ids.has(pair.key)) {
			removed.push_back(pair.key);
			if (pair.value) {
				if (pair.value->get_parent() == this) {
					remove_child(pair.value);
				}
				pair.value->queue_free();
			}
		}
	}
	for (uint64_t stable_id : removed) {
		active_instances.erase(stable_id);
	}
}

void StreamedClusterMeshInstance3D::_update_streaming() {
	if (streamed_mesh.is_null() || streamed_mesh->get_page_count() == 0) {
		return;
	}
	frame++;
	residency.update_completion_tokens(frame, frame);
	StreamedClusterSelection selection;
	selection.frame = frame;
	selection.maximum_geometric_error = maximum_geometric_error;
	selection.eye_regions = external_eye_regions;
	if (selection.eye_regions.is_empty() && get_viewport()) {
		Camera3D *camera = get_viewport()->get_camera_3d();
		if (camera) {
			const Vector3 center = to_local(camera->get_global_position());
			selection.eye_regions.push_back(AABB(center - Vector3(streaming_radius, streaming_radius, streaming_radius), Vector3(streaming_radius * 2.0f, streaming_radius * 2.0f, streaming_radius * 2.0f)));
		}
	}
	residency.update_selection(selection);
	const Vector<StreamedClusterLoadTask> loads = residency.take_load_tasks();
	for (const StreamedClusterLoadTask &load : loads) {
		const int page_index = streamed_mesh->find_page(load.stable_id);
		Error load_error = page_index >= 0 ? OK : ERR_DOES_NOT_EXIST;
		Ref<ArrayMesh> page_mesh = page_index >= 0 ? _decode_page_mesh(page_index, load_error) : Ref<ArrayMesh>();
		if (load_error == OK && page_mesh.is_valid()) {
			resident_meshes.insert(load.stable_id, page_mesh);
			const uint64_t rid_base = (load.stable_id & 0x3fffffffffffffffull) * 2ull;
			residency.complete_load(load.task_token, frame + 1, RID::from_uint64(rid_base + 1), RID::from_uint64(rid_base + 2), true);
		} else {
			residency.complete_load(load.task_token, frame + 1, RID(), RID(), false);
			ERR_PRINT(vformat("Streamed cluster page %d could not be loaded (error %d).", load.stable_id, load_error));
		}
	}
	const Vector<StreamedClusterRetireTask> retirements = residency.take_retire_tasks();
	for (const StreamedClusterRetireTask &retirement : retirements) {
		residency.submit_retirement(retirement.stable_id, frame + 3);
	}
	_reconcile_instances(residency.get_active_surfaces());
	Vector<uint64_t> released;
	for (const KeyValue<uint64_t, Ref<ArrayMesh>> &pair : resident_meshes) {
		if (residency.get_page_state(pair.key) == StreamedClusterPageState::UNLOADED) {
			released.push_back(pair.key);
		}
	}
	for (uint64_t stable_id : released) {
		resident_meshes.erase(stable_id);
	}
}

void StreamedClusterMeshInstance3D::_clear_instances() {
	for (const KeyValue<uint64_t, MeshInstance3D *> &pair : active_instances) {
		if (pair.value && !pair.value->is_queued_for_deletion()) {
			if (pair.value->get_parent() == this) {
				remove_child(pair.value);
			}
			pair.value->queue_free();
		}
	}
	active_instances.clear();
}

Dictionary StreamedClusterMeshInstance3D::get_streaming_diagnostics() const {
	const StreamedClusterFrameStats &stats = residency.get_frame_stats();
	Dictionary result;
	result["requested"] = stats.requested;
	result["loading"] = stats.loading;
	result["resident"] = stats.resident;
	result["active"] = stats.active;
	result["retiring"] = stats.retiring;
	result["resident_bytes"] = int64_t(stats.resident_bytes);
	result["reserved_bytes"] = int64_t(stats.reserved_bytes);
	result["transfer_bytes"] = int64_t(stats.transfer_bytes);
	result["budget_misses"] = stats.budget_misses;
	result["coarse_fallbacks"] = stats.coarse_fallbacks;
	result["selection_time_usec"] = int64_t(stats.selection_time_usec);
	return result;
}

void StreamedClusterMeshInstance3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
		_update_streaming();
	}
}

void StreamedClusterMeshInstance3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_streamed_mesh", "mesh"), &StreamedClusterMeshInstance3D::set_streamed_mesh);
	ClassDB::bind_method(D_METHOD("get_streamed_mesh"), &StreamedClusterMeshInstance3D::get_streamed_mesh);
	ClassDB::bind_method(D_METHOD("set_streaming_radius", "radius"), &StreamedClusterMeshInstance3D::set_streaming_radius);
	ClassDB::bind_method(D_METHOD("get_streaming_radius"), &StreamedClusterMeshInstance3D::get_streaming_radius);
	ClassDB::bind_method(D_METHOD("set_maximum_geometric_error", "error"), &StreamedClusterMeshInstance3D::set_maximum_geometric_error);
	ClassDB::bind_method(D_METHOD("get_maximum_geometric_error"), &StreamedClusterMeshInstance3D::get_maximum_geometric_error);
	ClassDB::bind_method(D_METHOD("set_maximum_resident_bytes", "bytes"), &StreamedClusterMeshInstance3D::set_maximum_resident_bytes);
	ClassDB::bind_method(D_METHOD("get_maximum_resident_bytes"), &StreamedClusterMeshInstance3D::get_maximum_resident_bytes);
	ClassDB::bind_method(D_METHOD("set_maximum_load_bytes_per_frame", "bytes"), &StreamedClusterMeshInstance3D::set_maximum_load_bytes_per_frame);
	ClassDB::bind_method(D_METHOD("get_maximum_load_bytes_per_frame"), &StreamedClusterMeshInstance3D::get_maximum_load_bytes_per_frame);
	ClassDB::bind_method(D_METHOD("set_maximum_load_tasks_per_frame", "tasks"), &StreamedClusterMeshInstance3D::set_maximum_load_tasks_per_frame);
	ClassDB::bind_method(D_METHOD("get_maximum_load_tasks_per_frame"), &StreamedClusterMeshInstance3D::get_maximum_load_tasks_per_frame);
	ClassDB::bind_method(D_METHOD("set_eye_regions", "regions"), &StreamedClusterMeshInstance3D::set_eye_regions);
	ClassDB::bind_method(D_METHOD("get_eye_regions"), &StreamedClusterMeshInstance3D::get_eye_regions);
	ClassDB::bind_method(D_METHOD("get_streaming_diagnostics"), &StreamedClusterMeshInstance3D::get_streaming_diagnostics);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "streamed_mesh", PROPERTY_HINT_RESOURCE_TYPE, "StreamedClusterMesh"), "set_streamed_mesh", "get_streamed_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streaming_radius", PROPERTY_HINT_RANGE, "0.01,100000,0.1,or_greater,suffix:m"), "set_streaming_radius", "get_streaming_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "maximum_geometric_error", PROPERTY_HINT_RANGE, "0,1000,0.001,or_greater,suffix:m"), "set_maximum_geometric_error", "get_maximum_geometric_error");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "maximum_resident_bytes", PROPERTY_HINT_RANGE, "1,4294967295,1,or_greater,suffix:B"), "set_maximum_resident_bytes", "get_maximum_resident_bytes");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "maximum_load_bytes_per_frame", PROPERTY_HINT_RANGE, "1,4294967295,1,or_greater,suffix:B"), "set_maximum_load_bytes_per_frame", "get_maximum_load_bytes_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "maximum_load_tasks_per_frame", PROPERTY_HINT_RANGE, "1,1024,1,or_greater"), "set_maximum_load_tasks_per_frame", "get_maximum_load_tasks_per_frame");
}
