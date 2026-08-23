/**************************************************************************/
/*  sky_material.cpp                                                      */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "sky_material.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/version.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server.h"

Mutex ProceduralSkyMaterial::shader_mutex;
RID ProceduralSkyMaterial::shader_cache[4];

void ProceduralSkyMaterial::set_sky_top_color(const Color &p_sky_top) {
	sky_top_color = p_sky_top;
	RS::get_singleton()->material_set_param(_get_material(), "sky_top_color", sky_top_color * sky_energy_multiplier);
}

Color ProceduralSkyMaterial::get_sky_top_color() const {
	return sky_top_color;
}

void ProceduralSkyMaterial::set_sky_horizon_color(const Color &p_sky_horizon) {
	sky_horizon_color = p_sky_horizon;
	RS::get_singleton()->material_set_param(_get_material(), "sky_horizon_color", sky_horizon_color * sky_energy_multiplier);
}

Color ProceduralSkyMaterial::get_sky_horizon_color() const {
	return sky_horizon_color;
}

void ProceduralSkyMaterial::set_sky_curve(float p_curve) {
	sky_curve = p_curve;
	// Actual curve passed to shader includes an ad hoc adjustment because the curve used to be
	// in calculated in angles and now uses cosines.
	RS::get_singleton()->material_set_param(_get_material(), "inv_sky_curve", 0.6 / sky_curve);
}

float ProceduralSkyMaterial::get_sky_curve() const {
	return sky_curve;
}

void ProceduralSkyMaterial::set_sky_energy_multiplier(float p_multiplier) {
	sky_energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "sky_top_color", sky_top_color * sky_energy_multiplier);
	RS::get_singleton()->material_set_param(_get_material(), "sky_horizon_color", sky_horizon_color * sky_energy_multiplier);
	RS::get_singleton()->material_set_param(_get_material(), "sky_cover_modulate", Color(sky_cover_modulate.r, sky_cover_modulate.g, sky_cover_modulate.b, sky_cover_modulate.a * sky_energy_multiplier));
}

float ProceduralSkyMaterial::get_sky_energy_multiplier() const {
	return sky_energy_multiplier;
}

void ProceduralSkyMaterial::set_sky_cover(const Ref<Texture2D> &p_sky_cover) {
	sky_cover = p_sky_cover;

	if (p_sky_cover.is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), "sky_cover", p_sky_cover->get_rid());
	} else {
		RS::get_singleton()->material_set_param(_get_material(), "sky_cover", Variant());
	}

	_update_shader(use_debanding, sky_cover.is_valid());

	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

Ref<Texture2D> ProceduralSkyMaterial::get_sky_cover() const {
	return sky_cover;
}

void ProceduralSkyMaterial::set_sky_cover_modulate(const Color &p_sky_cover_modulate) {
	sky_cover_modulate = p_sky_cover_modulate;
	RS::get_singleton()->material_set_param(_get_material(), "sky_cover_modulate", Color(sky_cover_modulate.r, sky_cover_modulate.g, sky_cover_modulate.b, sky_cover_modulate.a * sky_energy_multiplier));
}

Color ProceduralSkyMaterial::get_sky_cover_modulate() const {
	return sky_cover_modulate;
}

void ProceduralSkyMaterial::set_ground_bottom_color(const Color &p_ground_bottom) {
	ground_bottom_color = p_ground_bottom;
	RS::get_singleton()->material_set_param(_get_material(), "ground_bottom_color", ground_bottom_color * ground_energy_multiplier);
}

Color ProceduralSkyMaterial::get_ground_bottom_color() const {
	return ground_bottom_color;
}

void ProceduralSkyMaterial::set_ground_horizon_color(const Color &p_ground_horizon) {
	ground_horizon_color = p_ground_horizon;
	RS::get_singleton()->material_set_param(_get_material(), "ground_horizon_color", ground_horizon_color * ground_energy_multiplier);
}

Color ProceduralSkyMaterial::get_ground_horizon_color() const {
	return ground_horizon_color;
}

void ProceduralSkyMaterial::set_ground_curve(float p_curve) {
	ground_curve = p_curve;
	// Actual curve passed to shader includes an ad hoc adjustment because the curve used to be
	// in calculated in angles and now uses cosines.
	RS::get_singleton()->material_set_param(_get_material(), "inv_ground_curve", 0.6 / ground_curve);
}

float ProceduralSkyMaterial::get_ground_curve() const {
	return ground_curve;
}

void ProceduralSkyMaterial::set_ground_energy_multiplier(float p_multiplier) {
	ground_energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "ground_bottom_color", ground_bottom_color * ground_energy_multiplier);
	RS::get_singleton()->material_set_param(_get_material(), "ground_horizon_color", ground_horizon_color * ground_energy_multiplier);
}

float ProceduralSkyMaterial::get_ground_energy_multiplier() const {
	return ground_energy_multiplier;
}

void ProceduralSkyMaterial::set_sun_angle_max(float p_angle) {
	sun_angle_max = p_angle;
	RS::get_singleton()->material_set_param(_get_material(), "sun_angle_max", Math::cos(Math::deg_to_rad(sun_angle_max)));
}

float ProceduralSkyMaterial::get_sun_angle_max() const {
	return sun_angle_max;
}

void ProceduralSkyMaterial::set_sun_curve(float p_curve) {
	sun_curve = p_curve;
	// Actual curve passed to shader includes an ad hoc adjustment because the curve used to be
	// in calculated in angles and now uses cosines.
	RS::get_singleton()->material_set_param(_get_material(), "inv_sun_curve", 1.6f / Math::pow(sun_curve, 1.4f));
}

float ProceduralSkyMaterial::get_sun_curve() const {
	return sun_curve;
}

void ProceduralSkyMaterial::set_use_debanding(bool p_use_debanding) {
	use_debanding = p_use_debanding;
	_update_shader(use_debanding, sky_cover.is_valid());
	// Only set if shader already compiled
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

bool ProceduralSkyMaterial::get_use_debanding() const {
	return use_debanding;
}

void ProceduralSkyMaterial::set_energy_multiplier(float p_multiplier) {
	global_energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", global_energy_multiplier);
}

float ProceduralSkyMaterial::get_energy_multiplier() const {
	return global_energy_multiplier;
}

Shader::Mode ProceduralSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

// Internal function to grab the current shader RID.
// Must only be called if the shader is initialized.
RID ProceduralSkyMaterial::get_shader_cache() const {
	return shader_cache[int(use_debanding) + (sky_cover.is_valid() ? 2 : 0)];
}

RID ProceduralSkyMaterial::get_rid() const {
	_update_shader(use_debanding, sky_cover.is_valid());
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
		shader_set = true;
	}
	return _get_material();
}

RID ProceduralSkyMaterial::get_shader_rid() const {
	_update_shader(use_debanding, sky_cover.is_valid());
	return get_shader_cache();
}

