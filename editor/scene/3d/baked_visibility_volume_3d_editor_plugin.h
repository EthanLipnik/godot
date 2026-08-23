/**************************************************************************/
/*  baked_visibility_volume_3d_editor_plugin.h                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"

class BakedVisibilityVolume3D;
class Node;

class BakedVisibilityVolume3DEditorPlugin : public EditorPlugin {
	GDCLASS(BakedVisibilityVolume3DEditorPlugin, EditorPlugin);

	void _bake_selected();
	void _defer_command_line_bake(bool p_sources_changed, const String &p_path, bool p_strict, bool p_require_anchor);
	void _run_command_line_bake(const String &p_path, bool p_strict, bool p_require_anchor);

public:
	BakedVisibilityVolume3DEditorPlugin();
	~BakedVisibilityVolume3DEditorPlugin() override;
};
