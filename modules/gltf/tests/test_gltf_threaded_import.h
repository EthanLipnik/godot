/**************************************************************************/
/*  test_gltf_threaded_import.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            GODOT ENGINE                                */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "../editor/editor_scene_importer_gltf.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "editor/import/3d/resource_importer_scene.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

namespace TestGltf {

static String write_threaded_import_glb(const String &p_name, const String &p_json) {
	const String path = TestUtils::get_temp_path(p_name + ".glb");
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	CHECK(file.is_valid());
	const CharString json = p_json.utf8();
	const uint32_t json_length = (json.length() + 3) & ~uint32_t(3);
	file->store_32(0x46546C67); // glTF
	file->store_32(2);
	file->store_32(12 + 8 + json_length);
	file->store_32(json_length);
	file->store_32(0x4E4F534A); // JSON
	file->store_buffer((const uint8_t *)json.get_data(), json.length());
	for (uint32_t i = json.length(); i < json_length; i++) {
		file->store_8(' ');
	}
	return path;
}

TEST_CASE("[GLTF] Threaded GLB import capability rejects side effects") {
	Ref<EditorSceneFormatImporterGLTF> importer;
	importer.instantiate();
	const HashMap<StringName, Variant> options;

	CHECK(importer->can_import_threaded(write_threaded_import_glb("threaded_plain", R"({"asset":{"version":"2.0"}})"), options));
	CHECK_FALSE(importer->can_import_threaded(write_threaded_import_glb("threaded_images", R"({"asset":{"version":"2.0"},"images":[]})"), options));
	CHECK_FALSE(importer->can_import_threaded(write_threaded_import_glb("threaded_extensions", R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_transform"]})"), options));
}

static HashMap<StringName, Variant> make_threaded_scene_options() {
	HashMap<StringName, Variant> options;
	options["nodes/root_type"] = "";
	options["nodes/root_name"] = "ThreadedImport";
	options["nodes/root_script"] = Variant();
	options["nodes/apply_root_scale"] = true;
	options["nodes/root_scale"] = 1.0;
	options["nodes/import_as_skeleton_bones"] = false;
	options["nodes/use_name_suffixes"] = true;
	options["nodes/use_node_type_suffixes"] = true;
	options["meshes/ensure_tangents"] = true;
	options["meshes/generate_lods"] = false;
	options["meshes/create_shadow_meshes"] = true;
	options["meshes/light_baking"] = 0;
	options["meshes/lightmap_texel_size"] = 0.2;
	options["meshes/force_disable_compression"] = false;
	options["skins/use_named_skins"] = true;
	options["animation/import"] = true;
	options["animation/fps"] = 30;
	options["animation/trimming"] = false;
	options["animation/remove_immutable_tracks"] = true;
	options["import_script/path"] = "";
	options["materials/extract"] = 0;
	options["materials/extract_path"] = "";
	options["_subresources"] = Dictionary();
	options["gltf/naming_version"] = 2;
	options["gltf/embedded_image_handling"] = 1;
	options["gltf/texture_map_mode"] = 1;
	options["gltf/streamed_clusters/enabled"] = false;
	return options;
}

struct ConcurrentSceneImportTask {
	String source_paths[2];
	String save_paths[2];
	Error errors[2] = { FAILED, FAILED };
	uint64_t thread_ids[2] = {};
	Semaphore ready;
	Semaphore start;
	Mutex mutex;
	int active = 0;
	int peak_active = 0;

	void run(uint32_t p_index, void *) {
		{
			MutexLock lock(mutex);
			active++;
			peak_active = MAX(peak_active, active);
			thread_ids[p_index] = Thread::get_caller_id();
		}
		ready.post();
		start.wait();

		Ref<ResourceImporterScene> importer;
		importer.instantiate("PackedScene");
		errors[p_index] = importer->import(ResourceUID::INVALID_ID, source_paths[p_index], save_paths[p_index], make_threaded_scene_options(), nullptr);

		{
			MutexLock lock(mutex);
			active--;
		}
	}
};

TEST_CASE("[GLTF] Plain GLBs import concurrently with deterministic scene outputs") {
	ProjectSettings::get_singleton()->set_setting("editor/import/generate_scene_previews", false);
	Ref<EditorSceneFormatImporterGLTF> gltf_importer;
	gltf_importer.instantiate();
	ResourceImporterScene::add_scene_importer(gltf_importer);
	Ref<ResourceImporterScene> capability_importer;
	capability_importer.instantiate("PackedScene");

	ConcurrentSceneImportTask task;
	const String plain_scene = R"({"asset":{"version":"2.0"},"nodes":[{}],"scenes":[{"nodes":[0]}],"scene":0})";
	task.source_paths[0] = write_threaded_import_glb("threaded_scene_a", plain_scene);
	task.source_paths[1] = write_threaded_import_glb("threaded_scene_b", plain_scene);
	task.save_paths[0] = TestUtils::get_temp_path("threaded_scene_a");
	task.save_paths[1] = TestUtils::get_temp_path("threaded_scene_b");
	CHECK(capability_importer->can_import_threaded(task.source_paths[0], make_threaded_scene_options()));
	HashMap<StringName, Variant> scripted_options = make_threaded_scene_options();
	scripted_options["import_script/path"] = "res://post_import.gd";
	CHECK_FALSE(capability_importer->can_import_threaded(task.source_paths[0], scripted_options));
	HashMap<StringName, Variant> extracted_options = make_threaded_scene_options();
	extracted_options["materials/extract"] = 1;
	CHECK_FALSE(capability_importer->can_import_threaded(task.source_paths[0], extracted_options));
	Ref<ResourceImporterScene> prewarmer;
	prewarmer.instantiate("PackedScene");
	prewarmer->prepare_threaded_import(task.source_paths[0], make_threaded_scene_options());

	WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(&task, &ConcurrentSceneImportTask::run, nullptr, 2, -1, false, "Test parallel glTF imports");
	task.ready.wait();
	task.ready.wait();
	task.start.post();
	task.start.post();
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);

	CHECK(task.peak_active == 2);
	CHECK(task.thread_ids[0] != task.thread_ids[1]);
	CHECK(task.errors[0] == OK);
	CHECK(task.errors[1] == OK);
	Ref<PackedScene> scene_a = ResourceLoader::load(task.save_paths[0] + ".scn", "", ResourceLoader::CACHE_MODE_IGNORE_DEEP);
	Ref<PackedScene> scene_b = ResourceLoader::load(task.save_paths[1] + ".scn", "", ResourceLoader::CACHE_MODE_IGNORE_DEEP);
	REQUIRE(scene_a.is_valid());
	REQUIRE(scene_b.is_valid());
	CHECK(scene_a->get_state()->get_node_count() == scene_b->get_state()->get_node_count());
	Node *root_a = scene_a->instantiate();
	Node *root_b = scene_b->instantiate();
	CHECK(root_a->get_name() == "ThreadedImport");
	CHECK(root_b->get_name() == "ThreadedImport");
	memdelete(root_a);
	memdelete(root_b);
	scene_a.unref();
	scene_b.unref();
	prewarmer.unref();
	capability_importer.unref();

	ResourceImporterScene::remove_scene_importer(gltf_importer);
	gltf_importer.unref();
}

} // namespace TestGltf

#endif // TOOLS_ENABLED
