/**************************************************************************/
/*  baked_visibility_bake_service.h                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/error/error_list.h"
#include "core/io/resource_loader.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

class BakedVisibilityVolume3D;
class Node;

// Installs a front-priority loader only for the direct, headless bake path.
// It substitutes direct-process-scoped texture placeholders while scene resources load.
class BakedVisibilityTexturePayloadSuppressor {
	Ref<ResourceFormatLoader> loader;

public:
	explicit BakedVisibilityTexturePayloadSuppressor(bool p_enabled);
	~BakedVisibilityTexturePayloadSuppressor();

	BakedVisibilityTexturePayloadSuppressor(const BakedVisibilityTexturePayloadSuppressor &) = delete;
	BakedVisibilityTexturePayloadSuppressor &operator=(const BakedVisibilityTexturePayloadSuppressor &) = delete;
};

// Owns baked-visibility serialization and batch transactions without editor
// lifecycle dependencies, so Main and the editor plugin share identical work.
class BakedVisibilityBakeService {
public:
	struct BatchResult {
		int scene_count = 0;
		int failed_scene_count = 0;
	};

	static Error bake_volume(BakedVisibilityVolume3D *p_volume, Node *p_scene_root, bool p_strict, String *r_error);
	static String get_direct_texture_placeholder_type(const String &p_path);
	static Error bake_scene(const String &p_scene_path, bool p_strict, bool p_require_anchor, String *r_error, bool p_suppress_texture_payloads = false);
	static void collect_scene_paths(const String &p_path, Vector<String> &r_paths);
	static Error bake_path(const String &p_path, bool p_strict, bool p_require_anchor, BatchResult *r_result = nullptr, bool p_print_progress = false, String *r_error = nullptr, bool p_suppress_texture_payloads = false);
};
