/**************************************************************************/
/*  baked_visibility_volume_3d.h                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "scene/3d/node_3d.h"
#include "scene/resources/3d/baked_visibility_data_3d.h"

class BakedVisibilityVolume3D : public Node3D {
	GDCLASS(BakedVisibilityVolume3D, Node3D);

	Ref<BakedVisibilityData3D> data;
	bool enabled = true;
	Vector3 size = Vector3(10.0f, 10.0f, 10.0f);
	Vector3 cell_size = Vector3(1.0f, 1.0f, 1.0f);
	NodePath bake_root = NodePath("..");
	uint32_t bake_mask = 0xfffff;
	float transport_distance = -1.0f;
	float lookup_margin = 0.1f;
	int max_cells = 65536;
	int max_work_units_per_cell = 65536;
	int max_blocker_triangles = 131072;
	int max_output_bytes = 512 * 1024 * 1024;
	void _refresh_runtime_registration();
	void _data_changed();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_data(const Ref<BakedVisibilityData3D> &p_data);
	Ref<BakedVisibilityData3D> get_data() const;
	void set_enabled(bool p_enabled);
	bool is_enabled() const;
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;
	void set_cell_size(const Vector3 &p_cell_size);
	Vector3 get_cell_size() const;
	void set_bake_root(const NodePath &p_bake_root);
	NodePath get_bake_root() const;
	void set_bake_mask(uint32_t p_bake_mask);
	uint32_t get_bake_mask() const;
	void set_transport_distance(float p_distance);
	float get_transport_distance() const;
	void set_lookup_margin(float p_margin);
	float get_lookup_margin() const;
	void set_max_cells(int p_max_cells);
	int get_max_cells() const;
	void set_max_work_units_per_cell(int p_max_work_units_per_cell);
	int get_max_work_units_per_cell() const;
	void set_max_blocker_triangles(int p_max_blocker_triangles);
	int get_max_blocker_triangles() const;
	void set_max_output_bytes(int p_max_output_bytes);
	int get_max_output_bytes() const;
	Dictionary get_runtime_stats() const;
};
