/**************************************************************************/
/*  atmosphere_sky_clock.cpp                                              */
/**************************************************************************/

#include "atmosphere_sky_clock.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/math/math_funcs.h"

void AtmosphereSkyClock::_sync_cloud_phase_to_time() {
	simulated_time = Math::fposmod(current_time * day_length / 24.0f, day_length);
}

void AtmosphereSkyClock::_apply_state() {
	if (atmosphere.is_valid()) {
		atmosphere->set_time_of_day(current_time);
		atmosphere->set_simulated_time(simulated_time);
	}
}

void AtmosphereSkyClock::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		apply_initial_state();
		return;
	}
	if (p_what != NOTIFICATION_PROCESS) {
		return;
	}
	const bool editor = Engine::get_singleton()->is_editor_hint();
	if ((editor && (!editor_preview_enabled || editor_preview_paused)) || (!editor && paused)) {
		return;
	}
	advance(get_process_delta_time());
}

void AtmosphereSkyClock::set_atmosphere(const Ref<AtmosphereSkyMaterial> &p_atmosphere) {
	atmosphere = p_atmosphere;
	_apply_state();
}

Ref<AtmosphereSkyMaterial> AtmosphereSkyClock::get_atmosphere() const {
	return atmosphere;
}

void AtmosphereSkyClock::set_starting_time(float p_time) {
	starting_time = Math::fposmod(p_time, 24.0f);
}

float AtmosphereSkyClock::get_starting_time() const {
	return starting_time;
}

void AtmosphereSkyClock::set_current_time(float p_time) {
	current_time = Math::fposmod(p_time, 24.0f);
	_sync_cloud_phase_to_time();
	_apply_state();
}

float AtmosphereSkyClock::get_current_time() const {
	return current_time;
}

void AtmosphereSkyClock::set_day_length(float p_length) {
	day_length = MAX(p_length, 0.001f);
	_sync_cloud_phase_to_time();
	_apply_state();
}

float AtmosphereSkyClock::get_day_length() const {
	return day_length;
}

void AtmosphereSkyClock::set_time_scale(float p_scale) {
	time_scale = p_scale;
}

float AtmosphereSkyClock::get_time_scale() const {
	return time_scale;
}

void AtmosphereSkyClock::set_paused(bool p_paused) {
	paused = p_paused;
}

bool AtmosphereSkyClock::is_paused() const {
	return paused;
}

void AtmosphereSkyClock::set_editor_preview_enabled(bool p_enabled) {
	editor_preview_enabled = p_enabled;
}

bool AtmosphereSkyClock::is_editor_preview_enabled() const {
	return editor_preview_enabled;
}

void AtmosphereSkyClock::set_editor_preview_paused(bool p_paused) {
	editor_preview_paused = p_paused;
}

bool AtmosphereSkyClock::is_editor_preview_paused() const {
	return editor_preview_paused;
}

void AtmosphereSkyClock::set_cloud_coverage(float p_coverage) {
	if (atmosphere.is_valid()) {
		atmosphere->set_cloud_coverage(p_coverage);
	}
}

float AtmosphereSkyClock::get_cloud_coverage() const {
	return atmosphere.is_valid() ? atmosphere->get_cloud_coverage() : 0.0f;
}

void AtmosphereSkyClock::set_cloud_density(float p_density) {
	if (atmosphere.is_valid()) {
		atmosphere->set_cloud_density(p_density);
	}
}

float AtmosphereSkyClock::get_cloud_density() const {
	return atmosphere.is_valid() ? atmosphere->get_cloud_density() : 0.0f;
}

void AtmosphereSkyClock::set_cloud_attenuation(float p_attenuation) {
	if (atmosphere.is_valid()) {
		atmosphere->set_cloud_attenuation(p_attenuation);
	}
}

float AtmosphereSkyClock::get_cloud_attenuation() const {
	return atmosphere.is_valid() ? atmosphere->get_cloud_attenuation() : 0.0f;
}

void AtmosphereSkyClock::set_cloud_scale(float p_scale) {
	if (atmosphere.is_valid()) {
		atmosphere->set_cloud_scale(p_scale);
	}
}

float AtmosphereSkyClock::get_cloud_scale() const {
	return atmosphere.is_valid() ? atmosphere->get_cloud_scale() : 0.0f;
}

void AtmosphereSkyClock::set_cloud_wind(const Vector2 &p_wind) {
	if (atmosphere.is_valid()) {
		atmosphere->set_cloud_wind(p_wind);
	}
}

Vector2 AtmosphereSkyClock::get_cloud_wind() const {
	return atmosphere.is_valid() ? atmosphere->get_cloud_wind() : Vector2();
}

void AtmosphereSkyClock::apply_initial_state() {
	if (Engine::get_singleton()->is_editor_hint()) {
		_apply_state();
	} else {
		reset();
	}
}

void AtmosphereSkyClock::advance(float p_seconds) {
	if (paused || (Engine::get_singleton()->is_editor_hint() && editor_preview_paused)) {
		return;
	}
	const float elapsed = p_seconds * time_scale;
	current_time = Math::fposmod(current_time + elapsed * 24.0f / day_length, 24.0f);
	simulated_time = Math::fposmod(simulated_time + elapsed, day_length);
	_apply_state();
}

