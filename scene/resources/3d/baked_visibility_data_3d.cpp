/**************************************************************************/
/*  baked_visibility_data_3d.cpp                                         */
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

#include "baked_visibility_data_3d.h"

#include "core/object/class_db.h"

void BakedVisibilityData3D::_decode_payload() {
	decoded_data = BakedVisibilityData3DData();
	validation_error = String();
	valid = BakedVisibilityCodec::decode(payload, decoded_data, &validation_error) == OK;
}

void BakedVisibilityData3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_payload", "payload"), &BakedVisibilityData3D::set_payload);
	ClassDB::bind_method(D_METHOD("get_payload"), &BakedVisibilityData3D::get_payload);
	ClassDB::bind_method(D_METHOD("is_valid"), &BakedVisibilityData3D::is_valid);
	ClassDB::bind_method(D_METHOD("get_validation_error"), &BakedVisibilityData3D::get_validation_error);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "payload", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_payload", "get_payload");
}

void BakedVisibilityData3D::set_payload(const PackedByteArray &p_payload) {
	payload = p_payload;
	_decode_payload();
	emit_changed();
}

PackedByteArray BakedVisibilityData3D::get_payload() const {
	return payload;
}

Error BakedVisibilityData3D::set_baked_data(const BakedVisibilityData3DData &p_data, String *r_error) {
	PackedByteArray encoded;
	Error err = BakedVisibilityCodec::encode(p_data, encoded, r_error);
	if (err != OK) {
		return err;
	}
	set_payload(encoded);
	return OK;
}

const BakedVisibilityData3DData *BakedVisibilityData3D::get_baked_data() const {
	return valid ? &decoded_data : nullptr;
}

bool BakedVisibilityData3D::is_valid() const {
	return valid;
}

String BakedVisibilityData3D::get_validation_error() const {
	return validation_error;
}
