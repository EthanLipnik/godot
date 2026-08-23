/**************************************************************************/
/*  test_streamed_cluster_mesh.h                                          */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

#include "modules/meshoptimizer/streamed_cluster_mesh.h"
#include "modules/meshoptimizer/streamed_cluster_mesh_instance_3d.h"

#include "servers/rendering/path_tracing/streamed_cluster_runtime.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/packed_scene.h"

namespace TestStreamedClusterMesh {

static void _make_grid(PackedVector3Array &r_vertices, PackedInt32Array &r_indices) {
	constexpr int side = 17;
	for (int z = 0; z < side; z++) {
		for (int x = 0; x < side; x++) {
			r_vertices.push_back(Vector3(x, 0, z));
		}
	}
	for (int z = 0; z < side - 1; z++) {
		for (int x = 0; x < side - 1; x++) {
			const int a = z * side + x;
			const int b = a + 1;
			const int c = a + side;
			const int d = c + 1;
			r_indices.push_back(a);
			r_indices.push_back(c);
			r_indices.push_back(b);
			r_indices.push_back(b);
			r_indices.push_back(c);
			r_indices.push_back(d);
		}
	}
}

static Error _validate_serialized_runtime_manifest(const Ref<StreamedClusterMesh> &p_mesh) {
	Vector<RendererPathTracing::StreamedClusterPageDescriptor> descriptors;
	ERR_FAIL_COND_V(p_mesh.is_null(), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(descriptors.resize(p_mesh->get_page_count()) != OK, ERR_OUT_OF_MEMORY);
	for (int i = 0; i < p_mesh->get_page_count(); i++) {
		const StreamedClusterMesh::Page *page = p_mesh->get_page(i);
		ERR_FAIL_NULL_V(page, ERR_INVALID_DATA);
		RendererPathTracing::StreamedClusterPageDescriptor &descriptor = descriptors.write[i];
		descriptor.stable_id = page->stable_id;
		descriptor.revision = page->revision;
		descriptor.parent_id = page->parent_id;
		for (int64_t child_id : page->child_ids) {
			descriptor.child_ids.push_back(uint64_t(child_id));
		}
		descriptor.bounds = page->bounds;
		descriptor.geometric_error = page->geometric_error;
		descriptor.blob_bytes = page->blob_length;
		descriptor.vertex_bytes = page->vertex_count * 12;
		descriptor.index_bytes = page->index_count * 4;
		descriptor.vertex_count = page->vertex_count;
		descriptor.index_count = page->index_count;
		descriptor.material_index = page->material_index;
		descriptor.lod_level = page->lod_level;
		descriptor.persistent = page->persistent;
	}
	RendererPathTracing::StreamedClusterResidencyManager residency;
	return residency.set_manifest(descriptors);
}

TEST_CASE("[SceneTree][StreamedClusterMesh] Cluster hierarchy and page cache are deterministic and bounded") {
	PackedVector3Array vertices;
	PackedInt32Array indices;
	_make_grid(vertices, indices);
	Ref<StreamedClusterMesh> first;
	first.instantiate();
	CHECK(first->build_from_arrays(vertices, indices, 0, 32, 4) == OK);
	CHECK(first->get_page_count() > first->get_persistent_page_count());
	CHECK(first->get_persistent_page_count() > 0);
	CHECK(first->get_maximum_blob_bytes() < uint64_t(UINT32_MAX));

	Ref<StreamedClusterMesh> second;
	second.instantiate();
	CHECK(second->build_from_arrays(vertices, indices, 0, 32, 4) == OK);
	REQUIRE(second->get_page_count() == first->get_page_count());
	for (int i = 0; i < first->get_page_count(); i++) {
		const StreamedClusterMesh::Page *a = first->get_page(i);
		const StreamedClusterMesh::Page *b = second->get_page(i);
		REQUIRE(a != nullptr);
		REQUIRE(b != nullptr);
		CHECK(a->stable_id == b->stable_id);
		CHECK(a->revision == b->revision);
		CHECK(a->content_hash == b->content_hash);
		CHECK(a->blob_length < uint64_t(UINT32_MAX));
		if (a->persistent) {
			AABB fine_union;
			bool initialized = false;
			for (int64_t child_id_signed : a->child_ids) {
				const int child_index = first->find_page(uint64_t(child_id_signed));
				REQUIRE(child_index >= 0);
				const AABB child_bounds = first->get_page(child_index)->bounds;
				fine_union = initialized ? fine_union.merge(child_bounds) : child_bounds;
				initialized = true;
			}
			CHECK(initialized);
			// The serialized coarse bounds are intentionally outward by a few ULPs.
			CHECK(a->bounds.encloses(fine_union));
		}
	}

	const String manifest_path = OS::get_singleton()->get_cache_path().path_join("streamed_cluster_mesh_fixture.tres");
	const String page_directory = manifest_path.get_basename() + ".pages";
	CHECK(first->save_cache(manifest_path) == OK);
	Error load_error = OK;
	Ref<StreamedClusterMesh> loaded = ResourceLoader::load(manifest_path, "StreamedClusterMesh", ResourceFormatLoader::CACHE_MODE_REPLACE, &load_error);
	REQUIRE(load_error == OK);
	REQUIRE(loaded.is_valid());
	if (loaded.is_null()) {
		return;
	}
	CHECK(loaded->get_format_version() == StreamedClusterMesh::FORMAT_VERSION);
	CHECK(loaded->get_page_count() == first->get_page_count());
	CHECK(_validate_serialized_runtime_manifest(loaded) == OK);
	Error payload_error = OK;
	CHECK_FALSE(loaded->load_page_payload(0, &payload_error).is_empty());
	CHECK(payload_error == OK);
	const StreamedClusterMesh::Page *first_page = loaded->get_page(0);
	REQUIRE(first_page != nullptr);
	Error page_open_error = OK;
	Ref<FileAccess> tampered_page = FileAccess::open(first_page->blob_path, FileAccess::READ_WRITE, &page_open_error);
	REQUIRE(page_open_error == OK);
	REQUIRE(tampered_page.is_valid());
	// The content hash starts at byte 64 in the fixed 72-byte page header.
	tampered_page->seek(64);
	const uint64_t original_hash = tampered_page->get_64();
	tampered_page->seek(64);
	tampered_page->store_64(original_hash ^ 1ull);
	tampered_page.unref();
	payload_error = OK;
	ERR_PRINT_OFF;
	CHECK(loaded->load_page_payload(0, &payload_error).is_empty());
	ERR_PRINT_ON;
	CHECK(payload_error == ERR_FILE_CORRUPT);

	const String scene_path = OS::get_singleton()->get_cache_path().path_join("streamed_cluster_mesh_fixture.tscn");
	Node3D *scene_root = memnew(Node3D);
	scene_root->set_name("StreamedClusterSceneRoot");
	StreamedClusterMeshInstance3D *cluster_instance = memnew(StreamedClusterMeshInstance3D);
	cluster_instance->set_name("StreamedClusterMesh");
	cluster_instance->set_streamed_mesh(loaded);
	scene_root->add_child(cluster_instance);
	cluster_instance->set_owner(scene_root);
	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	CHECK(packed_scene->pack(scene_root) == OK);
	CHECK(ResourceSaver::save(packed_scene, scene_path) == OK);
	memdelete(scene_root);

	Error scene_load_error = OK;
	Ref<PackedScene> reloaded_scene = ResourceLoader::load(scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE, &scene_load_error);
	REQUIRE(scene_load_error == OK);
	REQUIRE(reloaded_scene.is_valid());
	Node *instantiated_scene = reloaded_scene->instantiate();
	REQUIRE(instantiated_scene != nullptr);
	StreamedClusterMeshInstance3D *reloaded_instance = Object::cast_to<StreamedClusterMeshInstance3D>(instantiated_scene->get_node_or_null(NodePath("StreamedClusterMesh")));
	REQUIRE(reloaded_instance != nullptr);
	CHECK(reloaded_instance->get_streamed_mesh().is_valid());
	CHECK(reloaded_instance->get_streamed_mesh()->get_page_count() == loaded->get_page_count());
	memdelete(instantiated_scene);

	const Array page_metadata = loaded->get_pages();
	for (int i = 0; i < page_metadata.size(); i++) {
		const Dictionary page = page_metadata[i];
		DirAccess::remove_absolute(page["blob_path"]);
	}
	DirAccess::remove_absolute(scene_path);
	DirAccess::remove_absolute(page_directory);
	DirAccess::remove_absolute(manifest_path);
}

} // namespace TestStreamedClusterMesh
