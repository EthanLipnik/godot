/**************************************************************************/
/*  sky_material.h                                                        */
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

#pragma once

#include "core/templates/rid.h"
#include "scene/resources/material.h"

class ProceduralSkyMaterial : public Material {
	GDCLASS(ProceduralSkyMaterial, Material);

private:
	Color sky_top_color;
	Color sky_horizon_color;
	float sky_curve = 0.0f;
	float sky_energy_multiplier = 0.0f;
	Ref<Texture2D> sky_cover;
	Color sky_cover_modulate;

	Color ground_bottom_color;
	Color ground_horizon_color;
	float ground_curve = 0.0f;
	float ground_energy_multiplier = 0.0f;

	float sun_angle_max = 0.0f;
	float sun_curve = 0.0f;
	bool use_debanding = true;
	float global_energy_multiplier = 1.0f;

	static Mutex shader_mutex;
	static RID shader_cache[4];
	static void _update_shader(bool p_use_debanding, bool p_use_sky_cover);
	mutable bool shader_set = false;

	RID get_shader_cache() const;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &property) const;

public:
	void set_sky_top_color(const Color &p_sky_top);
	Color get_sky_top_color() const;

	void set_sky_horizon_color(const Color &p_sky_horizon);
	Color get_sky_horizon_color() const;

	void set_sky_curve(float p_curve);
	float get_sky_curve() const;

	void set_sky_energy_multiplier(float p_multiplier);
	float get_sky_energy_multiplier() const;

	void set_sky_cover(const Ref<Texture2D> &p_sky_cover);
	Ref<Texture2D> get_sky_cover() const;

	void set_sky_cover_modulate(const Color &p_sky_cover_modulate);
	Color get_sky_cover_modulate() const;

	void set_ground_bottom_color(const Color &p_ground_bottom);
	Color get_ground_bottom_color() const;

	void set_ground_horizon_color(const Color &p_ground_horizon);
	Color get_ground_horizon_color() const;

	void set_ground_curve(float p_curve);
	float get_ground_curve() const;

	void set_ground_energy_multiplier(float p_energy);
	float get_ground_energy_multiplier() const;

	void set_sun_angle_max(float p_angle);
	float get_sun_angle_max() const;

	void set_sun_curve(float p_curve);
	float get_sun_curve() const;

	void set_use_debanding(bool p_use_debanding);
	bool get_use_debanding() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;

	static void cleanup_shader();

	ProceduralSkyMaterial();
	~ProceduralSkyMaterial();
};

//////////////////////////////////////////////////////
/* PanoramaSkyMaterial */

class PanoramaSkyMaterial : public Material {
	GDCLASS(PanoramaSkyMaterial, Material);

private:
	Ref<Texture2D> panorama;
	float energy_multiplier = 1.0f;

	static Mutex shader_mutex;
	static RID shader_cache[2];
	static void _update_shader(bool p_filter);
	mutable bool shader_set = false;

	bool filter = true;

protected:
	static void _bind_methods();

public:
	void set_panorama(const Ref<Texture2D> &p_panorama);
	Ref<Texture2D> get_panorama() const;

	void set_filtering_enabled(bool p_enabled);
	bool is_filtering_enabled() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;

	static void cleanup_shader();

	PanoramaSkyMaterial();
	~PanoramaSkyMaterial();
};

//////////////////////////////////////////////////////
/* PanoramaSkyMaterial */

class PhysicalSkyMaterial : public Material {
	GDCLASS(PhysicalSkyMaterial, Material);

private:
	static Mutex shader_mutex;
	static RID shader_cache[4];

	RID get_shader_cache() const;

	float rayleigh = 0.0f;
	Color rayleigh_color;
	float mie = 0.0f;
	float mie_eccentricity = 0.0f;
	Color mie_color;
	float turbidity = 0.0f;
	float sun_disk_scale = 0.0f;
	Color ground_color;
	float energy_multiplier = 1.0f;
	bool use_debanding = true;
	Ref<Texture2D> night_sky;
	static void _update_shader(bool p_use_debanding, bool p_use_night_sky);
	mutable bool shader_set = false;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &property) const;

public:
	void set_rayleigh_coefficient(float p_rayleigh);
	float get_rayleigh_coefficient() const;

	void set_rayleigh_color(Color p_rayleigh_color);
	Color get_rayleigh_color() const;

	void set_turbidity(float p_turbidity);
	float get_turbidity() const;

	void set_mie_coefficient(float p_mie);
	float get_mie_coefficient() const;

	void set_mie_eccentricity(float p_eccentricity);
	float get_mie_eccentricity() const;

	void set_mie_color(Color p_mie_color);
	Color get_mie_color() const;

	void set_sun_disk_scale(float p_sun_disk_scale);
	float get_sun_disk_scale() const;

	void set_ground_color(Color p_ground_color);
	Color get_ground_color() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	void set_exposure_value(float p_exposure);
	float get_exposure_value() const;

	void set_use_debanding(bool p_use_debanding);
	bool get_use_debanding() const;

	void set_night_sky(const Ref<Texture2D> &p_night_sky);
	Ref<Texture2D> get_night_sky() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;

	static void cleanup_shader();
	virtual RID get_rid() const override;

	PhysicalSkyMaterial();
	~PhysicalSkyMaterial();
};

//////////////////////////////////////////////////////
/* AtmosphereSkyMaterial */

