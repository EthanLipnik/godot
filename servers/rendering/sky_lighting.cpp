/**************************************************************************/
/*  sky_lighting.cpp                                                      */
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
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "sky_lighting.h"

#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"

#include <cmath>

namespace RendererSkyLighting {

namespace {

static bool _is_valid_color(const Color &p_color) {
	return Math::is_finite(p_color.r) && Math::is_finite(p_color.g) && Math::is_finite(p_color.b) && p_color.r >= 0.0f && p_color.g >= 0.0f && p_color.b >= 0.0f;
}

static bool _is_valid_direction(const Vector3 &p_direction) {
	return p_direction.is_finite() && p_direction.length_squared() > 0.0f && Math::is_equal_approx(p_direction.length(), 1.0f);
}

static bool _is_valid_transform(const Transform3D &p_transform) {
	return p_transform.is_finite() && Math::abs(p_transform.basis.determinant()) > CMP_EPSILON;
}

static bool _is_valid_layer(const SkyCloudOpticalDepthLayer &p_layer) {
	return Math::is_finite(p_layer.altitude) && Math::is_finite(p_layer.optical_depth) && p_layer.optical_depth >= 0.0f &&
			p_layer.uv_min.is_finite() && p_layer.uv_max.is_finite() && p_layer.uv_min.x < p_layer.uv_max.x && p_layer.uv_min.y < p_layer.uv_max.y &&
			_is_valid_transform(p_layer.current_world_to_uv) && _is_valid_transform(p_layer.previous_world_to_uv);
}

static Color _rgb_scaled(const Color &p_color, float p_scale) {
	return Color(p_color.r * p_scale, p_color.g * p_scale, p_color.b * p_scale);
}

static bool _fail(const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return false;
}

static uint64_t _mix(uint64_t p_value) {
	p_value += 0x9e3779b97f4a7c15ULL;
	p_value = (p_value ^ (p_value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	p_value = (p_value ^ (p_value >> 27)) * 0x94d049bb133111ebULL;
	return p_value ^ (p_value >> 31);
}

static bool _apply_update(SkyLightingState &r_state, double p_simulation_time, const Vector<Vector3> &p_current_lobe_directions, const Transform3D &p_world_from_local, bool p_discontinuous, String *r_error) {
	if (!std::isfinite(p_simulation_time) || !_is_valid_transform(p_world_from_local) || p_current_lobe_directions.size() != r_state.lobes.size()) {
		return _fail("Sky lighting update contains an invalid time, transform, or lobe-direction count.", r_error);
	}
	for (const Vector3 &direction : p_current_lobe_directions) {
		if (!_is_valid_direction(direction)) {
			return _fail("Sky lighting update contains an invalid lobe direction.", r_error);
		}
	}
	if (r_state.state_generation == UINT64_MAX || r_state.radiance_generation == UINT64_MAX || r_state.partition_generation == UINT64_MAX || (p_discontinuous && r_state.history_epoch == UINT64_MAX)) {
		return _fail("Sky lighting generation overflow.", r_error);
	}
	r_state.previous_simulation_time = r_state.current_simulation_time;
	r_state.previous_world_from_local = r_state.current_world_from_local;
	r_state.current_simulation_time = p_simulation_time;
	r_state.current_world_from_local = p_world_from_local;
	for (int i = 0; i < r_state.lobes.size(); i++) {
		SkyLightingLobe &lobe = r_state.lobes.write[i];
		lobe.previous_direction = lobe.current_direction;
		lobe.current_direction = p_current_lobe_directions[i];
	}
	r_state.state_generation++;
	r_state.radiance_generation++;
	r_state.partition_generation++;
	if (p_discontinuous) {
		r_state.history_epoch++;
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

} // namespace

uint64_t sky_lighting_derive_id(SkyLightingDomain p_domain, uint64_t p_source_identity, uint64_t p_ordinal) {
	uint64_t value = _mix(p_source_identity) ^ _mix(p_ordinal + 0x4cf5ad432745937fULL) ^ _mix((uint64_t)p_domain * 0xd6e8feb86659fd93ULL);
	return value == 0 ? 1 : value;
}

bool sky_lighting_validate_lobe(const SkyLightingLobe &p_lobe, String *r_error) {
	if (p_lobe.source_id == 0 || p_lobe.sample_id == 0) {
		return _fail("Sky lighting lobe IDs must be nonzero.", r_error);
	}
	if (p_lobe.domain != SKY_LIGHTING_DOMAIN_SOLAR && p_lobe.domain != SKY_LIGHTING_DOMAIN_LUNAR) {
		return _fail("Sky lighting lobe domain is invalid.", r_error);
	}
	if (!_is_valid_direction(p_lobe.current_direction) || !_is_valid_direction(p_lobe.previous_direction)) {
		return _fail("Sky lighting lobe directions must be finite and normalized.", r_error);
	}
	if (!Math::is_finite(p_lobe.angular_radius) || p_lobe.angular_radius <= 0.0f || p_lobe.angular_radius > Math::PI * 0.5f) {
		return _fail("Sky lighting lobe angular radius must be in (0, pi/2].", r_error);
	}
	if (!_is_valid_color(p_lobe.perpendicular_irradiance)) {
		return _fail("Sky lighting lobe irradiance must be finite and nonnegative.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

bool sky_lighting_validate_solar_lobe_runtime(const SkyLightingSolarLobeRuntime &p_runtime, String *r_error) {
	if (!p_runtime.enabled) {
		return _fail("Sky solar lobe runtime is disabled.", r_error);
	}
	if (p_runtime.lobe.domain != SKY_LIGHTING_DOMAIN_SOLAR || !sky_lighting_validate_lobe(p_runtime.lobe, r_error)) {
		return false;
	}
	if (!Math::is_finite(p_runtime.cloud_transmittance) || p_runtime.cloud_transmittance < 0.0f || p_runtime.cloud_transmittance > 1.0f) {
		return _fail("Sky solar lobe cloud transmittance must be finite and within [0, 1].", r_error);
	}
	if (p_runtime.profile_version == 0 || p_runtime.partition_version == 0 || p_runtime.state_generation == 0 || p_runtime.history_epoch == 0) {
		return _fail("Sky solar lobe profile, partition, state, and history generations must be nonzero.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

bool sky_lighting_validate_partition(const SkyLightingRadiancePartition &p_partition, String *r_error) {
	if (p_partition.full_generation == 0 || p_partition.residual_generation == 0 || p_partition.partition_generation == 0) {
		return _fail("Sky lighting partition generations must be nonzero.", r_error);
	}
	if (!_is_valid_color(p_partition.full_radiance) || !_is_valid_color(p_partition.residual_radiance) || p_partition.lobe_radiance.size() > SKY_LIGHTING_MAX_LOBES) {
		return _fail("Sky lighting partition radiance or lobe bound is invalid.", r_error);
	}
	Color recombined = p_partition.residual_radiance;
	for (const Color &lobe : p_partition.lobe_radiance) {
		if (!_is_valid_color(lobe)) {
			return _fail("Sky lighting lobe radiance must be finite and nonnegative.", r_error);
		}
		recombined.r += lobe.r;
		recombined.g += lobe.g;
		recombined.b += lobe.b;
	}
	if (Math::abs(recombined.r - p_partition.full_radiance.r) > SKY_LIGHTING_RADIANCE_TOLERANCE || Math::abs(recombined.g - p_partition.full_radiance.g) > SKY_LIGHTING_RADIANCE_TOLERANCE || Math::abs(recombined.b - p_partition.full_radiance.b) > SKY_LIGHTING_RADIANCE_TOLERANCE) {
		return _fail("Sky lighting partition does not conserve full radiance.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

bool sky_lighting_validate_state(const SkyLightingState &p_state, String *r_error) {
	if (p_state.state_generation == 0 || p_state.radiance_generation == 0 || p_state.partition_generation == 0 || p_state.cloud_content_generation == 0 || p_state.history_epoch == 0) {
		return _fail("Sky lighting state generations and history epoch must be nonzero.", r_error);
	}
	if (!std::isfinite(p_state.current_simulation_time) || !std::isfinite(p_state.previous_simulation_time) || !_is_valid_transform(p_state.current_world_from_local) || !_is_valid_transform(p_state.previous_world_from_local)) {
		return _fail("Sky lighting state has an invalid time or transform.", r_error);
	}
	if (p_state.lobes.size() > SKY_LIGHTING_MAX_LOBES || p_state.cloud_layers.size() > SKY_LIGHTING_MAX_CLOUD_LAYERS) {
		return _fail("Sky lighting state exceeds a bounded lobe or cloud-layer count.", r_error);
	}
	HashSet<uint64_t> sample_ids;
	for (const SkyLightingLobe &lobe : p_state.lobes) {
		if (!sky_lighting_validate_lobe(lobe, r_error)) {
			return false;
		}
		if (sample_ids.has(lobe.sample_id)) {
			return _fail("Sky lighting lobe sample IDs must be unique within a state.", r_error);
		}
		sample_ids.insert(lobe.sample_id);
	}
	for (const SkyCloudOpticalDepthLayer &layer : p_state.cloud_layers) {
		if (!_is_valid_layer(layer)) {
			return _fail("Sky cloud layer has invalid optical depth, altitude, or transform.", r_error);
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

float sky_lighting_uniform_disk_profile(const Vector3 &p_receiver_to_source, const Vector3 &p_lobe_direction, float p_angular_radius) {
	if (!_is_valid_direction(p_receiver_to_source) || !_is_valid_direction(p_lobe_direction) || !Math::is_finite(p_angular_radius) || p_angular_radius <= 0.0f || p_angular_radius > Math::PI * 0.5f || p_receiver_to_source.dot(p_lobe_direction) < Math::cos(p_angular_radius)) {
		return 0.0f;
	}
	const float sine = Math::sin(p_angular_radius);
	return 1.0f / (Math::PI * sine * sine);
}

Color sky_lighting_uniform_disk_radiance(const SkyLightingLobe &p_lobe) {
	if (!sky_lighting_validate_lobe(p_lobe)) {
		return Color();
	}
	const float sine = Math::sin(p_lobe.angular_radius);
	return _rgb_scaled(p_lobe.perpendicular_irradiance, 1.0f / (Math::PI * sine * sine));
}

Color sky_lighting_evaluate_uniform_disk(const SkyLightingLobe &p_lobe, const Vector3 &p_receiver_to_source) {
	if (!sky_lighting_validate_lobe(p_lobe)) {
		return Color();
	}
	return _rgb_scaled(p_lobe.perpendicular_irradiance, sky_lighting_uniform_disk_profile(p_receiver_to_source, p_lobe.current_direction, p_lobe.angular_radius));
}

Color sky_lighting_recover_uniform_disk_irradiance(const Color &p_radiance, float p_angular_radius) {
	if (!_is_valid_color(p_radiance) || !Math::is_finite(p_angular_radius) || p_angular_radius <= 0.0f || p_angular_radius > Math::PI * 0.5f) {
		return Color();
	}
	const float sine = Math::sin(p_angular_radius);
	return _rgb_scaled(p_radiance, Math::PI * sine * sine);
}

bool sky_lighting_subtract_lobe_radiance(const Color &p_full_radiance, const Color &p_lobe_radiance, Color &r_residual, float p_tolerance) {
	r_residual = Color();
	if (!_is_valid_color(p_full_radiance) || !_is_valid_color(p_lobe_radiance) || !Math::is_finite(p_tolerance) || p_tolerance < 0.0f || p_lobe_radiance.r > p_full_radiance.r + p_tolerance || p_lobe_radiance.g > p_full_radiance.g + p_tolerance || p_lobe_radiance.b > p_full_radiance.b + p_tolerance) {
		return false;
	}
	r_residual.r = MAX(0.0f, p_full_radiance.r - p_lobe_radiance.r);
	r_residual.g = MAX(0.0f, p_full_radiance.g - p_lobe_radiance.g);
	r_residual.b = MAX(0.0f, p_full_radiance.b - p_lobe_radiance.b);
	return true;
}

bool sky_lighting_cloud_transmittance(const Vector<SkyCloudOpticalDepthLayer> &p_layers, const Vector3 &p_receiver_position, const Vector3 &p_receiver_to_source, const Vector3 &p_world_up, const Vector3 &p_reference_origin, float &r_transmittance, float p_cosine_epsilon, String *r_error) {
	r_transmittance = 0.0f;
	if (p_layers.size() > SKY_LIGHTING_MAX_CLOUD_LAYERS || !p_receiver_position.is_finite() || !_is_valid_direction(p_receiver_to_source) || !_is_valid_direction(p_world_up) || !p_reference_origin.is_finite() || !Math::is_finite(p_cosine_epsilon) || p_cosine_epsilon <= 0.0f) {
		return _fail("Sky cloud transmittance input is invalid.", r_error);
	}
	for (const SkyCloudOpticalDepthLayer &layer : p_layers) {
		if (!_is_valid_layer(layer)) {
			return _fail("Sky cloud layer is invalid.", r_error);
		}
	}
	const float cosine = p_receiver_to_source.dot(p_world_up);
	if (Math::abs(cosine) <= p_cosine_epsilon) {
		r_transmittance = 1.0f;
		if (r_error) {
			r_error->clear();
		}
		return true;
	}
	double total_optical_depth = 0.0;
	const float receiver_altitude = (p_receiver_position - p_reference_origin).dot(p_world_up);
	for (const SkyCloudOpticalDepthLayer &layer : p_layers) {
		const float distance = (layer.altitude - receiver_altitude) / cosine;
		if (distance < 0.0f) {
			continue;
		}
		const Vector3 sample_position = p_receiver_position + p_receiver_to_source * distance;
		const Vector3 transformed = layer.current_world_to_uv.xform(sample_position);
		if (!transformed.is_finite()) {
			return _fail("Sky cloud layer transform produced a nonfinite sample position.", r_error);
		}
		const Vector2 uv(transformed.x, transformed.y);
		if (uv.x < layer.uv_min.x || uv.x > layer.uv_max.x || uv.y < layer.uv_min.y || uv.y > layer.uv_max.y) {
			continue;
		}
		total_optical_depth += layer.optical_depth / MAX(Math::abs(cosine), p_cosine_epsilon);
	}
	r_transmittance = (float)std::exp(-total_optical_depth);
	if (!Math::is_finite(r_transmittance)) {
		return _fail("Sky cloud transmittance is nonfinite.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

bool sky_lighting_apply_continuous_update(SkyLightingState &r_state, double p_simulation_time, const Vector<Vector3> &p_current_lobe_directions, const Transform3D &p_world_from_local, String *r_error) {
	return _apply_update(r_state, p_simulation_time, p_current_lobe_directions, p_world_from_local, false, r_error);
}

bool sky_lighting_apply_discontinuous_update(SkyLightingState &r_state, double p_simulation_time, const Vector<Vector3> &p_current_lobe_directions, const Transform3D &p_world_from_local, String *r_error) {
	return _apply_update(r_state, p_simulation_time, p_current_lobe_directions, p_world_from_local, true, r_error);
}

bool sky_lighting_mark_cloud_content_changed(SkyLightingState &r_state, bool p_discontinuous, String *r_error) {
	if (r_state.state_generation == UINT64_MAX || r_state.radiance_generation == UINT64_MAX || r_state.partition_generation == UINT64_MAX || r_state.cloud_content_generation == UINT64_MAX || (p_discontinuous && r_state.history_epoch == UINT64_MAX)) {
		return _fail("Sky lighting generation overflow.", r_error);
	}
	r_state.state_generation++;
	r_state.radiance_generation++;
	r_state.partition_generation++;
	r_state.cloud_content_generation++;
	if (p_discontinuous) {
		r_state.history_epoch++;
	}
	if (r_error) {
		r_error->clear();
	}
	return true;
}

} // namespace RendererSkyLighting
