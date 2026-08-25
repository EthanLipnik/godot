/**************************************************************************/
/*  baked_visibility_bake_service.cpp                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "baked_visibility_bake_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_format_binary.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/pair.h"
#include "scene/3d/baked_visibility_volume_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/placeholder_textures.h"
#include "scene/resources/resource_format_text.h"
#include "servers/rendering/baked_visibility/baked_visibility_baker.h"
#include "servers/rendering/baked_visibility/baked_visibility_checkpoint.h"

namespace {

static String direct_texture_placeholder_type_for_path(const String &p_path) {
	if (p_path.has_extension("ctex")) {
		return "PlaceholderTexture2D";
	}
	if (p_path.has_extension("ctexarray")) {
		return "PlaceholderTexture2DArray";
	}
	if (p_path.has_extension("ccube")) {
		return "PlaceholderCubemap";
	}
	if (p_path.has_extension("ccubearray")) {
		return "PlaceholderCubemapArray";
	}
	if (p_path.has_extension("ctex3d")) {
		return "PlaceholderTexture3D";
	}
	return String();
}

static String direct_compressed_texture_type_for_path(const String &p_path) {
	if (p_path.has_extension("ctex")) {
		return "CompressedTexture2D";
	}
	if (p_path.has_extension("ctexarray")) {
		return "CompressedTexture2DArray";
	}
	if (p_path.has_extension("ccube")) {
		return "CompressedCubemap";
	}
	if (p_path.has_extension("ccubearray")) {
		return "CompressedCubemapArray";
	}
	if (p_path.has_extension("ctex3d")) {
		return "CompressedTexture3D";
	}
	return String();
}

class BakedVisibilityTexturePayloadLoader : public ResourceFormatLoader {
	GDSOFTCLASS(BakedVisibilityTexturePayloadLoader, ResourceFormatLoader);

public:
	virtual bool recognize_path(const String &p_path, const String &p_for_type) const override {
		const String placeholder_type = direct_texture_placeholder_type_for_path(p_path);
		return !placeholder_type.is_empty() && (p_for_type.is_empty() || p_for_type == direct_compressed_texture_type_for_path(p_path) || ClassDB::is_parent_class(placeholder_type, p_for_type));
	}

	virtual String get_resource_type(const String &p_path) const override {
		return direct_texture_placeholder_type_for_path(p_path);
	}

	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override {
		// The scene itself uses CACHE_MODE_IGNORE, but its external resources retain
		// CACHE_MODE_REUSE so serializing the staged scene preserves external paths.
		// This command returns immediately after its batch, so these in-memory entries
		// cannot persist into an editor or game process.
		if (p_cache_mode != CACHE_MODE_REUSE) {
			if (r_error) {
				*r_error = ERR_INVALID_PARAMETER;
			}
			return Ref<Resource>();
		}

		Ref<Resource> placeholder;
		if (p_path.has_extension("ctex")) {
			Ref<PlaceholderTexture2D> texture;
			texture.instantiate();
			placeholder = texture;
		} else if (p_path.has_extension("ctexarray")) {
			Ref<PlaceholderTexture2DArray> texture;
			texture.instantiate();
			placeholder = texture;
		} else if (p_path.has_extension("ccube")) {
			Ref<PlaceholderCubemap> texture;
			texture.instantiate();
			placeholder = texture;
		} else if (p_path.has_extension("ccubearray")) {
			Ref<PlaceholderCubemapArray> texture;
			texture.instantiate();
			placeholder = texture;
		} else if (p_path.has_extension("ctex3d")) {
			Ref<PlaceholderTexture3D> texture;
			texture.instantiate();
			placeholder = texture;
		}
		if (r_error) {
			*r_error = placeholder.is_valid() ? OK : ERR_FILE_UNRECOGNIZED;
		}
		return placeholder;
	}
};

static void collect_volumes(Node *p_node, Vector<BakedVisibilityVolume3D *> &r_volumes) {
	if (BakedVisibilityVolume3D *volume = Object::cast_to<BakedVisibilityVolume3D>(p_node)) {
		r_volumes.push_back(volume);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_volumes(p_node->get_child(i), r_volumes);
	}
}

static bool is_top_level_anchor(const BakedVisibilityVolume3D *p_volume, const Node *p_scene_root) {
	for (const Node *node = p_volume; node && node != p_scene_root; node = node->get_parent()) {
		if (!node->get_scene_file_path().is_empty()) {
			return false;
		}
	}
	return true;
}

static String volume_output_path(const String &p_scene_path, const Node *p_scene_root, const BakedVisibilityVolume3D *p_volume) {
	String suffix = String(p_scene_root->get_path_to(p_volume)).replace("/", "_").replace("..", "up");
	if (suffix.is_empty() || suffix == ".") {
		suffix = "visibility";
	}
	return p_scene_path.get_basename() + "." + suffix + ".bvis";
}

// Main's direct batch path intentionally runs without EditorFileSystem's UID
// scan. Read scene headers directly instead of relying on its global cache.
static ResourceUID::ID read_source_uid(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	if (extension == "tscn") {
		return ResourceFormatLoaderText::singleton ? ResourceFormatLoaderText::singleton->get_resource_uid(p_path) : ResourceUID::INVALID_ID;
	}
	if (extension == "scn") {
		ResourceFormatLoaderBinary loader;
		return loader.get_resource_uid(p_path);
	}
	return ResourceUID::INVALID_ID;
}

static Error atomic_rename(const String &p_temporary_resource_path, const String &p_target_resource_path) {
	return DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(p_temporary_resource_path), ProjectSettings::get_singleton()->globalize_path(p_target_resource_path));
}

static void remove_if_present(const String &p_path) {
	if (FileAccess::exists(p_path)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	}
}

struct StagedReplacement {
	String temporary_path;
	String target_path;
	String backup_path;
	bool had_previous = false;
	bool committed = false;
};

static Error commit_staged_replacement(StagedReplacement &r_replacement) {
	r_replacement.had_previous = FileAccess::exists(r_replacement.target_path);
	remove_if_present(r_replacement.backup_path);
	if (r_replacement.had_previous && atomic_rename(r_replacement.target_path, r_replacement.backup_path) != OK) {
		return ERR_CANT_CREATE;
	}
	if (atomic_rename(r_replacement.temporary_path, r_replacement.target_path) != OK) {
		if (r_replacement.had_previous) {
			atomic_rename(r_replacement.backup_path, r_replacement.target_path);
		}
		return ERR_CANT_CREATE;
	}
	r_replacement.committed = true;
	return OK;
}

static void rollback_staged_replacement(StagedReplacement &r_replacement) {
	if (!r_replacement.committed) {
		return;
	}
	remove_if_present(r_replacement.target_path);
	if (r_replacement.had_previous) {
		atomic_rename(r_replacement.backup_path, r_replacement.target_path);
	}
	r_replacement.committed = false;
}

static void finalize_staged_replacement(const StagedReplacement &p_replacement) {
	if (p_replacement.had_previous) {
		remove_if_present(p_replacement.backup_path);
	}
}

static Error assign_final_resource_uid(const String &p_temporary_path, const String &p_final_path, ResourceUID::ID *r_uid, String *r_error) {
	ResourceUID *registry = ResourceUID::get_singleton();
	if (!registry) {
		if (r_error) {
			*r_error = "Resource UID registry is unavailable.";
		}
		return ERR_UNAVAILABLE;
	}
	ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_final_path);
	if (uid == ResourceUID::INVALID_ID) {
		uid = registry->get_path_id(p_final_path);
	}
	if (uid == ResourceUID::INVALID_ID) {
		uid = registry->create_id_for_path(p_final_path);
	}
	if (uid == ResourceUID::INVALID_ID) {
		if (r_error) {
			*r_error = vformat("Could not allocate a stable resource UID for '%s'.", p_final_path);
		}
		return ERR_CANT_CREATE;
	}
	if (registry->has_id(uid)) {
		registry->set_id(uid, p_final_path);
	} else {
		registry->add_id(uid, p_final_path);
	}
	if (ResourceSaver::set_uid(p_temporary_path, uid) != OK || ResourceLoader::get_resource_uid(p_temporary_path) != uid) {
		if (r_error) {
			*r_error = vformat("Could not assign the final UID to staged resource '%s'.", p_final_path);
		}
		return ERR_CANT_CREATE;
	}
	if (r_uid) {
		*r_uid = uid;
	}
	return OK;
}

static Error restage_source_digest(const Vector<Pair<String, String>> &p_pending_resources, const Vector<BakedVisibilityVolume3D *> &p_volumes, const PackedByteArray &p_source_digest, ResourceUID::ID p_source_uid, String *r_error) {
	if (p_source_digest.size() != 32 || p_pending_resources.size() != p_volumes.size()) {
		if (r_error) {
			*r_error = "Could not compute the canonical staged scene digest.";
		}
		return ERR_INVALID_DATA;
	}
	const String source_identity = String::hex_encode_buffer(p_source_digest.ptr(), p_source_digest.size());
	for (int resource_index = 0; resource_index < p_pending_resources.size(); resource_index++) {
		const Pair<String, String> &pending = p_pending_resources[resource_index];
		Node *bake_root = p_volumes[resource_index]->get_node_or_null(p_volumes[resource_index]->get_bake_root());
		if (!bake_root) {
			if (r_error) {
				*r_error = "Staged baked visibility lost its bake_root.";
			}
			return ERR_DOES_NOT_EXIST;
		}
		Ref<BakedVisibilityData3D> data = ResourceLoader::load(pending.first, "BakedVisibilityData3D", ResourceFormatLoader::CACHE_MODE_IGNORE);
		const BakedVisibilityData3DData *decoded = data.is_valid() ? data->get_baked_data() : nullptr;
		if (!decoded) {
			if (r_error) {
				*r_error = vformat("Could not reload staged baked visibility '%s'.", pending.first);
			}
			return ERR_FILE_CORRUPT;
		}
		BakedVisibilityData3DData staged_data = *decoded;
		staged_data.source_sha256 = p_source_digest;
		staged_data.source_uid = p_source_uid;
		for (BakedVisibilityData3DData::Instance &instance : staged_data.instances) {
			Node *node = bake_root->get_node_or_null(instance.path);
			String identity = source_identity;
			if (instance.kind == BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY) {
				MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(node);
				if (!mesh) {
					if (r_error) {
						*r_error = vformat("Could not resolve staged geometry '%s' for its signature.", instance.path);
					}
					return ERR_DOES_NOT_EXIST;
				}
				identity += ":" + BakedVisibilityBaker::make_geometry_identity(mesh);
			} else if (!Object::cast_to<Light3D>(node)) {
				if (r_error) {
					*r_error = vformat("Could not resolve staged light '%s' for its signature.", instance.path);
				}
				return ERR_DOES_NOT_EXIST;
			}
			instance.signature_sha256 = BakedVisibilityBaker::make_instance_signature(instance.path, instance.kind, instance.local_bounds, instance.flags, identity);
		}
		String data_error;
		data->set_scene_unique_id("BakedVisibilityData3D");
		if (data->set_baked_data(staged_data, &data_error) != OK || ResourceSaver::save(data, pending.first) != OK || assign_final_resource_uid(pending.first, pending.second, nullptr, &data_error) != OK) {
			if (r_error) {
				*r_error = data_error.is_empty() ? vformat("Could not restage baked visibility '%s'.", pending.first) : data_error;
			}
			return ERR_CANT_CREATE;
		}
		Ref<BakedVisibilityData3D> check = ResourceLoader::load(pending.first, "BakedVisibilityData3D", ResourceFormatLoader::CACHE_MODE_IGNORE);
		if (check.is_null() || !check->is_valid() || !check->get_baked_data() || check->get_baked_data()->source_uid != p_source_uid || check->get_baked_data()->source_sha256 != p_source_digest) {
			if (r_error) {
				*r_error = vformat("Canonical staged baked visibility '%s' did not validate.", pending.first);
			}
			return ERR_FILE_CORRUPT;
		}
	}
	return OK;
}

static Error preflight_imported_dependencies(const String &p_path, HashSet<String> &r_visited, String *r_error) {
	if (r_visited.has(p_path)) {
		return OK;
	}
	r_visited.insert(p_path);
	if (p_path.ends_with(".import")) {
		return OK;
	}
	if (FileAccess::exists(p_path + ".import")) {
		ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
		if (!importer) {
			if (r_error) {
				*r_error = "Imported-resource preflight requires the resource importer.";
			}
			return ERR_UNAVAILABLE;
		}
		List<String> imported_paths;
		importer->get_internal_resource_path_list(p_path, &imported_paths);
		if (imported_paths.is_empty()) {
			if (r_error) {
				*r_error = vformat("Imported dependency '%s' has no cached artifact paths; import it before baking visibility.", p_path);
			}
			return ERR_FILE_NOT_FOUND;
		}
		const uint64_t source_time = FileAccess::get_modified_time(p_path);
		const uint64_t import_time = FileAccess::get_modified_time(p_path + ".import");
		for (const String &imported_path : imported_paths) {
			if (!FileAccess::exists(imported_path) || FileAccess::get_modified_time(imported_path) < MAX(source_time, import_time)) {
				if (r_error) {
					*r_error = vformat("Imported dependency '%s' has a missing or stale cached artifact '%s'; import it before baking visibility.", p_path, imported_path);
				}
				return ERR_FILE_NOT_FOUND;
			}
		}
	}
	List<String> dependencies;
	ResourceLoader::get_dependencies(p_path, &dependencies);
	for (const String &dependency : dependencies) {
		if (!dependency.begins_with("res://")) {
			continue;
		}
		Error error = preflight_imported_dependencies(dependency, r_visited, r_error);
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

static Error preflight_imported_dependencies(const String &p_path, String *r_error) {
	HashSet<String> visited;
	return preflight_imported_dependencies(p_path, visited, r_error);
}

static Error bake_volume_internal(BakedVisibilityVolume3D *p_volume, Node *p_scene_root, bool p_strict, String *r_error, bool p_defer_commit, Vector<Pair<String, String>> *r_pending_resources) {
	ERR_FAIL_NULL_V(p_volume, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_scene_root, ERR_INVALID_PARAMETER);
	Node *bake_root = p_volume->get_node_or_null(p_volume->get_bake_root());
	if (!bake_root) {
		if (r_error) {
			*r_error = vformat("Baked visibility volume '%s' has no valid bake_root.", p_volume->get_path());
		}
		return ERR_DOES_NOT_EXIST;
	}
	const Vector3 cell_size = p_volume->get_cell_size();
	if (!Math::is_equal_approx(cell_size.x, cell_size.y) || !Math::is_equal_approx(cell_size.x, cell_size.z)) {
		if (r_error) {
			*r_error = "Baked visibility currently requires uniform x/y/z cell_size.";
		}
		return ERR_INVALID_PARAMETER;
	}
	float transport_distance = p_volume->get_transport_distance();
	if (transport_distance <= 0.0f) {
		transport_distance = float(ProjectSettings::get_singleton()->get_setting("rendering/flux/ray_tracing/transport_culling/max_distance", 64.0f));
	}
	if (!Math::is_finite(transport_distance) || transport_distance <= 0.0f) {
		if (r_error) {
			*r_error = "Baked visibility requires a finite positive transport distance.";
		}
		return ERR_INVALID_PARAMETER;
	}
	BakedVisibilityBakeInput input;
	input.anchor = p_volume;
	input.scene_root = bake_root;
	input.source_path = p_scene_root->get_scene_file_path();
	input.bounds = AABB(-p_volume->get_size() * 0.5f, p_volume->get_size());
	input.requested_cell_size = cell_size.x;
	input.max_cells = p_volume->get_max_cells();
	input.certificate_work_cap = p_volume->get_max_work_units_per_cell();
	input.transport_distance = transport_distance;
	input.bake_mask = p_volume->get_bake_mask();
	input.lookup_margin = p_volume->get_lookup_margin();
	input.max_memory_bytes = uint64_t(p_volume->get_max_memory_bytes());
	input.strict = p_strict;
	const String output_path = volume_output_path(input.source_path, p_scene_root, p_volume);
	const String checkpoint_path = output_path + ".checkpoint";
	BakedVisibilityBakeCheckpoint checkpoint;
	if (FileAccess::exists(checkpoint_path)) {
		String checkpoint_error;
		const Error checkpoint_status = BakedVisibilityBakeCheckpointStore::load(checkpoint_path, checkpoint, &checkpoint_error);
		if (checkpoint_status != OK) {
			if (r_error) {
				*r_error = checkpoint_error.is_empty() ? vformat("Could not load baked visibility checkpoint '%s'.", checkpoint_path) : checkpoint_error;
			}
			return checkpoint_status;
		}
		input.resume_checkpoint = &checkpoint;
	}
	const PackedByteArray source_digest_before = BakedVisibilityBaker::make_source_digest(input.source_path);
	const ResourceUID::ID source_uid_before = read_source_uid(input.source_path);
	if (source_digest_before.size() != 32 || source_uid_before == ResourceUID::INVALID_ID) {
		if (r_error) {
			*r_error = vformat("Could not snapshot baked visibility source '%s'.", input.source_path);
		}
		return ERR_CANT_CREATE;
	}
	BakedVisibilityBaker baker;
	BakedVisibilityBakeOutput output;
	Error error = baker.bake(input, output);
	if (error != OK) {
		if (error == ERR_BUSY && output.checkpoint.settings_sha256.size() == 32) {
			String checkpoint_error;
			if (BakedVisibilityBakeCheckpointStore::save(checkpoint_path, output.checkpoint, &checkpoint_error) != OK) {
				if (r_error) {
					*r_error = checkpoint_error;
				}
				return ERR_CANT_CREATE;
			}
		}
		if (r_error) {
			*r_error = output.error;
		}
		return error;
	}
	if (source_digest_before != BakedVisibilityBaker::make_source_digest(input.source_path) || source_uid_before != read_source_uid(input.source_path)) {
		if (r_error) {
			*r_error = vformat("Baked visibility source '%s' changed while baking; prior output was left intact.", input.source_path);
		}
		return ERR_BUSY;
	}
	BakedVisibilityData3DData baked_data;
	String bake_error;
	if (baker.build_data(input, output, baked_data, &bake_error) != OK) {
		if (r_error) {
			*r_error = bake_error;
		}
		return ERR_CANT_CREATE;
	}
	if (source_digest_before != BakedVisibilityBaker::make_source_digest(input.source_path) || source_uid_before != read_source_uid(input.source_path)) {
		if (r_error) {
			*r_error = vformat("Baked visibility source '%s' changed before staging; prior output was left intact.", input.source_path);
		}
		return ERR_BUSY;
	}
	Ref<BakedVisibilityData3D> data;
	data.instantiate();
	data->set_scene_unique_id("BakedVisibilityData3D");
	if (data->set_baked_data(baked_data, &bake_error) != OK || data->get_payload().size() > p_volume->get_max_output_bytes()) {
		if (r_error) {
			*r_error = bake_error.is_empty() ? "Baked visibility output exceeded max_output_bytes." : bake_error;
		}
		return ERR_CANT_CREATE;
	}
	const String temporary_path = output_path.get_basename() + ".tmp.bvis";
	if (ResourceSaver::save(data, temporary_path) != OK || assign_final_resource_uid(temporary_path, output_path, nullptr, &bake_error) != OK) {
		if (r_error) {
			*r_error = bake_error.is_empty() ? vformat("Could not save temporary baked visibility resource '%s'.", temporary_path) : bake_error;
		}
		return ERR_CANT_CREATE;
	}
	Ref<BakedVisibilityData3D> check = ResourceLoader::load(temporary_path, "BakedVisibilityData3D", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (check.is_null() || !check->is_valid()) {
		if (r_error) {
			*r_error = vformat("Baked visibility validation or atomic replacement failed for '%s'.", output_path);
		}
		return ERR_CANT_CREATE;
	}
	check->set_path(output_path, true);
	if (p_defer_commit) {
		if (!r_pending_resources) {
			return ERR_INVALID_PARAMETER;
		}
		p_volume->set_data(check);
		r_pending_resources->push_back(Pair<String, String>(temporary_path, output_path));
		return OK;
	}
	StagedReplacement replacement;
	replacement.temporary_path = temporary_path;
	replacement.target_path = output_path;
	replacement.backup_path = output_path.get_basename() + ".rollback.bvis";
	if (commit_staged_replacement(replacement) != OK) {
		if (r_error) {
			*r_error = vformat("Baked visibility atomic replacement failed for '%s'.", output_path);
		}
		return ERR_CANT_CREATE;
	}
	Ref<BakedVisibilityData3D> committed = ResourceLoader::load(output_path, "BakedVisibilityData3D", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (committed.is_null() || !committed->is_valid() || ResourceLoader::get_resource_uid(output_path) == ResourceUID::INVALID_ID) {
		rollback_staged_replacement(replacement);
		if (r_error) {
			*r_error = vformat("Committed baked visibility resource '%s' could not be reloaded; prior bake was restored.", output_path);
		}
		return ERR_CANT_CREATE;
	}
	finalize_staged_replacement(replacement);
	if (FileAccess::exists(checkpoint_path)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(checkpoint_path));
	}
	p_volume->set_data(committed);
	return OK;
}

} // namespace

BakedVisibilityTexturePayloadSuppressor::BakedVisibilityTexturePayloadSuppressor(bool p_enabled) {
	if (!p_enabled) {
		return;
	}
	Ref<BakedVisibilityTexturePayloadLoader> texture_payload_loader;
	texture_payload_loader.instantiate();
	loader = texture_payload_loader;
	ResourceLoader::add_resource_format_loader(loader, true);
}

BakedVisibilityTexturePayloadSuppressor::~BakedVisibilityTexturePayloadSuppressor() {
	if (loader.is_valid()) {
		ResourceLoader::remove_resource_format_loader(loader);
	}
}

String BakedVisibilityBakeService::get_direct_texture_placeholder_type(const String &p_path) {
	return direct_texture_placeholder_type_for_path(p_path);
}

Error BakedVisibilityBakeService::bake_volume(BakedVisibilityVolume3D *p_volume, Node *p_scene_root, bool p_strict, String *r_error) {
	return bake_volume_internal(p_volume, p_scene_root, p_strict, r_error, false, nullptr);
}

Error BakedVisibilityBakeService::bake_scene(const String &p_scene_path, bool p_strict, bool p_require_anchor, String *r_error, bool p_suppress_texture_payloads) {
	BakedVisibilityTexturePayloadSuppressor texture_payload_suppressor(p_suppress_texture_payloads);
	Error preflight_error = preflight_imported_dependencies(p_scene_path, r_error);
	if (preflight_error != OK) {
		return preflight_error;
	}
	Ref<PackedScene> packed = ResourceLoader::load(p_scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (packed.is_null()) {
		if (r_error) {
			*r_error = vformat("Could not load scene '%s'.", p_scene_path);
		}
		return ERR_FILE_CANT_OPEN;
	}
	const PackedByteArray source_digest_before = BakedVisibilityBaker::make_source_digest(p_scene_path);
	const ResourceUID::ID source_uid_before = read_source_uid(p_scene_path);
	if (source_digest_before.size() != 32 || source_uid_before == ResourceUID::INVALID_ID) {
		if (r_error) {
			*r_error = vformat("Could not snapshot baked visibility source '%s'.", p_scene_path);
		}
		return ERR_CANT_CREATE;
	}
	Node *root = packed->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
	if (!root) {
		return ERR_CANT_CREATE;
	}
	Vector<BakedVisibilityVolume3D *> volumes;
	collect_volumes(root, volumes);
	if (volumes.is_empty()) {
		memdelete(root);
		if (p_require_anchor && r_error) {
			*r_error = vformat("Scene '%s' has no BakedVisibilityVolume3D anchor.", p_scene_path);
		}
		return p_require_anchor ? ERR_DOES_NOT_EXIST : OK;
	}
	Vector<BakedVisibilityVolume3D *> authored_volumes;
	for (BakedVisibilityVolume3D *volume : volumes) {
		if (is_top_level_anchor(volume, root)) {
			authored_volumes.push_back(volume);
		}
	}
	if (authored_volumes.is_empty()) {
		memdelete(root);
		return OK;
	}
	Vector<Pair<String, String>> pending_resources;
	for (BakedVisibilityVolume3D *volume : authored_volumes) {
		String error;
		if (bake_volume_internal(volume, root, p_strict, &error, true, &pending_resources) != OK) {
			memdelete(root);
			if (r_error) {
				*r_error = error;
			}
			return ERR_CANT_CREATE;
		}
	}
	Ref<PackedScene> updated;
	updated.instantiate();
	if (updated->pack(root) != OK) {
		memdelete(root);
		return ERR_CANT_CREATE;
	}
	const String temporary_path = p_scene_path.get_basename() + ".tmp.tscn";
	if (ResourceSaver::save(updated, temporary_path) != OK || ResourceSaver::set_uid(temporary_path, source_uid_before) != OK || read_source_uid(temporary_path) != source_uid_before || ResourceLoader::load(temporary_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE).is_null()) {
		memdelete(root);
		if (r_error) {
			*r_error = vformat("Could not stage updated scene '%s'.", p_scene_path);
		}
		return ERR_CANT_CREATE;
	}
	const PackedByteArray staged_source_digest = BakedVisibilityBaker::make_source_digest(p_scene_path, temporary_path);
	if (restage_source_digest(pending_resources, authored_volumes, staged_source_digest, source_uid_before, r_error) != OK) {
		memdelete(root);
		remove_if_present(temporary_path);
		return ERR_CANT_CREATE;
	}
	memdelete(root);
	if (source_digest_before != BakedVisibilityBaker::make_source_digest(p_scene_path) || source_uid_before != read_source_uid(p_scene_path)) {
		remove_if_present(temporary_path);
		if (r_error) {
			*r_error = vformat("Baked visibility source '%s' changed while staging; prior output was left intact.", p_scene_path);
		}
		return ERR_BUSY;
	}
	Vector<StagedReplacement> resources;
	resources.reserve(pending_resources.size());
	for (const Pair<String, String> &pending : pending_resources) {
		StagedReplacement replacement;
		replacement.temporary_path = pending.first;
		replacement.target_path = pending.second;
		replacement.backup_path = pending.second.get_basename() + ".rollback.bvis";
		resources.push_back(replacement);
	}
	for (int i = 0; i < resources.size(); i++) {
		if (commit_staged_replacement(resources.write[i]) != OK) {
			for (int rollback = i - 1; rollback >= 0; rollback--) {
				rollback_staged_replacement(resources.write[rollback]);
			}
			if (r_error) {
				*r_error = vformat("Could not commit baked visibility '%s'; prior bake data was restored.", resources[i].target_path);
			}
			return ERR_CANT_CREATE;
		}
	}
	StagedReplacement scene_replacement;
	scene_replacement.temporary_path = temporary_path;
	scene_replacement.target_path = p_scene_path;
	scene_replacement.backup_path = p_scene_path.get_basename() + ".rollback.tscn";
	if (commit_staged_replacement(scene_replacement) != OK) {
		for (int rollback = resources.size() - 1; rollback >= 0; rollback--) {
			rollback_staged_replacement(resources.write[rollback]);
		}
		if (r_error) {
			*r_error = vformat("Could not commit scene '%s'; prior bake data was restored.", p_scene_path);
		}
		return ERR_CANT_CREATE;
	}
	for (const StagedReplacement &replacement : resources) {
		finalize_staged_replacement(replacement);
		const String checkpoint_path = replacement.target_path + ".checkpoint";
		if (FileAccess::exists(checkpoint_path)) {
			DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(checkpoint_path));
		}
	}
	finalize_staged_replacement(scene_replacement);
	return OK;
}

void BakedVisibilityBakeService::collect_scene_paths(const String &p_path, Vector<String> &r_paths) {
	if (p_path.ends_with(".tscn") || p_path.ends_with(".scn")) {
		r_paths.push_back(p_path);
		r_paths.sort();
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (!entry.begins_with(".")) {
			const String child = p_path.path_join(entry);
			if (dir->current_is_dir()) {
				collect_scene_paths(child, r_paths);
			} else if (entry.ends_with(".tscn") || entry.ends_with(".scn")) {
				r_paths.push_back(child);
			}
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
	r_paths.sort();
}

Error BakedVisibilityBakeService::bake_path(const String &p_path, bool p_strict, bool p_require_anchor, BatchResult *r_result, bool p_print_progress, String *r_error, bool p_suppress_texture_payloads) {
	Vector<String> paths;
	collect_scene_paths(p_path, paths);
	paths.sort();
	BatchResult result;
	result.scene_count = paths.size();
	String failures;
	for (const String &path : paths) {
		if (p_print_progress) {
			print_line(vformat("Baked visibility: begin scene=%s", path));
		}
		String error;
		const Error bake_error = bake_scene(path, p_strict, p_require_anchor, &error, p_suppress_texture_payloads);
		if (p_print_progress) {
			print_line(vformat("Baked visibility: end scene=%s status=%s", path, bake_error == OK ? "ok" : "failed"));
		}
		if (bake_error != OK) {
			result.failed_scene_count++;
			failures += (failures.is_empty() ? "" : "\n") + error;
		}
	}
	if (paths.is_empty()) {
		result.failed_scene_count = 1;
		failures = vformat("No scenes found under '%s'.", p_path);
	}
	if (r_result) {
		*r_result = result;
	}
	const bool failed = result.failed_scene_count != 0;
	print_line(vformat("Baked visibility: scenes=%d status=%s", result.scene_count, failed ? "failed" : "ok"));
	if (r_error) {
		*r_error = failures;
	}
	if (failed) {
		return ERR_CANT_CREATE;
	}
	if (ResourceUID *registry = ResourceUID::get_singleton()) {
		if (registry->update_cache() != OK) {
			if (r_error) {
				*r_error = "Baked visibility completed, but the resource UID cache could not be updated.";
			}
			return ERR_CANT_CREATE;
		}
	}
	return OK;
}