void ProceduralSkyMaterial::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if ((p_property.name == "sky_luminance" || p_property.name == "ground_luminance") && !GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void ProceduralSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_sky_top_color", "color"), &ProceduralSkyMaterial::set_sky_top_color);
	ClassDB::bind_method(D_METHOD("get_sky_top_color"), &ProceduralSkyMaterial::get_sky_top_color);

	ClassDB::bind_method(D_METHOD("set_sky_horizon_color", "color"), &ProceduralSkyMaterial::set_sky_horizon_color);
	ClassDB::bind_method(D_METHOD("get_sky_horizon_color"), &ProceduralSkyMaterial::get_sky_horizon_color);

	ClassDB::bind_method(D_METHOD("set_sky_curve", "curve"), &ProceduralSkyMaterial::set_sky_curve);
	ClassDB::bind_method(D_METHOD("get_sky_curve"), &ProceduralSkyMaterial::get_sky_curve);

	ClassDB::bind_method(D_METHOD("set_sky_energy_multiplier", "multiplier"), &ProceduralSkyMaterial::set_sky_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_sky_energy_multiplier"), &ProceduralSkyMaterial::get_sky_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_sky_cover", "sky_cover"), &ProceduralSkyMaterial::set_sky_cover);
	ClassDB::bind_method(D_METHOD("get_sky_cover"), &ProceduralSkyMaterial::get_sky_cover);

	ClassDB::bind_method(D_METHOD("set_sky_cover_modulate", "color"), &ProceduralSkyMaterial::set_sky_cover_modulate);
	ClassDB::bind_method(D_METHOD("get_sky_cover_modulate"), &ProceduralSkyMaterial::get_sky_cover_modulate);

	ClassDB::bind_method(D_METHOD("set_ground_bottom_color", "color"), &ProceduralSkyMaterial::set_ground_bottom_color);
	ClassDB::bind_method(D_METHOD("get_ground_bottom_color"), &ProceduralSkyMaterial::get_ground_bottom_color);

	ClassDB::bind_method(D_METHOD("set_ground_horizon_color", "color"), &ProceduralSkyMaterial::set_ground_horizon_color);
	ClassDB::bind_method(D_METHOD("get_ground_horizon_color"), &ProceduralSkyMaterial::get_ground_horizon_color);

	ClassDB::bind_method(D_METHOD("set_ground_curve", "curve"), &ProceduralSkyMaterial::set_ground_curve);
	ClassDB::bind_method(D_METHOD("get_ground_curve"), &ProceduralSkyMaterial::get_ground_curve);

	ClassDB::bind_method(D_METHOD("set_ground_energy_multiplier", "energy"), &ProceduralSkyMaterial::set_ground_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_ground_energy_multiplier"), &ProceduralSkyMaterial::get_ground_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_sun_angle_max", "degrees"), &ProceduralSkyMaterial::set_sun_angle_max);
	ClassDB::bind_method(D_METHOD("get_sun_angle_max"), &ProceduralSkyMaterial::get_sun_angle_max);

	ClassDB::bind_method(D_METHOD("set_sun_curve", "curve"), &ProceduralSkyMaterial::set_sun_curve);
	ClassDB::bind_method(D_METHOD("get_sun_curve"), &ProceduralSkyMaterial::get_sun_curve);

	ClassDB::bind_method(D_METHOD("set_use_debanding", "use_debanding"), &ProceduralSkyMaterial::set_use_debanding);
	ClassDB::bind_method(D_METHOD("get_use_debanding"), &ProceduralSkyMaterial::get_use_debanding);

	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &ProceduralSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &ProceduralSkyMaterial::get_energy_multiplier);

	ADD_GROUP("Sky", "sky_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sky_top_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_sky_top_color", "get_sky_top_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sky_horizon_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_sky_horizon_color", "get_sky_horizon_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_curve", PROPERTY_HINT_EXP_EASING), "set_sky_curve", "get_sky_curve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_energy_multiplier", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_sky_energy_multiplier", "get_sky_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_cover", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_sky_cover", "get_sky_cover");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sky_cover_modulate"), "set_sky_cover_modulate", "get_sky_cover_modulate");

	ADD_GROUP("Ground", "ground_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_bottom_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_bottom_color", "get_ground_bottom_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_horizon_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_horizon_color", "get_ground_horizon_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_curve", PROPERTY_HINT_EXP_EASING), "set_ground_curve", "get_ground_curve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_energy_multiplier", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_ground_energy_multiplier", "get_ground_energy_multiplier");

	ADD_GROUP("Sun", "sun_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_angle_max", PROPERTY_HINT_RANGE, "0,360,0.01,degrees"), "set_sun_angle_max", "get_sun_angle_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_curve", PROPERTY_HINT_EXP_EASING), "set_sun_curve", "get_sun_curve");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_debanding"), "set_use_debanding", "get_use_debanding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
}

void ProceduralSkyMaterial::cleanup_shader() {
	for (int i = 0; i < 4; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void ProceduralSkyMaterial::_update_shader(bool p_use_debanding, bool p_use_sky_cover) {
	MutexLock shader_lock(shader_mutex);
	int index = int(p_use_debanding) + int(p_use_sky_cover) * 2;
	if (shader_cache[index].is_null()) {
		shader_cache[index] = RS::get_singleton()->shader_create();

		// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
		RS::get_singleton()->shader_set_code(shader_cache[index], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s ProceduralSkyMaterial.

shader_type sky;
%s

uniform vec4 sky_top_color : source_color = vec4(0.385, 0.454, 0.55, 1.0);
uniform vec4 sky_horizon_color : source_color = vec4(0.646, 0.656, 0.67, 1.0);
uniform float inv_sky_curve : hint_range(1, 100) = 4.0;
uniform vec4 ground_bottom_color : source_color = vec4(0.2, 0.169, 0.133, 1.0);
uniform vec4 ground_horizon_color : source_color = vec4(0.646, 0.656, 0.67, 1.0);
uniform float inv_ground_curve : hint_range(1, 100) = 30.0;
uniform float sun_angle_max = 0.877;
uniform float inv_sun_curve : hint_range(1, 100) = 22.78;
uniform float exposure : hint_range(0, 128) = 1.0;

uniform sampler2D sky_cover : filter_linear, source_color, hint_default_black;
uniform vec4 sky_cover_modulate : source_color = vec4(1.0, 1.0, 1.0, 1.0);

void sky() {
	float v_angle = clamp(EYEDIR.y, -1.0, 1.0);
	vec3 sky = mix(sky_top_color.rgb, sky_horizon_color.rgb, clamp(pow(1.0 - v_angle, inv_sky_curve), 0.0, 1.0));

	if (LIGHT0_ENABLED) {
		float sun_angle = dot(LIGHT0_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT0_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT0_COLOR * LIGHT0_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT0_COLOR * LIGHT0_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	if (LIGHT1_ENABLED) {
		float sun_angle = dot(LIGHT1_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT1_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT1_COLOR * LIGHT1_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT1_COLOR * LIGHT1_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	if (LIGHT2_ENABLED) {
		float sun_angle = dot(LIGHT2_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT2_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT2_COLOR * LIGHT2_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT2_COLOR * LIGHT2_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	if (LIGHT3_ENABLED) {
		float sun_angle = dot(LIGHT3_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT3_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT3_COLOR * LIGHT3_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT3_COLOR * LIGHT3_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	%s
	%s
	vec3 ground = mix(ground_bottom_color.rgb, ground_horizon_color.rgb, clamp(pow(1.0 + v_angle, inv_ground_curve), 0.0, 1.0));

	COLOR = mix(ground, sky, step(0.0, EYEDIR.y)) * exposure;
}
)",
																		  p_use_debanding ? "render_mode use_debanding;" : "", p_use_sky_cover ? "vec4 sky_cover_texture = texture(sky_cover, SKY_COORDS);" : "", p_use_sky_cover ? "sky += (sky_cover_texture.rgb * sky_cover_modulate.rgb) * sky_cover_texture.a * sky_cover_modulate.a;" : ""));
	}
}

ProceduralSkyMaterial::ProceduralSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_sky_top_color(Color(0.385, 0.454, 0.55));
	set_sky_horizon_color(Color(0.6463, 0.6558, 0.6708));
	set_sky_curve(0.15);
	set_sky_energy_multiplier(1.0);
	set_sky_cover_modulate(Color(1, 1, 1));

	set_ground_bottom_color(Color(0.2, 0.169, 0.133));
	set_ground_horizon_color(Color(0.6463, 0.6558, 0.6708));
	set_ground_curve(0.02);
	set_ground_energy_multiplier(1.0);

	set_sun_angle_max(30.0);
	set_sun_curve(0.15);
	set_use_debanding(true);
	set_energy_multiplier(1.0);
}

ProceduralSkyMaterial::~ProceduralSkyMaterial() {
}

/////////////////////////////////////////
/* PanoramaSkyMaterial */

void PanoramaSkyMaterial::set_panorama(const Ref<Texture2D> &p_panorama) {
	panorama = p_panorama;
	if (p_panorama.is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), "source_panorama", p_panorama->get_rid());
	} else {
		RS::get_singleton()->material_set_param(_get_material(), "source_panorama", Variant());
	}
}

Ref<Texture2D> PanoramaSkyMaterial::get_panorama() const {
	return panorama;
}

void PanoramaSkyMaterial::set_filtering_enabled(bool p_enabled) {
	filter = p_enabled;
	notify_property_list_changed();
	_update_shader(filter);
	// Only set if shader already compiled
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache[int(filter)]);
	}
}

bool PanoramaSkyMaterial::is_filtering_enabled() const {
	return filter;
}

void PanoramaSkyMaterial::set_energy_multiplier(float p_multiplier) {
	energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", energy_multiplier);
}

float PanoramaSkyMaterial::get_energy_multiplier() const {
	return energy_multiplier;
}

Shader::Mode PanoramaSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

RID PanoramaSkyMaterial::get_rid() const {
	_update_shader(filter);
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache[int(filter)]);
		shader_set = true;
	}
	return _get_material();
}

RID PanoramaSkyMaterial::get_shader_rid() const {
	_update_shader(filter);
	return shader_cache[int(filter)];
}

void PanoramaSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_panorama", "texture"), &PanoramaSkyMaterial::set_panorama);
	ClassDB::bind_method(D_METHOD("get_panorama"), &PanoramaSkyMaterial::get_panorama);

	ClassDB::bind_method(D_METHOD("set_filtering_enabled", "enabled"), &PanoramaSkyMaterial::set_filtering_enabled);
	ClassDB::bind_method(D_METHOD("is_filtering_enabled"), &PanoramaSkyMaterial::is_filtering_enabled);

	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &PanoramaSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &PanoramaSkyMaterial::get_energy_multiplier);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "panorama", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_panorama", "get_panorama");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "filter"), "set_filtering_enabled", "is_filtering_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
}

