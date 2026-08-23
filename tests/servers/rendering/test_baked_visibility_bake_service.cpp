/**************************************************************************/
/*  test_baked_visibility_bake_service.cpp                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_baked_visibility_bake_service)

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "editor/scene/3d/baked_visibility_bake_service.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

namespace TestBakedVisibilityBakeService {

static void remove_test_directory(const String &p_root) {
	DirAccess::remove_absolute(p_root.path_join("alpha.tscn"));
	DirAccess::remove_absolute(p_root.path_join("zeta.tscn"));
	DirAccess::remove_absolute(p_root.path_join("ignore.txt"));
	DirAccess::remove_absolute(p_root.path_join("nested/first.scn"));
	DirAccess::remove_absolute(p_root.path_join("nested"));
	DirAccess::remove_absolute(p_root);
}

static bool write_empty_file(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_string("test");
	return true;
}

static void remove_resource_file(const String &p_path) {
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
}

TEST_CASE("[Rendering][BakedVisibility] bake service exposes a lifecycle-free deterministic scene selector") {
	using BakePathSignature = Error (*)(const String &, bool, bool, BakedVisibilityBakeService::BatchResult *, bool, String *, bool);
	const BakePathSignature bake_path = &BakedVisibilityBakeService::bake_path;
	CHECK(bake_path != nullptr);

	const String root = OS::get_singleton()->get_cache_path().path_join("test_baked_visibility_bake_service");
	remove_test_directory(root);
	REQUIRE_EQ(DirAccess::make_dir_recursive_absolute(root.path_join("nested")), OK);
	const bool files_written = write_empty_file(root.path_join("zeta.tscn")) &&
			write_empty_file(root.path_join("alpha.tscn")) &&
			write_empty_file(root.path_join("nested/first.scn")) &&
			write_empty_file(root.path_join("ignore.txt"));
	REQUIRE(files_written);
	if (!files_written) {
		remove_test_directory(root);
		return;
	}

	Vector<String> paths;
	BakedVisibilityBakeService::collect_scene_paths(root, paths);
	CHECK_EQ(paths.size(), 3);
	if (paths.size() != 3) {
		remove_test_directory(root);
		return;
	}
	CHECK_EQ(paths[0], root.path_join("alpha.tscn"));
	CHECK_EQ(paths[1], root.path_join("nested/first.scn"));
	CHECK_EQ(paths[2], root.path_join("zeta.tscn"));
	remove_test_directory(root);
}

TEST_CASE("[Rendering][BakedVisibility] direct baking suppresses only imported texture payloads") {
	struct TextureCase {
		const char *path;
		const char *compressed_type;
		const char *placeholder_type;
	};
	const TextureCase cases[] = {
		{ "res://baked_visibility_texture.ctex", "CompressedTexture2D", "PlaceholderTexture2D" },
		{ "res://baked_visibility_texture.ctexarray", "CompressedTexture2DArray", "PlaceholderTexture2DArray" },
		{ "res://baked_visibility_texture.ccube", "CompressedCubemap", "PlaceholderCubemap" },
		{ "res://baked_visibility_texture.ccubearray", "CompressedCubemapArray", "PlaceholderCubemapArray" },
		{ "res://baked_visibility_texture.ctex3d", "CompressedTexture3D", "PlaceholderTexture3D" },
	};

	for (const TextureCase &texture_case : cases) {
		const String path = texture_case.path;
		CHECK_EQ(BakedVisibilityBakeService::get_direct_texture_placeholder_type(path), texture_case.placeholder_type);
		CHECK_EQ(ResourceLoader::get_resource_type(path), texture_case.compressed_type);
		Ref<Resource> texture;
		{
			BakedVisibilityTexturePayloadSuppressor suppressor(true);
			CHECK_EQ(ResourceLoader::get_resource_type(path), texture_case.placeholder_type);
			texture = ResourceLoader::load(path, texture_case.compressed_type, ResourceFormatLoader::CACHE_MODE_REUSE);
			CHECK(texture.is_valid());
			if (texture.is_null()) {
				continue;
			}
			CHECK_EQ(texture->get_class(), texture_case.placeholder_type);
			CHECK(ResourceCache::has(path));
		}
		CHECK_EQ(ResourceLoader::get_resource_type(path), texture_case.compressed_type);
		// The real direct process exits after the batch. Clear only this isolated
		// test resource after loader removal; production keeps its external paths
		// through scene staging and never continues into an editor session.
		texture->set_path(String());
		CHECK_FALSE(ResourceCache::has(path));
	}

	const Error failure = []() {
		BakedVisibilityTexturePayloadSuppressor suppressor(true);
		return ERR_CANT_OPEN;
	}();
	CHECK_EQ(failure, ERR_CANT_OPEN);
	CHECK_EQ(ResourceLoader::get_resource_type("res://baked_visibility_failure_cleanup.ctex"), "CompressedTexture2D");
}

TEST_CASE("[Rendering][BakedVisibility] direct baking stages texture placeholders as their original external resource") {
	const String source_path = "user://baked_visibility_texture_payload_source.tscn";
	const String staged_path = "user://baked_visibility_texture_payload_stage.tscn";
	const String texture_path = "user://baked_visibility_texture_payload.ctex";
	remove_resource_file(source_path);
	remove_resource_file(staged_path);
	Ref<FileAccess> source = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source.is_valid());
	source->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"Texture2D\" path=\"%s\" id=\"1_texture\"]\n\n[node name=\"Root\" type=\"Sprite2D\"]\ntexture = ExtResource(\"1_texture\")\n", texture_path));
	source.unref();

	Node *root = nullptr;
	{
		BakedVisibilityTexturePayloadSuppressor suppressor(true);
		Ref<PackedScene> input = ResourceLoader::load(source_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
		REQUIRE(input.is_valid());
		root = input->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
		REQUIRE(root != nullptr);
		Ref<PackedScene> staged;
		staged.instantiate();
		REQUIRE_EQ(staged->pack(root), OK);
		REQUIRE_EQ(ResourceSaver::save(staged, staged_path), OK);
	}
	memdelete(root);

	Ref<FileAccess> staged_file = FileAccess::open(staged_path, FileAccess::READ);
	REQUIRE(staged_file.is_valid());
	const String staged_text = staged_file->get_as_text();
	CHECK(staged_text.contains("type=\"Texture2D\""));
	CHECK(staged_text.contains(vformat("path=\"%s\"", texture_path)));
	CHECK_FALSE(staged_text.contains("PlaceholderTexture"));

	Ref<Resource> texture = ResourceCache::get_ref(texture_path);
	if (texture.is_valid()) {
		texture->set_path(String());
	}
	CHECK_FALSE(ResourceCache::has(texture_path));
	remove_resource_file(source_path);
	remove_resource_file(staged_path);
}

} // namespace TestBakedVisibilityBakeService
