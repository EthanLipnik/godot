/**************************************************************************/
/*  baked_visibility_volume_3d_editor_plugin.cpp                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "baked_visibility_volume_3d_editor_plugin.h"

#include "baked_visibility_bake_service.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/3d/baked_visibility_volume_3d.h"

void BakedVisibilityVolume3DEditorPlugin::_run_command_line_bake(const String &p_path, bool p_strict, bool p_require_anchor) {
	BakedVisibilityBakeService::BatchResult result;
	String error;
	const Error bake_error = BakedVisibilityBakeService::bake_path(p_path, p_strict, p_require_anchor, &result, false, &error);
	if (bake_error != OK && !error.is_empty()) {
		ERR_PRINT(error);
	}
	OS::get_singleton()->set_exit_code(bake_error == OK ? EXIT_SUCCESS : EXIT_FAILURE);
	// SceneTree::quit bypasses EditorNode's orderly teardown. Route command
	// completion through the same public action as File > Quit instead.
	callable_mp(EditorNode::get_singleton(), &EditorNode::trigger_menu_option).call_deferred(EditorNode::SCENE_QUIT, false);
}

void BakedVisibilityVolume3DEditorPlugin::_defer_command_line_bake(bool p_sources_changed, const String &p_path, bool p_strict, bool p_require_anchor) {
	(void)p_sources_changed;
	callable_mp(this, &BakedVisibilityVolume3DEditorPlugin::_run_command_line_bake).call_deferred(p_path, p_strict, p_require_anchor);
}

void BakedVisibilityVolume3DEditorPlugin::_bake_selected() {
	TypedArray<Node> selected = get_editor_interface()->get_selection()->get_selected_nodes();
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	for (int i = 0; i < selected.size(); i++) {
		if (BakedVisibilityVolume3D *volume = Object::cast_to<BakedVisibilityVolume3D>(selected[i])) {
			String error;
			if (BakedVisibilityBakeService::bake_volume(volume, scene_root, false, &error) != OK) {
				ERR_PRINT(error);
			}
		}
	}
}

BakedVisibilityVolume3DEditorPlugin::BakedVisibilityVolume3DEditorPlugin() {
	add_tool_menu_item("Bake Visibility", callable_mp(this, &BakedVisibilityVolume3DEditorPlugin::_bake_selected));
	String bake_path;
	bool strict = false;
	bool require_anchor = false;
	for (const String &argument : OS::get_singleton()->get_cmdline_user_args()) {
		if (argument.begins_with("--bake-visibility=")) {
			bake_path = argument.trim_prefix("--bake-visibility=");
		} else if (argument == "--bake-visibility-strict") {
			strict = true;
		} else if (argument == "--bake-visibility-require-anchor") {
			require_anchor = true;
		}
	}
	if (!bake_path.is_empty()) {
		EditorFileSystem::get_singleton()->connect(SNAME("sources_changed"), callable_mp(this, &BakedVisibilityVolume3DEditorPlugin::_defer_command_line_bake).bind(bake_path, strict, require_anchor), CONNECT_ONE_SHOT);
	}
}

BakedVisibilityVolume3DEditorPlugin::~BakedVisibilityVolume3DEditorPlugin() {
	remove_tool_menu_item("Bake Visibility");
}