Mutex PanoramaSkyMaterial::shader_mutex;
RID PanoramaSkyMaterial::shader_cache[2];

void PanoramaSkyMaterial::cleanup_shader() {
	for (int i = 0; i < 2; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void PanoramaSkyMaterial::_update_shader(bool p_filter) {
	MutexLock shader_lock(shader_mutex);
	int index = int(p_filter);
	if (shader_cache[index].is_null()) {
		shader_cache[index] = RS::get_singleton()->shader_create();

		// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
		RS::get_singleton()->shader_set_code(shader_cache[index], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s PanoramaSkyMaterial.

shader_type sky;

uniform sampler2D source_panorama : %s, source_color, hint_default_black;
uniform float exposure : hint_range(0, 128) = 1.0;

void sky() {
	COLOR = texture(source_panorama, SKY_COORDS).rgb * exposure;
}
)",
																		  p_filter ? "filter_linear" : "filter_nearest"));
	}
}

PanoramaSkyMaterial::PanoramaSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_energy_multiplier(1.0);
}

PanoramaSkyMaterial::~PanoramaSkyMaterial() {
}

//////////////////////////////////
/* PhysicalSkyMaterial */

void PhysicalSkyMaterial::set_rayleigh_coefficient(float p_rayleigh) {
	rayleigh = p_rayleigh;
	RS::get_singleton()->material_set_param(_get_material(), "rayleigh", rayleigh);
}

float PhysicalSkyMaterial::get_rayleigh_coefficient() const {
	return rayleigh;
}

void PhysicalSkyMaterial::set_rayleigh_color(Color p_rayleigh_color) {
	rayleigh_color = p_rayleigh_color;
	RS::get_singleton()->material_set_param(_get_material(), "rayleigh_color", rayleigh_color);
}

Color PhysicalSkyMaterial::get_rayleigh_color() const {
	return rayleigh_color;
}

void PhysicalSkyMaterial::set_mie_coefficient(float p_mie) {
	mie = p_mie;
	RS::get_singleton()->material_set_param(_get_material(), "mie", mie);
}

float PhysicalSkyMaterial::get_mie_coefficient() const {
	return mie;
}

void PhysicalSkyMaterial::set_mie_eccentricity(float p_eccentricity) {
	mie_eccentricity = p_eccentricity;
	RS::get_singleton()->material_set_param(_get_material(), "mie_eccentricity", mie_eccentricity);
}

float PhysicalSkyMaterial::get_mie_eccentricity() const {
	return mie_eccentricity;
}

void PhysicalSkyMaterial::set_mie_color(Color p_mie_color) {
	mie_color = p_mie_color;
	RS::get_singleton()->material_set_param(_get_material(), "mie_color", mie_color);
}

Color PhysicalSkyMaterial::get_mie_color() const {
	return mie_color;
}

void PhysicalSkyMaterial::set_turbidity(float p_turbidity) {
	turbidity = p_turbidity;
	RS::get_singleton()->material_set_param(_get_material(), "turbidity", turbidity);
}

float PhysicalSkyMaterial::get_turbidity() const {
	return turbidity;
}

void PhysicalSkyMaterial::set_sun_disk_scale(float p_sun_disk_scale) {
	sun_disk_scale = p_sun_disk_scale;
	RS::get_singleton()->material_set_param(_get_material(), "sun_disk_scale", sun_disk_scale);
}

float PhysicalSkyMaterial::get_sun_disk_scale() const {
	return sun_disk_scale;
}

void PhysicalSkyMaterial::set_ground_color(Color p_ground_color) {
	ground_color = p_ground_color;
	RS::get_singleton()->material_set_param(_get_material(), "ground_color", ground_color);
}

Color PhysicalSkyMaterial::get_ground_color() const {
	return ground_color;
}

void PhysicalSkyMaterial::set_energy_multiplier(float p_multiplier) {
	energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", energy_multiplier);
}

float PhysicalSkyMaterial::get_energy_multiplier() const {
	return energy_multiplier;
}

void PhysicalSkyMaterial::set_use_debanding(bool p_use_debanding) {
	use_debanding = p_use_debanding;
	_update_shader(use_debanding, night_sky.is_valid());
	// Only set if shader already compiled
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

bool PhysicalSkyMaterial::get_use_debanding() const {
	return use_debanding;
}

void PhysicalSkyMaterial::set_night_sky(const Ref<Texture2D> &p_night_sky) {
	night_sky = p_night_sky;
	if (p_night_sky.is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), "night_sky", p_night_sky->get_rid());
	} else {
		RS::get_singleton()->material_set_param(_get_material(), "night_sky", Variant());
	}

	_update_shader(use_debanding, night_sky.is_valid());

	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

Ref<Texture2D> PhysicalSkyMaterial::get_night_sky() const {
	return night_sky;
}

Shader::Mode PhysicalSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

// Internal function to grab the current shader RID.
// Must only be called if the shader is initialized.
RID PhysicalSkyMaterial::get_shader_cache() const {
	return shader_cache[int(use_debanding) + (night_sky.is_valid() ? 2 : 0)];
}

RID PhysicalSkyMaterial::get_rid() const {
	_update_shader(use_debanding, night_sky.is_valid());
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
		shader_set = true;
	}
	return _get_material();
}

RID PhysicalSkyMaterial::get_shader_rid() const {
	_update_shader(use_debanding, night_sky.is_valid());
	return get_shader_cache();
}

void PhysicalSkyMaterial::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_property.name == "exposure_value" && !GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

Mutex PhysicalSkyMaterial::shader_mutex;
RID PhysicalSkyMaterial::shader_cache[4];

void PhysicalSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rayleigh_coefficient", "rayleigh"), &PhysicalSkyMaterial::set_rayleigh_coefficient);
	ClassDB::bind_method(D_METHOD("get_rayleigh_coefficient"), &PhysicalSkyMaterial::get_rayleigh_coefficient);

	ClassDB::bind_method(D_METHOD("set_rayleigh_color", "color"), &PhysicalSkyMaterial::set_rayleigh_color);
	ClassDB::bind_method(D_METHOD("get_rayleigh_color"), &PhysicalSkyMaterial::get_rayleigh_color);

	ClassDB::bind_method(D_METHOD("set_mie_coefficient", "mie"), &PhysicalSkyMaterial::set_mie_coefficient);
	ClassDB::bind_method(D_METHOD("get_mie_coefficient"), &PhysicalSkyMaterial::get_mie_coefficient);

	ClassDB::bind_method(D_METHOD("set_mie_eccentricity", "eccentricity"), &PhysicalSkyMaterial::set_mie_eccentricity);
	ClassDB::bind_method(D_METHOD("get_mie_eccentricity"), &PhysicalSkyMaterial::get_mie_eccentricity);

	ClassDB::bind_method(D_METHOD("set_mie_color", "color"), &PhysicalSkyMaterial::set_mie_color);
	ClassDB::bind_method(D_METHOD("get_mie_color"), &PhysicalSkyMaterial::get_mie_color);

	ClassDB::bind_method(D_METHOD("set_turbidity", "turbidity"), &PhysicalSkyMaterial::set_turbidity);
	ClassDB::bind_method(D_METHOD("get_turbidity"), &PhysicalSkyMaterial::get_turbidity);

	ClassDB::bind_method(D_METHOD("set_sun_disk_scale", "scale"), &PhysicalSkyMaterial::set_sun_disk_scale);
	ClassDB::bind_method(D_METHOD("get_sun_disk_scale"), &PhysicalSkyMaterial::get_sun_disk_scale);

	ClassDB::bind_method(D_METHOD("set_ground_color", "color"), &PhysicalSkyMaterial::set_ground_color);
	ClassDB::bind_method(D_METHOD("get_ground_color"), &PhysicalSkyMaterial::get_ground_color);

	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &PhysicalSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &PhysicalSkyMaterial::get_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_use_debanding", "use_debanding"), &PhysicalSkyMaterial::set_use_debanding);
	ClassDB::bind_method(D_METHOD("get_use_debanding"), &PhysicalSkyMaterial::get_use_debanding);

	ClassDB::bind_method(D_METHOD("set_night_sky", "night_sky"), &PhysicalSkyMaterial::set_night_sky);
	ClassDB::bind_method(D_METHOD("get_night_sky"), &PhysicalSkyMaterial::get_night_sky);

	ADD_GROUP("Rayleigh", "rayleigh_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rayleigh_coefficient", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_rayleigh_coefficient", "get_rayleigh_coefficient");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "rayleigh_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_rayleigh_color", "get_rayleigh_color");

	ADD_GROUP("Mie", "mie_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_coefficient", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_mie_coefficient", "get_mie_coefficient");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_eccentricity", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_mie_eccentricity", "get_mie_eccentricity");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mie_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mie_color", "get_mie_color");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbidity", PROPERTY_HINT_RANGE, "0,1000,0.01"), "set_turbidity", "get_turbidity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_disk_scale", PROPERTY_HINT_RANGE, "0,360,0.01"), "set_sun_disk_scale", "get_sun_disk_scale");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_color", "get_ground_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_debanding"), "set_use_debanding", "get_use_debanding");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "night_sky", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_night_sky", "get_night_sky");
}

