/**************************************************************************/
/*  path_tracing_editor_plugin.h                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "scene/resources/path_tracing_scene_state.h"

#ifdef METAL_ENABLED
#include "servers/rendering/renderer_rd/effects/metal_fx.h"
#include "servers/rendering/renderer_rd/effects/metal_path_tracing.h"
#endif

class Button;
class Camera3D;
class CheckBox;
class Label;
class OptionButton;
class SpinBox;
class Texture2DRD;
class TextureRect;
class VBoxContainer;

class PathTracingEditorPlugin : public EditorPlugin {
	GDCLASS(PathTracingEditorPlugin, EditorPlugin);

	VBoxContainer *panel = nullptr;
	OptionButton *mode = nullptr;
	OptionButton *resolution = nullptr;
	OptionButton *guide = nullptr;
	SpinBox *bounces = nullptr;
	CheckBox *live_update = nullptr;
	CheckBox *freeze_accumulation = nullptr;
	CheckBox *metalfx = nullptr;
	Label *diagnostics = nullptr;
	TextureRect *preview = nullptr;
	Ref<Texture2DRD> preview_texture;
	RendererPathTracing::PathTracingSceneState scene_state;
#ifdef METAL_ENABLED
	RendererRD::MetalPathTracingBackend backend;
	RendererRD::MFXDenoisedEffect denoised_effect;
	RendererRD::MFXDenoisedContext *denoised_context = nullptr;
#endif
	RID color_target;
	RID reconstructed_target;
	Vector<RID> guide_targets[10];
	uint32_t sample_index = 0;
	Size2i target_size;
	Size2i reconstructed_size;
	double update_accumulator = 0.0;
	bool validation_pending = false;
	uint32_t validation_wait_frames = 0;
	uint32_t validation_stage = 0;
	bool validation_render_scheduled = false;
	uint64_t validation_first_hash = 0;
	uint64_t validation_first_geometry_hash = 0;
	float validation_morph_before = 0.0f;
	float validation_morph_after = 0.0f;
	uint32_t validation_first_specular_pixels = 0;
	uint32_t last_blas_refit = 0;
	bool last_material_fallback = false;
	bool last_render_ok = false;
	uint64_t last_geometry_hash = 0;

	static Camera3D *_find_camera(Node *p_node);
	void _free_targets();
	Error _ensure_targets(const Size2i &p_input_size, const Size2i &p_output_size, bool p_use_metalfx, String *r_error);
	void _render_scene();
	void _reset_history();
	void _update_preview_texture();
	void _mode_changed(int p_mode);
	void _guide_changed(int p_guide);

protected:
	void _notification(int p_what);

public:
	PathTracingEditorPlugin();
	~PathTracingEditorPlugin() override;
};
