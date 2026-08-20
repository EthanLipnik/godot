/**************************************************************************/
/*  path_tracing_scene_compiler.h                                         */
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

#include "path_tracing_scene_packet.h"

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

#include <cstring>

namespace RendererPathTracing {

struct ScenePacketInput {
	GuideContract guide_contract = {};
	Vector<CameraRecord> cameras;
	Vector<InstanceRecord> instances;
	Vector<MaterialRecord> materials;
	Vector<LightRecord> lights;
};

class SceneCompiler {
public:
	static uint64_t hash_payload(const uint8_t *p_data, uint64_t p_size);
	static Error compile(const ScenePacketInput &p_input, PackedByteArray &r_packet, String *r_error = nullptr);
	static Error validate(const PackedByteArray &p_packet, String *r_error = nullptr);
	static Error save_packet(const String &p_path, const PackedByteArray &p_packet, String *r_error = nullptr);
	static Error load_packet(const String &p_path, PackedByteArray &r_packet, String *r_error = nullptr);

	template <typename T>
	static Error read_record(const PackedByteArray &p_packet, uint32_t p_offset, T &r_record) {
		if (p_offset > (uint64_t)p_packet.size() || sizeof(T) > (uint64_t)p_packet.size() - p_offset) {
			return ERR_FILE_CORRUPT;
		}
		memcpy(&r_record, p_packet.ptr() + p_offset, sizeof(T));
		return OK;
	}
};

} // namespace RendererPathTracing