void PhysicalSkyMaterial::cleanup_shader() {
	for (int i = 0; i < 4; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void PhysicalSkyMaterial::_update_shader(bool p_use_debanding, bool p_use_night_sky) {
	MutexLock shader_lock(shader_mutex);
	int index = int(p_use_debanding) + int(p_use_night_sky) * 2;
	if (shader_cache[index].is_null()) {
		shader_cache[index] = RS::get_singleton()->shader_create();

		// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
		RS::get_singleton()->shader_set_code(shader_cache[index], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s PhysicalSkyMaterial.

shader_type sky;
%s

uniform float rayleigh : hint_range(0, 64) = 2.0;
uniform vec4 rayleigh_color : source_color = vec4(0.3, 0.405, 0.6, 1.0);
uniform float mie : hint_range(0, 1) = 0.005;
uniform float mie_eccentricity : hint_range(-1, 1) = 0.8;
uniform vec4 mie_color : source_color = vec4(0.69, 0.729, 0.812, 1.0);

uniform float turbidity : hint_range(0, 1000) = 10.0;
uniform float sun_disk_scale : hint_range(0, 360) = 1.0;
uniform vec4 ground_color : source_color = vec4(0.1, 0.07, 0.034, 1.0);
uniform float exposure : hint_range(0, 128) = 1.0;

uniform sampler2D night_sky : filter_linear, source_color, hint_default_black;

const vec3 UP = vec3( 0.0, 1.0, 0.0 );

// Optical length at zenith for molecules.
const float rayleigh_zenith_size = 8.4e3;
const float mie_zenith_size = 1.25e3;

float henyey_greenstein(float cos_theta, float g) {
	const float k = 0.0795774715459;
	return k * (1.0 - g * g) / (pow(1.0 + g * g - 2.0 * g * cos_theta, 1.5));
}

void sky() {
	if (LIGHT0_ENABLED) {
		float zenith_angle = clamp( dot(UP, normalize(LIGHT0_DIRECTION)), -1.0, 1.0 );
		float sun_energy = max(0.0, 0.757 * zenith_angle) * LIGHT0_ENERGY;
		float sun_fade = 1.0 - clamp(1.0 - exp(LIGHT0_DIRECTION.y), 0.0, 1.0);

		// Rayleigh coefficients.
		float rayleigh_coefficient = rayleigh - ( 1.0 * ( 1.0 - sun_fade ) );
		vec3 rayleigh_beta = rayleigh_coefficient * rayleigh_color.rgb * 0.0001;
		// mie coefficients from Preetham
		vec3 mie_beta = turbidity * mie * mie_color.rgb * 0.000434;

		// Optical length.
		float zenith = max(0.0, dot(UP, EYEDIR));
		float optical_mass = 1.0 / (zenith + 0.15 * pow(3.885 + 54.5 * zenith, -1.253));
		float rayleigh_scatter = rayleigh_zenith_size * optical_mass;
		float mie_scatter = mie_zenith_size * optical_mass;

		// Light extinction based on thickness of atmosphere.
		vec3 extinction = exp(-(rayleigh_beta * rayleigh_scatter + mie_beta * mie_scatter));

		// In scattering.
		float cos_theta = dot(EYEDIR, normalize(LIGHT0_DIRECTION));

		float rayleigh_phase = (3.0 / (16.0 * PI)) * (1.0 + pow(cos_theta * 0.5 + 0.5, 2.0));
		vec3 betaRTheta = rayleigh_beta * rayleigh_phase;

		float mie_phase = henyey_greenstein(cos_theta, mie_eccentricity);
		vec3 betaMTheta = mie_beta * mie_phase;

		vec3 Lin = pow(sun_energy * ((betaRTheta + betaMTheta) / (rayleigh_beta + mie_beta)) * (1.0 - extinction), vec3(1.5));
		// Hack from https://github.com/mrdoob/three.js/blob/master/examples/jsm/objects/Sky.js
		Lin *= mix(vec3(1.0), pow(sun_energy * ((betaRTheta + betaMTheta) / (rayleigh_beta + mie_beta)) * extinction, vec3(0.5)), clamp(pow(1.0 - zenith_angle, 5.0), 0.0, 1.0));

		// Hack in the ground color.
		Lin  *= mix(ground_color.rgb, vec3(1.0), smoothstep(-0.1, 0.1, dot(UP, EYEDIR)));

		// Solar disk and out-scattering.
		float sunAngularDiameterCos = cos(LIGHT0_SIZE * sun_disk_scale);
		float sunAngularDiameterCos2 = cos(LIGHT0_SIZE * sun_disk_scale * 0.5);
		float sundisk = smoothstep(sunAngularDiameterCos, sunAngularDiameterCos2, cos_theta);
		vec3 L0 = (sun_energy * extinction) * sundisk * LIGHT0_COLOR;
		%s

		vec3 color = Lin + L0;
		COLOR = pow(color, vec3(1.0 / (1.2 + (1.2 * sun_fade))));
		COLOR *= exposure;
	} else {
		// There is no sun, so display night_sky and nothing else.
		%s
		COLOR *= exposure;
	}
}
)",
																		  p_use_debanding ? "render_mode use_debanding;" : "", p_use_night_sky ? "L0 += texture(night_sky, SKY_COORDS).xyz * extinction;" : "", p_use_night_sky ? "COLOR = texture(night_sky, SKY_COORDS).xyz;" : ""));
	}
}

PhysicalSkyMaterial::PhysicalSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_rayleigh_coefficient(2.0);
	set_rayleigh_color(Color(0.3, 0.405, 0.6));
	set_mie_coefficient(0.005);
	set_mie_eccentricity(0.8);
	set_mie_color(Color(0.69, 0.729, 0.812));
	set_turbidity(10.0);
	set_sun_disk_scale(1.0);
	set_ground_color(Color(0.1, 0.07, 0.034));
	set_energy_multiplier(1.0);
	set_use_debanding(true);
}

PhysicalSkyMaterial::~PhysicalSkyMaterial() {
}

//////////////////////////////////////////////////////
/* AtmosphereSkyMaterial */

Mutex AtmosphereSkyMaterial::shader_mutex;
RID AtmosphereSkyMaterial::shader_cache;

