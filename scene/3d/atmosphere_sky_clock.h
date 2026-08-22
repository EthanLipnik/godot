/**************************************************************************/
/*  atmosphere_sky_clock.h                                                */
/**************************************************************************/

#pragma once

#include "scene/main/node.h"
#include "scene/resources/3d/sky_material.h"

class AtmosphereSkyClock : public Node {
	GDCLASS(AtmosphereSkyClock, Node);

	Ref<AtmosphereSkyMaterial> atmosphere;
	float starting_time = 12.0f;
	float current_time = 12.0f;
	float day_length = 600.0f;
	float time_scale = 1.0f;
	bool paused = false;
	bool editor_preview_enabled = true;
	bool editor_preview_paused = false;
	float simulated_time = 0.0f;

	void _sync_cloud_phase_to_time();
	void _apply_state();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_atmosphere(const Ref<AtmosphereSkyMaterial> &p_atmosphere);
	Ref<AtmosphereSkyMaterial> get_atmosphere() const;
	void set_starting_time(float p_time);
	float get_starting_time() const;
	void set_current_time(float p_time);
	float get_current_time() const;
	void set_day_length(float p_length);
	float get_day_length() const;
	void set_time_scale(float p_scale);
	float get_time_scale() const;
	void set_paused(bool p_paused);
	bool is_paused() const;
	void set_editor_preview_enabled(bool p_enabled);
	bool is_editor_preview_enabled() const;
	void set_editor_preview_paused(bool p_paused);
	bool is_editor_preview_paused() const;
	void set_cloud_coverage(float p_coverage);
	float get_cloud_coverage() const;
	void set_cloud_density(float p_density);
	float get_cloud_density() const;
	void set_cloud_attenuation(float p_attenuation);
	float get_cloud_attenuation() const;
	void set_cloud_scale(float p_scale);
	float get_cloud_scale() const;
	void set_cloud_wind(const Vector2 &p_wind);
	Vector2 get_cloud_wind() const;
	void apply_initial_state();
	void advance(float p_seconds);
	void scrub(float p_time);
	void reset();

	AtmosphereSkyClock();
};