// A deterministic, non-volumetric atmosphere. It intentionally owns visible
// Sky radiance only; renderer-owned direct-light and cloud-shadow integration
// is a separate contract.
class AtmosphereSkyMaterial : public Material {
	GDCLASS(AtmosphereSkyMaterial, Material);

public:
	enum TwilightPalette {
		TWILIGHT_PALETTE_CUSTOM,
		TWILIGHT_PALETTE_WARM_GOLD,
		TWILIGHT_PALETTE_BLUE,
		TWILIGHT_PALETTE_PURPLE,
		TWILIGHT_PALETTE_RED,
	};

private:

	float time_of_day = 12.0f;
	float latitude = 35.0f;
	int day_of_year = 172;
	float north_offset = 0.0f;
	float turbidity = 2.5f;
	float scattering_strength = 1.0f;
	float exposure = 1.0f;
	float day_brightness = 1.7f;
	float sun_disk_size = 0.53f;
	float sun_disk_energy = 24.0f;
	float moon_disk_size = 0.52f;
	float moon_disk_energy = 5.0f;
	float moonlit_night_floor = 0.025f;
	float twilight_duration = 0.45f;
	TwilightPalette twilight_palette = TWILIGHT_PALETTE_CUSTOM;
	float twilight_saturation = 1.0f;
	float twilight_intensity = 1.0f;
	Color dawn_color = Color(0.63f, 0.28f, 0.58f);
	Color dusk_color = Color(0.84f, 0.31f, 0.22f);
	Color dark_night_sky_color = Color(0.008f, 0.003f, 0.018f);
	Color moonlight_color = Color(0.64f, 0.56f, 0.94f);
	float cloud_coverage = 0.25f;
	float cloud_density = 0.7f;
	float cloud_scale = 1.5f;
	float cloud_motion_scale = 1.0f;
	Vector2 cloud_wind = Vector2(0.015f, 0.008f);
	uint32_t cloud_seed = 1;
	float cloud_attenuation = 0.8f;
	float simulated_time = 0.0f;

	Vector3 sun_direction;
	Vector3 previous_sun_direction;
	Vector3 moon_direction;
	Color sun_color;
	Color moon_color;
	float sun_cloud_transmittance = 1.0f;
	uint64_t solar_state_generation = 0;
	uint64_t solar_partition_generation = 0;
	uint64_t solar_history_epoch = 1;
	bool solar_state_initialized = false;

	static Mutex shader_mutex;
	static RID shader_cache;
	mutable bool shader_set = false;
	static void _update_shader();
	void _update_state(bool p_advance_solar_history = true);

protected:
	static void _bind_methods();

public:
	void set_time_of_day(float p_time);
	float get_time_of_day() const;
	void set_latitude(float p_latitude);
	float get_latitude() const;
	void set_day_of_year(int p_day);
	int get_day_of_year() const;
	void set_north_offset(float p_offset);
	float get_north_offset() const;
	void set_turbidity(float p_turbidity);
	float get_turbidity() const;
	void set_scattering_strength(float p_strength);
	float get_scattering_strength() const;
	void set_exposure(float p_exposure);
	float get_exposure() const;
	void set_day_brightness(float p_brightness);
	float get_day_brightness() const;
	void set_sun_disk_size(float p_size);
	float get_sun_disk_size() const;
	void set_sun_disk_energy(float p_energy);
	float get_sun_disk_energy() const;
	void set_moon_disk_size(float p_size);
	float get_moon_disk_size() const;
	void set_moon_disk_energy(float p_energy);
	float get_moon_disk_energy() const;
	void set_moonlit_night_floor(float p_floor);
	float get_moonlit_night_floor() const;
	void set_twilight_duration(float p_duration);
	float get_twilight_duration() const;
	void set_twilight_palette(TwilightPalette p_palette);
	TwilightPalette get_twilight_palette() const;
	void set_twilight_saturation(float p_saturation);
	float get_twilight_saturation() const;
	void set_twilight_intensity(float p_intensity);
	float get_twilight_intensity() const;
	void set_dawn_color(const Color &p_color);
	Color get_dawn_color() const;
	void set_dusk_color(const Color &p_color);
	Color get_dusk_color() const;
	void set_dark_night_sky_color(const Color &p_color);
	Color get_dark_night_sky_color() const;
	void set_moonlight_color(const Color &p_color);
	Color get_moonlight_color() const;
	void set_cloud_coverage(float p_coverage);
	float get_cloud_coverage() const;
	void set_cloud_density(float p_density);
	float get_cloud_density() const;
	void set_cloud_scale(float p_scale);
	float get_cloud_scale() const;
	void set_cloud_motion_scale(float p_scale);
	float get_cloud_motion_scale() const;
	void set_cloud_wind(const Vector2 &p_wind);
	Vector2 get_cloud_wind() const;
	void set_cloud_seed(uint32_t p_seed);
	uint32_t get_cloud_seed() const;
	void set_cloud_attenuation(float p_attenuation);
	float get_cloud_attenuation() const;
	void set_simulated_time(float p_time);
	float get_simulated_time() const;
	Vector3 get_sun_direction() const;
	Vector3 get_moon_direction() const;
	Color get_sun_color() const;
	Color get_moon_color() const;
	Vector3 get_previous_sun_direction() const;
	float get_sun_cloud_transmittance() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;
	static void cleanup_shader();

	AtmosphereSkyMaterial();
	~AtmosphereSkyMaterial();
};

VARIANT_ENUM_CAST(AtmosphereSkyMaterial::TwilightPalette);