void AtmosphereSkyMaterial::_update_shader() {
	MutexLock lock(shader_mutex);
	if (shader_cache.is_valid()) {
		return;
	}
	shader_cache = RS::get_singleton()->shader_create();
	RS::get_singleton()->shader_set_code(shader_cache, R"(
shader_type sky;
uniform vec3 sun_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 moon_direction = vec3(0.0, -1.0, 0.0);
uniform vec3 sun_color : source_color = vec3(1.0, 0.95, 0.8);
uniform vec3 moon_color : source_color = vec3(0.55, 0.65, 1.0);
uniform vec3 dawn_color : source_color = vec3(0.63, 0.28, 0.58);
uniform vec3 dusk_color : source_color = vec3(0.84, 0.31, 0.22);
uniform vec3 dark_night_sky_color : source_color = vec3(0.008, 0.003, 0.018);
uniform vec3 moonlight_color : source_color = vec3(0.64, 0.56, 0.94);
uniform float sun_disk_cos = 0.99999;
uniform float moon_disk_cos = 0.99999;
uniform float sun_visibility = 1.0;
uniform float moon_visibility = 0.0;
uniform float sun_disk_energy = 24.0;
uniform float moon_disk_energy = 5.0;
uniform float turbidity = 2.5;
uniform float scattering_strength = 1.0;
uniform float exposure = 1.0;
uniform float day_brightness = 1.7;
uniform float night_floor = 0.025;
uniform float twilight_duration = 0.45;
uniform float twilight_saturation = 1.0;
uniform float twilight_intensity = 1.0;
uniform float cloud_coverage = 0.25;
uniform float cloud_density = 0.7;
uniform float cloud_scale = 1.5;
uniform vec2 cloud_offset = vec2(0.0);
uniform float cloud_seed = 1.0;
uniform float cloud_attenuation = 0.8;
// Renderer-owned finite solar-lobe contract. These remain material uniforms so
// any Sky shader may opt into the residual pass without class coupling.
uniform vec3 hybrid_solar_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 hybrid_solar_previous_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 hybrid_solar_perpendicular_irradiance = vec3(0.0);
uniform float hybrid_solar_angular_radius = 0.00463;
uniform float hybrid_solar_cloud_transmittance = 1.0;
uniform uint hybrid_solar_enabled = 0u;
uniform uint hybrid_solar_profile_version = 1u;
uniform uint hybrid_solar_partition_version = 1u;
uniform uint hybrid_solar_state_generation = 1u;
uniform uint hybrid_solar_history_epoch = 1u;

float hash12(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7)) + cloud_seed) * 43758.5453);
}
float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), f.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}
float cloud_noise(vec2 p) {
	float n = value_noise(p);
	n += 0.5 * value_noise(p * 2.03 + 17.0);
	n += 0.25 * value_noise(p * 4.07 + 31.0);
	return n / 1.75;
}
float clouds(vec3 direction) {
	if (direction.y <= 0.0) {
		return 0.0;
	}
	if (cloud_coverage <= 0.0) {
		return 0.0;
	}
	if (cloud_coverage >= 1.0) {
		return cloud_density;
	}
	vec2 p = direction.xz / max(direction.y + 0.15, 0.15) + cloud_offset;
	float n = cloud_noise(p * max(cloud_scale, 0.001));
	float threshold = 1.0 - cloud_coverage;
	return smoothstep(threshold - 0.12, threshold + 0.12, n) * cloud_density;
}
void sky() {
	vec3 d = normalize(EYEDIR);
	float horizon = smoothstep(-0.16, 0.42, d.y);
	float elevation = clamp(sun_direction.y, -1.0, 1.0);
	float duration = clamp(twilight_duration, 0.05, 1.0);
	float day = smoothstep(-0.03, 0.45, elevation);
	// Keep the extended shoulder above the horizon, but let astronomical night
	// become decisively dark rather than carrying the sunset hue through midnight.
	float twilight = smoothstep(-0.22, 0.0, elevation) * (1.0 - smoothstep(0.0, duration * 0.62, elevation));
	float dawn = 1.0 - smoothstep(-0.08, 0.08, sun_direction.x);
	vec3 night = dark_night_sky_color + moonlight_color * night_floor * (0.20 + 0.80 * max(d.y, 0.0));
	vec3 daytime_zenith = vec3(0.07, 0.30, 0.78) * scattering_strength * day_brightness;
	vec3 zenith = mix(night, daytime_zenith, day);
	vec3 twilight_color = mix(dusk_color, dawn_color, dawn);
	vec3 twilight_neutral = vec3(dot(twilight_color, vec3(0.2126, 0.7152, 0.0722)));
	twilight_color = mix(twilight_neutral, twilight_color, twilight_saturation) * twilight_intensity;
	vec3 horizon_color = mix(dark_night_sky_color * 1.8, twilight_color, twilight);
	// Preserve the authored dawn/dusk hue while the sun is low; the blue daytime
	// field takes over only after the twilight shoulder has faded.
	horizon_color = mix(horizon_color, vec3(0.66, 0.80, 1.0) * day_brightness / max(turbidity * 0.18, 0.4), day * (1.0 - twilight));
	vec3 color = mix(horizon_color, zenith, horizon);
	float cloud = clouds(d);
	color = mix(color, mix(dark_night_sky_color * 1.7, vec3(0.88, 0.90, 0.95) * day_brightness, day), cloud);
	float sun_dot = dot(d, normalize(hybrid_solar_direction));
	float moon_dot = dot(d, normalize(moon_direction));
	float sun_disk = sun_dot >= cos(hybrid_solar_angular_radius) ? 1.0 : 0.0;
	float moon_disk = smoothstep(moon_disk_cos, min(1.0, moon_disk_cos + 0.0004), moon_dot);
	float moon_cloud = clouds(normalize(moon_direction));
	// Full and residual octmaps are rendered with exactly the same material
	// state. The residual removes only this uniform solid-angle solar lobe;
	// moon and dome remain ordinary Sky radiance.
	color += moon_color * moon_disk_energy * moon_disk * moon_visibility * (1.0 - moon_cloud * cloud_attenuation);
	color *= exposure;
	if (!AT_HYBRID_RESIDUAL_PASS && hybrid_solar_enabled != 0u) {
		float solid_angle_profile = 1.0 / (PI * max(sin(hybrid_solar_angular_radius) * sin(hybrid_solar_angular_radius), 0.00000001));
		color += hybrid_solar_perpendicular_irradiance * solid_angle_profile * sun_disk;
	}
	COLOR = color;
}
)");
}

namespace {

static float atmosphere_fract(float p_value) {
	return p_value - Math::floor(p_value);
}

static float atmosphere_hash12(const Vector2 &p_position, float p_seed) {
	return atmosphere_fract(Math::sin(p_position.dot(Vector2(127.1f, 311.7f)) + p_seed) * 43758.5453f);
}

static float atmosphere_value_noise(const Vector2 &p_position, float p_seed) {
	const Vector2 cell(Math::floor(p_position.x), Math::floor(p_position.y));
	Vector2 fraction = p_position - cell;
	fraction = fraction * fraction * (Vector2(3.0f, 3.0f) - fraction * 2.0f);
	const float a = atmosphere_hash12(cell, p_seed);
	const float b = atmosphere_hash12(cell + Vector2(1.0f, 0.0f), p_seed);
	const float c = atmosphere_hash12(cell + Vector2(0.0f, 1.0f), p_seed);
	const float d = atmosphere_hash12(cell + Vector2(1.0f, 1.0f), p_seed);
	return Math::lerp(Math::lerp(a, b, fraction.x), Math::lerp(c, d, fraction.x), fraction.y);
}

static float atmosphere_cloud_coverage(const Vector3 &p_direction, float p_coverage, float p_density, float p_scale, const Vector2 &p_offset, float p_seed) {
	if (p_direction.y <= 0.0f || p_coverage <= 0.0f) {
		return 0.0f;
	}
	if (p_coverage >= 1.0f) {
		return p_density;
	}
	const Vector2 position(Vector2(p_direction.x, p_direction.z) / MAX(p_direction.y + 0.15f, 0.15f) + p_offset);
	float noise = atmosphere_value_noise(position * MAX(p_scale, 0.001f), p_seed);
	noise += 0.5f * atmosphere_value_noise(position * MAX(p_scale, 0.001f) * 2.03f + Vector2(17.0f, 17.0f), p_seed);
	noise += 0.25f * atmosphere_value_noise(position * MAX(p_scale, 0.001f) * 4.07f + Vector2(31.0f, 31.0f), p_seed);
	noise /= 1.75f;
	return Math::smoothstep(1.0f - p_coverage - 0.12f, 1.0f - p_coverage + 0.12f, noise) * p_density;
}

static void atmosphere_get_twilight_colors(AtmosphereSkyMaterial::TwilightPalette p_palette, const Color &p_custom_dawn, const Color &p_custom_dusk, Color &r_dawn, Color &r_dusk) {
	r_dawn = p_custom_dawn;
	r_dusk = p_custom_dusk;
	switch (p_palette) {
		case AtmosphereSkyMaterial::TWILIGHT_PALETTE_WARM_GOLD:
			r_dawn = Color(0.88f, 0.64f, 0.30f);
			r_dusk = Color(0.95f, 0.47f, 0.18f);
			break;
		case AtmosphereSkyMaterial::TWILIGHT_PALETTE_BLUE:
			r_dawn = Color(0.35f, 0.52f, 0.86f);
			r_dusk = Color(0.22f, 0.34f, 0.72f);
			break;
		case AtmosphereSkyMaterial::TWILIGHT_PALETTE_PURPLE:
			r_dawn = Color(0.53f, 0.35f, 0.72f);
			r_dusk = Color(0.58f, 0.24f, 0.52f);
			break;
		case AtmosphereSkyMaterial::TWILIGHT_PALETTE_RED:
			r_dawn = Color(0.86f, 0.34f, 0.30f);
			r_dusk = Color(0.78f, 0.17f, 0.16f);
			break;
		case AtmosphereSkyMaterial::TWILIGHT_PALETTE_CUSTOM:
			break;
	}
}

} // namespace