void AtmosphereSkyClock::scrub(float p_time) {
	set_current_time(p_time);
}

void AtmosphereSkyClock::reset() {
	current_time = starting_time;
	_sync_cloud_phase_to_time();
	_apply_state();
}

void AtmosphereSkyClock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_atmosphere", "atmosphere"), &AtmosphereSkyClock::set_atmosphere);
	ClassDB::bind_method(D_METHOD("get_atmosphere"), &AtmosphereSkyClock::get_atmosphere);
	ClassDB::bind_method(D_METHOD("set_starting_time", "time"), &AtmosphereSkyClock::set_starting_time);
	ClassDB::bind_method(D_METHOD("get_starting_time"), &AtmosphereSkyClock::get_starting_time);
	ClassDB::bind_method(D_METHOD("set_current_time", "time"), &AtmosphereSkyClock::set_current_time);
	ClassDB::bind_method(D_METHOD("get_current_time"), &AtmosphereSkyClock::get_current_time);
	ClassDB::bind_method(D_METHOD("set_day_length", "seconds"), &AtmosphereSkyClock::set_day_length);
	ClassDB::bind_method(D_METHOD("get_day_length"), &AtmosphereSkyClock::get_day_length);
	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &AtmosphereSkyClock::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &AtmosphereSkyClock::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &AtmosphereSkyClock::set_paused);
	ClassDB::bind_method(D_METHOD("is_paused"), &AtmosphereSkyClock::is_paused);
	ClassDB::bind_method(D_METHOD("set_editor_preview_enabled", "enabled"), &AtmosphereSkyClock::set_editor_preview_enabled);
	ClassDB::bind_method(D_METHOD("is_editor_preview_enabled"), &AtmosphereSkyClock::is_editor_preview_enabled);
	ClassDB::bind_method(D_METHOD("set_editor_preview_paused", "paused"), &AtmosphereSkyClock::set_editor_preview_paused);
	ClassDB::bind_method(D_METHOD("is_editor_preview_paused"), &AtmosphereSkyClock::is_editor_preview_paused);
	ClassDB::bind_method(D_METHOD("set_cloud_coverage", "coverage"), &AtmosphereSkyClock::set_cloud_coverage);
	ClassDB::bind_method(D_METHOD("get_cloud_coverage"), &AtmosphereSkyClock::get_cloud_coverage);
	ClassDB::bind_method(D_METHOD("set_cloud_density", "density"), &AtmosphereSkyClock::set_cloud_density);
	ClassDB::bind_method(D_METHOD("get_cloud_density"), &AtmosphereSkyClock::get_cloud_density);
	ClassDB::bind_method(D_METHOD("set_cloud_attenuation", "attenuation"), &AtmosphereSkyClock::set_cloud_attenuation);
	ClassDB::bind_method(D_METHOD("get_cloud_attenuation"), &AtmosphereSkyClock::get_cloud_attenuation);
	ClassDB::bind_method(D_METHOD("set_cloud_scale", "scale"), &AtmosphereSkyClock::set_cloud_scale);
	ClassDB::bind_method(D_METHOD("get_cloud_scale"), &AtmosphereSkyClock::get_cloud_scale);
	ClassDB::bind_method(D_METHOD("set_cloud_wind", "wind"), &AtmosphereSkyClock::set_cloud_wind);
	ClassDB::bind_method(D_METHOD("get_cloud_wind"), &AtmosphereSkyClock::get_cloud_wind);
	ClassDB::bind_method(D_METHOD("apply_initial_state"), &AtmosphereSkyClock::apply_initial_state);
	ClassDB::bind_method(D_METHOD("advance", "seconds"), &AtmosphereSkyClock::advance);
	ClassDB::bind_method(D_METHOD("scrub", "time"), &AtmosphereSkyClock::scrub);
	ClassDB::bind_method(D_METHOD("reset"), &AtmosphereSkyClock::reset);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "atmosphere", PROPERTY_HINT_RESOURCE_TYPE, "AtmosphereSkyMaterial"), "set_atmosphere", "get_atmosphere");
	ADD_GROUP("Clock", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "starting_time", PROPERTY_HINT_RANGE, "0,24,0.01"), "set_starting_time", "get_starting_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "current_time", PROPERTY_HINT_RANGE, "0,24,0.01"), "set_current_time", "get_current_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "day_length", PROPERTY_HINT_RANGE, "0.001,86400,0.001,suffix:s"), "set_day_length", "get_day_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "-100,100,0.01"), "set_time_scale", "get_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "is_paused");
	ADD_GROUP("Cloud Controls", "cloud_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_coverage", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_coverage", "get_cloud_coverage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_density", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_density", "get_cloud_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_attenuation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_attenuation", "get_cloud_attenuation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_scale", PROPERTY_HINT_RANGE, "0.01,10,0.01"), "set_cloud_scale", "get_cloud_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "cloud_wind"), "set_cloud_wind", "get_cloud_wind");
	ADD_GROUP("Editor Preview", "editor_preview_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview_enabled"), "set_editor_preview_enabled", "is_editor_preview_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview_paused"), "set_editor_preview_paused", "is_editor_preview_paused");
}

AtmosphereSkyClock::AtmosphereSkyClock() {
	set_process(true);
}
