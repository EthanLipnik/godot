/**************************************************************************/
/*  sky_lighting.h                                                        */
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

#pragma once

#include "core/math/color.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <cstdint>

namespace RendererSkyLighting {

// CPU semantic contract only: no scene lights, renderer resources, GPU
// layouts, or graphics API types appear here.
static constexpr uint32_t SKY_LIGHTING_MAX_LOBES = 4;
static constexpr uint32_t SKY_LIGHTING_MAX_CLOUD_LAYERS = 8;
static constexpr float SKY_LIGHTING_RADIANCE_TOLERANCE = 1e-5f;

enum SkyLightingDomain : uint32_t {
	SKY_LIGHTING_DOMAIN_SOLAR = 1,
	SKY_LIGHTING_DOMAIN_LUNAR = 2,
};

// Source-local directions point receiver -> source. Angular radii use radians, unlike
// Godot's Light3D/Sky shader diameter-facing properties. Irradiance and
// radiance are linear scene-working RGB.
struct SkyLightingLobe {
	// source_id is source lineage and may be shared; sample_id is unique.
	uint64_t source_id = 0;
	uint64_t sample_id = 0;
	SkyLightingDomain domain = SKY_LIGHTING_DOMAIN_SOLAR;
	Vector3 current_direction;
	Vector3 previous_direction;
	float angular_radius = 0.0f;
	Color perpendicular_irradiance;
};

// Backend-neutral runtime packet for one explicitly partitioned solar lobe.
// The lobe irradiance is already cloud-attenuated; consumers must not apply
// cloud transmittance a second time.
struct SkyLightingSolarLobeRuntime {
	SkyLightingLobe lobe;
	float cloud_transmittance = 0.0f;
	uint64_t profile_version = 0;
	uint64_t partition_version = 0;
	uint64_t state_generation = 0;
	uint64_t history_epoch = 0;
	bool enabled = false;
};

// L_full = L_residual + sum(L_lobe), at a single directional RGB sample.
struct SkyLightingRadiancePartition {
	uint64_t full_generation = 0;
	uint64_t residual_generation = 0;
	uint64_t partition_generation = 0;
	Color full_radiance;
	Color residual_radiance;
	Vector<Color> lobe_radiance;
};

// A planar optical-depth field at altitude above reference_origin along
// world_up. The optical_depth is the texture-independent reference value at
// every transformed sample within [uv_min, uv_max]. Outside that finite,
// nonempty domain the layer makes no contribution rather than extrapolating.
struct SkyCloudOpticalDepthLayer {
	float altitude = 0.0f;
	float optical_depth = 0.0f;
	Vector2 uv_min = Vector2(-10000.0f, -10000.0f);
	Vector2 uv_max = Vector2(10000.0f, 10000.0f);
	Transform3D current_world_to_uv;
	Transform3D previous_world_to_uv;
};

// Continuous updates advance state/radiance/partition generation but leave
// history_epoch alone. Discontinuous seed, weather, topology, scrub, or
// profile changes also advance history_epoch.
struct SkyLightingState {
	uint64_t state_generation = 0;
	uint64_t radiance_generation = 0;
	uint64_t partition_generation = 0;
	uint64_t cloud_content_generation = 0;
	uint64_t history_epoch = 0;
	double current_simulation_time = 0.0;
	double previous_simulation_time = 0.0;
	Transform3D current_world_from_local;
	Transform3D previous_world_from_local;
	Vector<SkyLightingLobe> lobes;
	Vector<SkyCloudOpticalDepthLayer> cloud_layers;
};

uint64_t sky_lighting_derive_id(SkyLightingDomain p_domain, uint64_t p_source_identity, uint64_t p_ordinal);

bool sky_lighting_validate_lobe(const SkyLightingLobe &p_lobe, String *r_error = nullptr);
bool sky_lighting_validate_solar_lobe_runtime(const SkyLightingSolarLobeRuntime &p_runtime, String *r_error = nullptr);
bool sky_lighting_validate_partition(const SkyLightingRadiancePartition &p_partition, String *r_error = nullptr);
bool sky_lighting_validate_state(const SkyLightingState &p_state, String *r_error = nullptr);

// The profile integrates to one under cos(theta) d omega. Therefore a uniform
// disk has radiance E_perp / (PI * sin(radius)^2).
float sky_lighting_uniform_disk_profile(const Vector3 &p_receiver_to_source, const Vector3 &p_lobe_direction, float p_angular_radius);
Color sky_lighting_uniform_disk_radiance(const SkyLightingLobe &p_lobe);
Color sky_lighting_evaluate_uniform_disk(const SkyLightingLobe &p_lobe, const Vector3 &p_receiver_to_source);
Color sky_lighting_recover_uniform_disk_irradiance(const Color &p_radiance, float p_angular_radius);

// Oversubtraction beyond tolerance fails closed; it is never silently clamped.
bool sky_lighting_subtract_lobe_radiance(const Color &p_full_radiance, const Color &p_lobe_radiance, Color &r_residual, float p_tolerance = SKY_LIGHTING_RADIANCE_TOLERANCE);

// exp(-sum(tau_v / max(abs(dot(direction, world_up)), epsilon))) over forward
// intersections with bounded planar layers. Sampling is deterministic and
// texture-independent through each layer's supplied optical depth.
bool sky_lighting_cloud_transmittance(const Vector<SkyCloudOpticalDepthLayer> &p_layers, const Vector3 &p_receiver_position, const Vector3 &p_receiver_to_source, const Vector3 &p_world_up, const Vector3 &p_reference_origin, float &r_transmittance, float p_cosine_epsilon = 1e-4f, String *r_error = nullptr);

bool sky_lighting_apply_continuous_update(SkyLightingState &r_state, double p_simulation_time, const Vector<Vector3> &p_current_lobe_directions, const Transform3D &p_world_from_local, String *r_error = nullptr);
bool sky_lighting_apply_discontinuous_update(SkyLightingState &r_state, double p_simulation_time, const Vector<Vector3> &p_current_lobe_directions, const Transform3D &p_world_from_local, String *r_error = nullptr);
bool sky_lighting_mark_cloud_content_changed(SkyLightingState &r_state, bool p_discontinuous, String *r_error = nullptr);

} // namespace RendererSkyLighting