void AtmosphereSkyMaterial::_update_state(bool p_advance_solar_history) {
	const float latitude_radians = Math::deg_to_rad(latitude);
	const float declination = Math::deg_to_rad(23.44f) * Math::sin(Math::TAU * float(day_of_year - 81) / 365.0f);
	const float hour_angle = Math::deg_to_rad((time_of_day - 12.0f) * 15.0f + north_offset);
	const float cos_declination = Math::cos(declination);
	const Vector3 next_sun_direction = Vector3(cos_declination * Math::sin(hour_angle), Math::sin(latitude_radians) * Math::sin(declination) + Math::cos(latitude_radians) * cos_declination * Math::cos(hour_angle), Math::cos(latitude_radians) * Math::sin(declination) - Math::sin(latitude_radians) * cos_declination * Math::cos(hour_angle)).normalized();
	if (p_advance_solar_history) {
		previous_sun_direction = solar_state_initialized ? sun_direction : next_sun_direction;
	}
	sun_direction = next_sun_direction;
	solar_state_initialized = true;
	solar_state_generation++;
	solar_partition_generation++;
	// The bounded lunar approximation intentionally keeps the moon opposite the
	// sun; phase/ephemeris refinement belongs to a future renderer contract.
	moon_direction = (-sun_direction).normalized();
	const float solar_elevation = CLAMP(sun_direction.y, -1.0f, 1.0f);
	const float day = Math::smoothstep(-0.03f, 0.45f, solar_elevation);
	// Keep disk radiance independent from the diffuse sky field: an authored
	// energy increase brightens the compact source without bleaching the dome.
	sun_color = Color(1.0f, 0.72f + 0.23f * day, 0.34f + 0.60f * day);
	moon_color = moonlight_color;
	const float cloud_offset_x = cloud_wind.x * simulated_time * cloud_motion_scale;
	const float cloud_offset_y = cloud_wind.y * simulated_time * cloud_motion_scale;
	const Vector2 cloud_offset(cloud_offset_x, cloud_offset_y);
	const float sun_visibility = Math::smoothstep(-0.12f, 0.04f, solar_elevation);
	const float source_cloud = atmosphere_cloud_coverage(sun_direction, cloud_coverage, cloud_density, cloud_scale, cloud_offset, float(cloud_seed));
	sun_cloud_transmittance = CLAMP(1.0f - source_cloud * cloud_attenuation, 0.0f, 1.0f);
	const float angular_radius = Math::deg_to_rad(sun_disk_size * 0.5f);
	const Color perpendicular_irradiance = sun_color * sun_disk_energy * sun_visibility * sun_cloud_transmittance * exposure;
	Color effective_dawn_color;
	Color effective_dusk_color;
	atmosphere_get_twilight_colors(twilight_palette, dawn_color, dusk_color, effective_dawn_color, effective_dusk_color);
	RS::get_singleton()->material_set_param(_get_material(), "sun_direction", sun_direction);
	RS::get_singleton()->material_set_param(_get_material(), "moon_direction", moon_direction);
	RS::get_singleton()->material_set_param(_get_material(), "sun_color", sun_color);
	RS::get_singleton()->material_set_param(_get_material(), "moon_color", moon_color);
	RS::get_singleton()->material_set_param(_get_material(), "dawn_color", effective_dawn_color);
	RS::get_singleton()->material_set_param(_get_material(), "dusk_color", effective_dusk_color);
	RS::get_singleton()->material_set_param(_get_material(), "dark_night_sky_color", dark_night_sky_color);
	RS::get_singleton()->material_set_param(_get_material(), "moonlight_color", moonlight_color);
	RS::get_singleton()->material_set_param(_get_material(), "sun_disk_cos", Math::cos(Math::deg_to_rad(sun_disk_size * 0.5f)));
	RS::get_singleton()->material_set_param(_get_material(), "moon_disk_cos", Math::cos(Math::deg_to_rad(moon_disk_size * 0.5f)));
	RS::get_singleton()->material_set_param(_get_material(), "sun_visibility", sun_visibility);
	RS::get_singleton()->material_set_param(_get_material(), "moon_visibility", 1.0f - Math::smoothstep(-0.04f, 0.14f, solar_elevation));
	RS::get_singleton()->material_set_param(_get_material(), "turbidity", turbidity);
	RS::get_singleton()->material_set_param(_get_material(), "scattering_strength", scattering_strength);
	RS::get_singleton()->material_set_param(_get_material(), "exposure", exposure);
	RS::get_singleton()->material_set_param(_get_material(), "day_brightness", day_brightness);
	RS::get_singleton()->material_set_param(_get_material(), "sun_disk_energy", sun_disk_energy);
	RS::get_singleton()->material_set_param(_get_material(), "moon_disk_energy", moon_disk_energy);
	RS::get_singleton()->material_set_param(_get_material(), "night_floor", moonlit_night_floor);
	RS::get_singleton()->material_set_param(_get_material(), "twilight_duration", twilight_duration);
	RS::get_singleton()->material_set_param(_get_material(), "twilight_saturation", twilight_saturation);
	RS::get_singleton()->material_set_param(_get_material(), "twilight_intensity", twilight_intensity);
	RS::get_singleton()->material_set_param(_get_material(), "cloud_coverage", cloud_coverage);
	RS::get_singleton()->material_set_param(_get_material(), "cloud_density", cloud_density);
	RS::get_singleton()->material_set_param(_get_material(), "cloud_scale", cloud_scale);
	RS::get_singleton()->material_set_param(_get_material(), "cloud_offset", Vector2(cloud_offset_x, cloud_offset_y));
	RS::get_singleton()->material_set_param(_get_material(), "cloud_seed", float(cloud_seed));
	RS::get_singleton()->material_set_param(_get_material(), "cloud_attenuation", cloud_attenuation);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_direction", sun_direction);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_previous_direction", previous_sun_direction);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_perpendicular_irradiance", perpendicular_irradiance);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_angular_radius", angular_radius);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_cloud_transmittance", sun_cloud_transmittance);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_enabled", sun_disk_energy > 0.0f && sun_visibility > 0.0f ? 1 : 0);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_profile_version", 1);
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_partition_version", int64_t(solar_partition_generation));
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_state_generation", int64_t(solar_state_generation));
	RS::get_singleton()->material_set_param(_get_material(), "hybrid_solar_history_epoch", int64_t(solar_history_epoch));
}

void AtmosphereSkyMaterial::set_time_of_day(float p_time) {
	time_of_day = Math::fposmod(p_time, 24.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_time_of_day() const {
	return time_of_day;
}

void AtmosphereSkyMaterial::set_latitude(float p_latitude) {
	latitude = CLAMP(p_latitude, -89.9f, 89.9f);
	_update_state();
}

float AtmosphereSkyMaterial::get_latitude() const {
	return latitude;
}

void AtmosphereSkyMaterial::set_day_of_year(int p_day) {
	day_of_year = CLAMP(p_day, 1, 365);
	_update_state();
}

int AtmosphereSkyMaterial::get_day_of_year() const {
	return day_of_year;
}

void AtmosphereSkyMaterial::set_north_offset(float p_offset) {
	north_offset = p_offset;
	_update_state();
}

float AtmosphereSkyMaterial::get_north_offset() const {
	return north_offset;
}

void AtmosphereSkyMaterial::set_turbidity(float p_value) {
	turbidity = MAX(p_value, 0.1f);
	_update_state();
}

float AtmosphereSkyMaterial::get_turbidity() const {
	return turbidity;
}

void AtmosphereSkyMaterial::set_scattering_strength(float p_strength) {
	scattering_strength = MAX(p_strength, 0.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_scattering_strength() const {
	return scattering_strength;
}

void AtmosphereSkyMaterial::set_exposure(float p_value) {
	exposure = MAX(p_value, 0.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_exposure() const {
	return exposure;
}

void AtmosphereSkyMaterial::set_day_brightness(float p_brightness) {
	day_brightness = CLAMP(p_brightness, 0.0f, 16.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_day_brightness() const {
	return day_brightness;
}

void AtmosphereSkyMaterial::set_sun_disk_size(float p_size) {
	sun_disk_size = CLAMP(p_size, 0.01f, 10.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_sun_disk_size() const {
	return sun_disk_size;
}

void AtmosphereSkyMaterial::set_sun_disk_energy(float p_energy) {
	sun_disk_energy = CLAMP(p_energy, 0.0f, 256.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_sun_disk_energy() const {
	return sun_disk_energy;
}

void AtmosphereSkyMaterial::set_moon_disk_size(float p_size) {
	moon_disk_size = CLAMP(p_size, 0.01f, 10.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_moon_disk_size() const {
	return moon_disk_size;
}

void AtmosphereSkyMaterial::set_moon_disk_energy(float p_energy) {
	moon_disk_energy = CLAMP(p_energy, 0.0f, 256.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_moon_disk_energy() const {
	return moon_disk_energy;
}

void AtmosphereSkyMaterial::set_moonlit_night_floor(float p_floor) {
	moonlit_night_floor = MAX(p_floor, 0.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_moonlit_night_floor() const {
	return moonlit_night_floor;
}

void AtmosphereSkyMaterial::set_twilight_duration(float p_duration) {
	twilight_duration = CLAMP(p_duration, 0.05f, 1.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_twilight_duration() const {
	return twilight_duration;
}

void AtmosphereSkyMaterial::set_twilight_palette(TwilightPalette p_palette) {
	twilight_palette = CLAMP(p_palette, TWILIGHT_PALETTE_CUSTOM, TWILIGHT_PALETTE_RED);
	_update_state();
}

AtmosphereSkyMaterial::TwilightPalette AtmosphereSkyMaterial::get_twilight_palette() const {
	return twilight_palette;
}

void AtmosphereSkyMaterial::set_twilight_saturation(float p_saturation) {
	twilight_saturation = CLAMP(p_saturation, 0.0f, 1.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_twilight_saturation() const {
	return twilight_saturation;
}

void AtmosphereSkyMaterial::set_twilight_intensity(float p_intensity) {
	twilight_intensity = CLAMP(p_intensity, 0.0f, 4.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_twilight_intensity() const {
	return twilight_intensity;
}

void AtmosphereSkyMaterial::set_dawn_color(const Color &p_color) {
	dawn_color = p_color;
	_update_state();
}

Color AtmosphereSkyMaterial::get_dawn_color() const {
	return dawn_color;
}

void AtmosphereSkyMaterial::set_dusk_color(const Color &p_color) {
	dusk_color = p_color;
	_update_state();
}

Color AtmosphereSkyMaterial::get_dusk_color() const {
	return dusk_color;
}

void AtmosphereSkyMaterial::set_dark_night_sky_color(const Color &p_color) {
	dark_night_sky_color = p_color;
	_update_state();
}

Color AtmosphereSkyMaterial::get_dark_night_sky_color() const {
	return dark_night_sky_color;
}

void AtmosphereSkyMaterial::set_moonlight_color(const Color &p_color) {
	moonlight_color = p_color;
	_update_state();
}

Color AtmosphereSkyMaterial::get_moonlight_color() const {
	return moonlight_color;
}

void AtmosphereSkyMaterial::set_cloud_coverage(float p_coverage) {
	cloud_coverage = CLAMP(p_coverage, 0.0f, 1.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_cloud_coverage() const {
	return cloud_coverage;
}

void AtmosphereSkyMaterial::set_cloud_density(float p_density) {
	cloud_density = CLAMP(p_density, 0.0f, 1.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_cloud_density() const {
	return cloud_density;
}

void AtmosphereSkyMaterial::set_cloud_scale(float p_scale) {
	cloud_scale = MAX(p_scale, 0.001f);
	_update_state();
}

float AtmosphereSkyMaterial::get_cloud_scale() const {
	return cloud_scale;
}

void AtmosphereSkyMaterial::set_cloud_motion_scale(float p_scale) {
	cloud_motion_scale = CLAMP(p_scale, 0.0f, 10.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_cloud_motion_scale() const {
	return cloud_motion_scale;
}

void AtmosphereSkyMaterial::set_cloud_wind(const Vector2 &p_wind) {
	cloud_wind = p_wind;
	_update_state();
}

Vector2 AtmosphereSkyMaterial::get_cloud_wind() const {
	return cloud_wind;
}

void AtmosphereSkyMaterial::set_cloud_seed(uint32_t p_seed) {
	cloud_seed = p_seed;
	_update_state();
}

uint32_t AtmosphereSkyMaterial::get_cloud_seed() const {
	return cloud_seed;
}

void AtmosphereSkyMaterial::set_cloud_attenuation(float p_attenuation) {
	cloud_attenuation = CLAMP(p_attenuation, 0.0f, 1.0f);
	_update_state();
}

float AtmosphereSkyMaterial::get_cloud_attenuation() const {
	return cloud_attenuation;
}

void AtmosphereSkyMaterial::set_simulated_time(float p_time) {
	simulated_time = p_time;
	// The clock applies civil time and cloud phase as one update. Preserve the
	// previous solar direction established by the time update when cloud phase
	// follows immediately, rather than collapsing motion to a zero delta.
	_update_state(false);
}

float AtmosphereSkyMaterial::get_simulated_time() const {
	return simulated_time;
}

Vector3 AtmosphereSkyMaterial::get_sun_direction() const {
	return sun_direction;
}

Vector3 AtmosphereSkyMaterial::get_moon_direction() const {
	return moon_direction;
}

Color AtmosphereSkyMaterial::get_sun_color() const {
	return sun_color;
}

Color AtmosphereSkyMaterial::get_moon_color() const {
	return moon_color;
}

Vector3 AtmosphereSkyMaterial::get_previous_sun_direction() const {
	return previous_sun_direction;
}

float AtmosphereSkyMaterial::get_sun_cloud_transmittance() const {
	return sun_cloud_transmittance;
}

Shader::Mode AtmosphereSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

RID AtmosphereSkyMaterial::get_shader_rid() const {
	_update_shader();
	return shader_cache;
}

RID AtmosphereSkyMaterial::get_rid() const {
	_update_shader();
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache);
		shader_set = true;
	}
	return _get_material();
}

void AtmosphereSkyMaterial::cleanup_shader() {
	if (shader_cache.is_valid()) {
		RS::get_singleton()->free_rid(shader_cache);
		shader_cache = RID();
	}
}

void AtmosphereSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_time_of_day", "value"), &AtmosphereSkyMaterial::set_time_of_day);
	ClassDB::bind_method(D_METHOD("get_time_of_day"), &AtmosphereSkyMaterial::get_time_of_day);
	ClassDB::bind_method(D_METHOD("set_latitude", "value"), &AtmosphereSkyMaterial::set_latitude);
	ClassDB::bind_method(D_METHOD("get_latitude"), &AtmosphereSkyMaterial::get_latitude);
	ClassDB::bind_method(D_METHOD("set_day_of_year", "value"), &AtmosphereSkyMaterial::set_day_of_year);
	ClassDB::bind_method(D_METHOD("get_day_of_year"), &AtmosphereSkyMaterial::get_day_of_year);
	ClassDB::bind_method(D_METHOD("set_north_offset", "value"), &AtmosphereSkyMaterial::set_north_offset);
	ClassDB::bind_method(D_METHOD("get_north_offset"), &AtmosphereSkyMaterial::get_north_offset);
	ClassDB::bind_method(D_METHOD("set_turbidity", "value"), &AtmosphereSkyMaterial::set_turbidity);
	ClassDB::bind_method(D_METHOD("get_turbidity"), &AtmosphereSkyMaterial::get_turbidity);
	ClassDB::bind_method(D_METHOD("set_scattering_strength", "value"), &AtmosphereSkyMaterial::set_scattering_strength);
	ClassDB::bind_method(D_METHOD("get_scattering_strength"), &AtmosphereSkyMaterial::get_scattering_strength);
	ClassDB::bind_method(D_METHOD("set_exposure", "value"), &AtmosphereSkyMaterial::set_exposure);
	ClassDB::bind_method(D_METHOD("get_exposure"), &AtmosphereSkyMaterial::get_exposure);
	ClassDB::bind_method(D_METHOD("set_day_brightness", "value"), &AtmosphereSkyMaterial::set_day_brightness);
	ClassDB::bind_method(D_METHOD("get_day_brightness"), &AtmosphereSkyMaterial::get_day_brightness);
	ClassDB::bind_method(D_METHOD("set_sun_disk_size", "value"), &AtmosphereSkyMaterial::set_sun_disk_size);
	ClassDB::bind_method(D_METHOD("get_sun_disk_size"), &AtmosphereSkyMaterial::get_sun_disk_size);
	ClassDB::bind_method(D_METHOD("set_sun_disk_energy", "value"), &AtmosphereSkyMaterial::set_sun_disk_energy);
	ClassDB::bind_method(D_METHOD("get_sun_disk_energy"), &AtmosphereSkyMaterial::get_sun_disk_energy);
	ClassDB::bind_method(D_METHOD("set_moon_disk_size", "value"), &AtmosphereSkyMaterial::set_moon_disk_size);
	ClassDB::bind_method(D_METHOD("get_moon_disk_size"), &AtmosphereSkyMaterial::get_moon_disk_size);
	ClassDB::bind_method(D_METHOD("set_moon_disk_energy", "value"), &AtmosphereSkyMaterial::set_moon_disk_energy);
	ClassDB::bind_method(D_METHOD("get_moon_disk_energy"), &AtmosphereSkyMaterial::get_moon_disk_energy);
	ClassDB::bind_method(D_METHOD("set_moonlit_night_floor", "value"), &AtmosphereSkyMaterial::set_moonlit_night_floor);
	ClassDB::bind_method(D_METHOD("get_moonlit_night_floor"), &AtmosphereSkyMaterial::get_moonlit_night_floor);
	ClassDB::bind_method(D_METHOD("set_twilight_duration", "value"), &AtmosphereSkyMaterial::set_twilight_duration);
	ClassDB::bind_method(D_METHOD("get_twilight_duration"), &AtmosphereSkyMaterial::get_twilight_duration);
	ClassDB::bind_method(D_METHOD("set_twilight_palette", "palette"), &AtmosphereSkyMaterial::set_twilight_palette);
	ClassDB::bind_method(D_METHOD("get_twilight_palette"), &AtmosphereSkyMaterial::get_twilight_palette);
	ClassDB::bind_method(D_METHOD("set_twilight_saturation", "value"), &AtmosphereSkyMaterial::set_twilight_saturation);
	ClassDB::bind_method(D_METHOD("get_twilight_saturation"), &AtmosphereSkyMaterial::get_twilight_saturation);
	ClassDB::bind_method(D_METHOD("set_twilight_intensity", "value"), &AtmosphereSkyMaterial::set_twilight_intensity);
	ClassDB::bind_method(D_METHOD("get_twilight_intensity"), &AtmosphereSkyMaterial::get_twilight_intensity);
	ClassDB::bind_method(D_METHOD("set_dawn_color", "color"), &AtmosphereSkyMaterial::set_dawn_color);
	ClassDB::bind_method(D_METHOD("get_dawn_color"), &AtmosphereSkyMaterial::get_dawn_color);
	ClassDB::bind_method(D_METHOD("set_dusk_color", "color"), &AtmosphereSkyMaterial::set_dusk_color);
	ClassDB::bind_method(D_METHOD("get_dusk_color"), &AtmosphereSkyMaterial::get_dusk_color);
	ClassDB::bind_method(D_METHOD("set_dark_night_sky_color", "color"), &AtmosphereSkyMaterial::set_dark_night_sky_color);
	ClassDB::bind_method(D_METHOD("get_dark_night_sky_color"), &AtmosphereSkyMaterial::get_dark_night_sky_color);
	ClassDB::bind_method(D_METHOD("set_moonlight_color", "color"), &AtmosphereSkyMaterial::set_moonlight_color);
	ClassDB::bind_method(D_METHOD("get_moonlight_color"), &AtmosphereSkyMaterial::get_moonlight_color);
	ClassDB::bind_method(D_METHOD("set_cloud_coverage", "value"), &AtmosphereSkyMaterial::set_cloud_coverage);
	ClassDB::bind_method(D_METHOD("get_cloud_coverage"), &AtmosphereSkyMaterial::get_cloud_coverage);
	ClassDB::bind_method(D_METHOD("set_cloud_density", "value"), &AtmosphereSkyMaterial::set_cloud_density);
	ClassDB::bind_method(D_METHOD("get_cloud_density"), &AtmosphereSkyMaterial::get_cloud_density);
	ClassDB::bind_method(D_METHOD("set_cloud_scale", "value"), &AtmosphereSkyMaterial::set_cloud_scale);
	ClassDB::bind_method(D_METHOD("get_cloud_scale"), &AtmosphereSkyMaterial::get_cloud_scale);
	ClassDB::bind_method(D_METHOD("set_cloud_motion_scale", "value"), &AtmosphereSkyMaterial::set_cloud_motion_scale);
	ClassDB::bind_method(D_METHOD("get_cloud_motion_scale"), &AtmosphereSkyMaterial::get_cloud_motion_scale);
	ClassDB::bind_method(D_METHOD("set_cloud_wind", "value"), &AtmosphereSkyMaterial::set_cloud_wind);
	ClassDB::bind_method(D_METHOD("get_cloud_wind"), &AtmosphereSkyMaterial::get_cloud_wind);
	ClassDB::bind_method(D_METHOD("set_cloud_seed", "value"), &AtmosphereSkyMaterial::set_cloud_seed);
	ClassDB::bind_method(D_METHOD("get_cloud_seed"), &AtmosphereSkyMaterial::get_cloud_seed);
	ClassDB::bind_method(D_METHOD("set_cloud_attenuation", "value"), &AtmosphereSkyMaterial::set_cloud_attenuation);
	ClassDB::bind_method(D_METHOD("get_cloud_attenuation"), &AtmosphereSkyMaterial::get_cloud_attenuation);
	ClassDB::bind_method(D_METHOD("set_simulated_time", "value"), &AtmosphereSkyMaterial::set_simulated_time);
	ClassDB::bind_method(D_METHOD("get_simulated_time"), &AtmosphereSkyMaterial::get_simulated_time);
	ClassDB::bind_method(D_METHOD("get_sun_direction"), &AtmosphereSkyMaterial::get_sun_direction);
	ClassDB::bind_method(D_METHOD("get_moon_direction"), &AtmosphereSkyMaterial::get_moon_direction);
	ClassDB::bind_method(D_METHOD("get_sun_color"), &AtmosphereSkyMaterial::get_sun_color);
	ClassDB::bind_method(D_METHOD("get_moon_color"), &AtmosphereSkyMaterial::get_moon_color);
	ClassDB::bind_method(D_METHOD("get_previous_sun_direction"), &AtmosphereSkyMaterial::get_previous_sun_direction);
	ClassDB::bind_method(D_METHOD("get_sun_cloud_transmittance"), &AtmosphereSkyMaterial::get_sun_cloud_transmittance);
	BIND_ENUM_CONSTANT(TWILIGHT_PALETTE_CUSTOM);
	BIND_ENUM_CONSTANT(TWILIGHT_PALETTE_WARM_GOLD);
	BIND_ENUM_CONSTANT(TWILIGHT_PALETTE_BLUE);
	BIND_ENUM_CONSTANT(TWILIGHT_PALETTE_PURPLE);
	BIND_ENUM_CONSTANT(TWILIGHT_PALETTE_RED);

	ADD_GROUP("Time", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_of_day", PROPERTY_HINT_RANGE, "0,24,0.01"), "set_time_of_day", "get_time_of_day");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "latitude", PROPERTY_HINT_RANGE, "-89.9,89.9,0.1,degrees"), "set_latitude", "get_latitude");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "day_of_year", PROPERTY_HINT_RANGE, "1,365,1"), "set_day_of_year", "get_day_of_year");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "north_offset", PROPERTY_HINT_RANGE, "-180,180,0.1,degrees"), "set_north_offset", "get_north_offset");

	ADD_GROUP("Atmosphere", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbidity", PROPERTY_HINT_RANGE, "0.1,20,0.01"), "set_turbidity", "get_turbidity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_scattering_strength", "get_scattering_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "exposure", PROPERTY_HINT_RANGE, "0,16,0.01"), "set_exposure", "get_exposure");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "day_brightness", PROPERTY_HINT_RANGE, "0,16,0.01"), "set_day_brightness", "get_day_brightness");
	ADD_GROUP("Celestial Disks", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_disk_size", PROPERTY_HINT_RANGE, "0.01,10,0.01,degrees"), "set_sun_disk_size", "get_sun_disk_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_disk_energy", PROPERTY_HINT_RANGE, "0,256,0.1"), "set_sun_disk_energy", "get_sun_disk_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moon_disk_size", PROPERTY_HINT_RANGE, "0.01,10,0.01,degrees"), "set_moon_disk_size", "get_moon_disk_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moon_disk_energy", PROPERTY_HINT_RANGE, "0,256,0.1"), "set_moon_disk_energy", "get_moon_disk_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moonlit_night_floor", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_moonlit_night_floor", "get_moonlit_night_floor");
	ADD_GROUP("Twilight", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "twilight_duration", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_twilight_duration", "get_twilight_duration");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "twilight_palette", PROPERTY_HINT_ENUM, "Custom,Warm Gold,Blue,Purple,Red"), "set_twilight_palette", "get_twilight_palette");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "twilight_saturation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_twilight_saturation", "get_twilight_saturation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "twilight_intensity", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_twilight_intensity", "get_twilight_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dawn_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dawn_color", "get_dawn_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dusk_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dusk_color", "get_dusk_color");
	ADD_GROUP("Night", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dark_night_sky_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dark_night_sky_color", "get_dark_night_sky_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "moonlight_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_moonlight_color", "get_moonlight_color");

	ADD_GROUP("Clouds", "cloud_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_coverage", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_coverage", "get_cloud_coverage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_density", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_density", "get_cloud_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_scale", PROPERTY_HINT_RANGE, "0.01,10,0.01"), "set_cloud_scale", "get_cloud_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_motion_scale", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_cloud_motion_scale", "get_cloud_motion_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "cloud_wind"), "set_cloud_wind", "get_cloud_wind");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cloud_seed", PROPERTY_HINT_RANGE, "0,4294967295,1"), "set_cloud_seed", "get_cloud_seed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_attenuation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_attenuation", "get_cloud_attenuation");
}

AtmosphereSkyMaterial::AtmosphereSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	_update_state();
}

AtmosphereSkyMaterial::~AtmosphereSkyMaterial() {
}
