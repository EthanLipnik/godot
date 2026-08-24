/**************************************************************************/
/*  test_sky_lighting.cpp                                                 */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_sky_lighting)

#include "servers/rendering/sky_lighting.h"

#include <limits>

namespace TestSkyLighting {

using namespace RendererSkyLighting;

static SkyLightingLobe lobe(float p_radius = 0.01f) {
	SkyLightingLobe result;
	result.source_id = 11;
	result.sample_id = 12;
	result.current_direction = Vector3(0, 1, 0);
	result.previous_direction = Vector3(0, 1, 0);
	result.angular_radius = p_radius;
	result.perpendicular_irradiance = Color(3.0f, 2.0f, 1.0f);
	return result;
}

static SkyLightingState state() {
	SkyLightingState result;
	result.state_generation = 1;
	result.radiance_generation = 1;
	result.partition_generation = 1;
	result.cloud_content_generation = 1;
	result.history_epoch = 1;
	return result;
}

TEST_CASE("[SkyLighting] Uniform disk recovers perpendicular irradiance across radii") {
	const float radii[] = { 0.001f, 0.01f, 0.25f, 0.75f };
	for (float radius : radii) {
		const SkyLightingLobe value = lobe(radius);
		const Color recovered = sky_lighting_recover_uniform_disk_irradiance(sky_lighting_uniform_disk_radiance(value), radius);
		CHECK(recovered.r == doctest::Approx(value.perpendicular_irradiance.r).epsilon(1e-5));
		CHECK(recovered.g == doctest::Approx(value.perpendicular_irradiance.g).epsilon(1e-5));
		CHECK(recovered.b == doctest::Approx(value.perpendicular_irradiance.b).epsilon(1e-5));
		CHECK(sky_lighting_evaluate_uniform_disk(value, value.current_direction).r == doctest::Approx(sky_lighting_uniform_disk_radiance(value).r).epsilon(1e-5));
	}
}

TEST_CASE("[SkyLighting] Contract radius is half the Godot Sky and Light3D diameter") {
	const float godot_diameter = 0.0087f;
	const SkyLightingLobe value = lobe(godot_diameter * 0.5f);
	CHECK(value.angular_radius == doctest::Approx(0.00435f));
	CHECK(sky_lighting_validate_lobe(value));
	CHECK(sky_lighting_uniform_disk_radiance(lobe(godot_diameter)).r < sky_lighting_uniform_disk_radiance(value).r);
}

TEST_CASE("[SkyLighting] Solar runtime keeps integrated energy, cloud scalar, and history identity together") {
	SkyLightingSolarLobeRuntime runtime;
	runtime.enabled = true;
	runtime.lobe = lobe(0.05f);
	runtime.lobe.perpendicular_irradiance = Color(2.4f, 1.6f, 0.8f);
	runtime.cloud_transmittance = 0.6f;
	runtime.profile_version = 7;
	runtime.partition_version = 11;
	runtime.state_generation = 12;
	runtime.history_epoch = 3;
	CHECK(sky_lighting_validate_solar_lobe_runtime(runtime));
	const Color radiance = sky_lighting_uniform_disk_radiance(runtime.lobe);
	const Color recovered = sky_lighting_recover_uniform_disk_irradiance(radiance, runtime.lobe.angular_radius);
	CHECK(recovered.r == doctest::Approx(runtime.lobe.perpendicular_irradiance.r).epsilon(1e-5));
	runtime.lobe.previous_direction = Vector3(1, 0, 0);
	CHECK(sky_lighting_validate_solar_lobe_runtime(runtime));
	runtime.cloud_transmittance = 1.01f;
	CHECK_FALSE(sky_lighting_validate_solar_lobe_runtime(runtime));
	runtime.cloud_transmittance = 0.6f;
	runtime.partition_version = 0;
	CHECK_FALSE(sky_lighting_validate_solar_lobe_runtime(runtime));
}

TEST_CASE("[SkyLighting] Lunar runtime carries a finite directional raster emitter") {
	SkyLightingLunarLobeRuntime runtime;
	runtime.enabled = true;
	runtime.lobe = lobe(0.0045f);
	runtime.lobe.domain = SKY_LIGHTING_DOMAIN_LUNAR;
	runtime.lobe.perpendicular_irradiance = Color(0.002f, 0.0018f, 0.003f);
	runtime.cloud_transmittance = 0.75f;
	runtime.profile_version = 1;
	runtime.state_generation = 2;
	runtime.history_epoch = 1;
	CHECK(sky_lighting_validate_lunar_lobe_runtime(runtime));
	runtime.lobe.domain = SKY_LIGHTING_DOMAIN_SOLAR;
	CHECK_FALSE(sky_lighting_validate_lunar_lobe_runtime(runtime));
}

TEST_CASE("[SkyLighting] Full radiance equals residual plus finite lobe radiance") {
	Color residual;
	const Color full(5.0f, 4.0f, 3.0f);
	const Color extracted(2.0f, 1.0f, 0.5f);
	CHECK(sky_lighting_subtract_lobe_radiance(full, extracted, residual));
	SkyLightingRadiancePartition partition;
	partition.full_generation = 1;
	partition.residual_generation = 1;
	partition.partition_generation = 1;
	partition.full_radiance = full;
	partition.residual_radiance = residual;
	partition.lobe_radiance.push_back(extracted);
	CHECK(sky_lighting_validate_partition(partition));
}

TEST_CASE("[SkyLighting] Oversubtraction and nonfinite radiance fail closed") {
	Color residual(1.0f, 1.0f, 1.0f);
	CHECK_FALSE(sky_lighting_subtract_lobe_radiance(Color(1, 1, 1), Color(1.1f, 0, 0), residual));
	CHECK(residual == Color());
	CHECK_FALSE(sky_lighting_subtract_lobe_radiance(Color(std::numeric_limits<float>::infinity(), 1, 1), Color(), residual));
}

TEST_CASE("[SkyLighting] Domain-tagged IDs are deterministic and duplicate sample IDs reject") {
	const uint64_t solar = sky_lighting_derive_id(SKY_LIGHTING_DOMAIN_SOLAR, 17, 2);
	CHECK_EQ(solar, sky_lighting_derive_id(SKY_LIGHTING_DOMAIN_SOLAR, 17, 2));
	CHECK_NE(solar, sky_lighting_derive_id(SKY_LIGHTING_DOMAIN_LUNAR, 17, 2));
	CHECK_NE(solar, 0);
	SkyLightingState value = state();
	value.lobes.push_back(lobe());
	SkyLightingLobe shared_source = lobe();
	shared_source.domain = SKY_LIGHTING_DOMAIN_LUNAR;
	shared_source.sample_id = 13;
	value.lobes.push_back(shared_source);
	CHECK(sky_lighting_validate_state(value));
	shared_source.sample_id = 12;
	value.lobes.write[1] = shared_source;
	CHECK_FALSE(sky_lighting_validate_state(value));
}

TEST_CASE("[SkyLighting] Cloud transmittance is clear, monotonic, and more attenuated obliquely") {
	Vector<SkyCloudOpticalDepthLayer> layers;
	SkyCloudOpticalDepthLayer layer;
	layer.altitude = 10.0f;
	layer.uv_min = Vector2(-1.0f, -1.0f);
	layer.uv_max = Vector2(1.0f, 20.0f);
	layers.push_back(layer);
	float transmittance = 0.0f;
	CHECK(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	CHECK(transmittance == doctest::Approx(1.0f));
	layers.write[0].optical_depth = 1.0f;
	CHECK(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	const float vertical = transmittance;
	CHECK(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 1).normalized(), Vector3(0, 1, 0), Vector3(), transmittance));
	CHECK(transmittance < vertical);
}

TEST_CASE("[SkyLighting] Signed ray-plane intersections handle downward sources") {
	Vector<SkyCloudOpticalDepthLayer> layers;
	SkyCloudOpticalDepthLayer below_receiver;
	below_receiver.altitude = 10.0f;
	below_receiver.optical_depth = 1.0f;
	layers.push_back(below_receiver);
	float transmittance = 1.0f;
	CHECK(sky_lighting_cloud_transmittance(layers, Vector3(0, 20, 0), Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	CHECK(transmittance < 1.0f);
	CHECK(transmittance == doctest::Approx(0.3678794f).epsilon(1e-5));
	layers.write[0].altitude = 30.0f;
	CHECK(sky_lighting_cloud_transmittance(layers, Vector3(0, 20, 0), Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	CHECK(transmittance == doctest::Approx(1.0f));
}

TEST_CASE("[SkyLighting] State updates retain previous values and only discontinuities reset history") {
	SkyLightingState value = state();
	value.current_simulation_time = 2.0;
	value.lobes.push_back(lobe());
	value.current_world_from_local.origin = Vector3(1, 0, 0);
	Vector<Vector3> directions;
	CHECK_FALSE(sky_lighting_apply_continuous_update(value, 3.0, directions, Transform3D()));
	directions.push_back(Vector3(1, 0, 0));
	CHECK(sky_lighting_apply_continuous_update(value, 3.0, directions, Transform3D(Basis(), Vector3(2, 0, 0))));
	CHECK_EQ(value.previous_simulation_time, 2.0);
	CHECK(value.lobes[0].previous_direction == Vector3(0, 1, 0));
	CHECK(value.lobes[0].current_direction == Vector3(1, 0, 0));
	CHECK(value.previous_world_from_local.origin == Vector3(1, 0, 0));
	CHECK_EQ(value.history_epoch, 1);
	CHECK_EQ(value.state_generation, 2);
	CHECK_EQ(value.radiance_generation, 2);
	CHECK_EQ(value.partition_generation, 2);
	CHECK(sky_lighting_mark_cloud_content_changed(value, false));
	CHECK_EQ(value.cloud_content_generation, 2);
	CHECK_EQ(value.history_epoch, 1);
	CHECK(sky_lighting_mark_cloud_content_changed(value, true));
	CHECK_EQ(value.history_epoch, 2);
	directions.write[0] = Vector3(0, 0, 1);
	CHECK(sky_lighting_apply_discontinuous_update(value, 10.0, directions, Transform3D()));
	CHECK_EQ(value.history_epoch, 3);
}

TEST_CASE("[SkyLighting] Invalid state, layer bound, and transforms reject") {
	SkyLightingState value = state();
	value.lobes.push_back(lobe());
	value.lobes.write[0].current_direction = Vector3();
	CHECK_FALSE(sky_lighting_validate_state(value));
	value = state();
	value.state_generation = 0;
	CHECK_FALSE(sky_lighting_validate_state(value));
	value = state();
	for (uint32_t i = 0; i <= SKY_LIGHTING_MAX_CLOUD_LAYERS; i++) {
		value.cloud_layers.push_back(SkyCloudOpticalDepthLayer());
	}
	CHECK_FALSE(sky_lighting_validate_state(value));
	SkyCloudOpticalDepthLayer invalid;
	invalid.optical_depth = -1.0f;
	Vector<SkyCloudOpticalDepthLayer> layers;
	layers.push_back(invalid);
	float transmittance = 0.0f;
	CHECK_FALSE(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	CHECK_FALSE(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	invalid.optical_depth = 0.0f;
	invalid.uv_min.x = 1.0f;
	invalid.uv_max.x = 1.0f;
	layers.write[0] = invalid;
	CHECK_FALSE(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	invalid.uv_min = Vector2(-1.0f, -1.0f);
	invalid.uv_max = Vector2(1.0f, 1.0f);
	invalid.current_world_to_uv.basis = Basis(Vector3(), Vector3(), Vector3());
	layers.write[0] = invalid;
	CHECK_FALSE(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	SkyCloudOpticalDepthLayer outside_bounds;
	outside_bounds.altitude = 10.0f;
	outside_bounds.optical_depth = 2.0f;
	outside_bounds.uv_min = Vector2(-1.0f, -1.0f);
	outside_bounds.uv_max = Vector2(1.0f, 1.0f);
	layers.write[0] = outside_bounds;
	CHECK(sky_lighting_cloud_transmittance(layers, Vector3(), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(), transmittance));
	CHECK(transmittance == doctest::Approx(1.0f));
	value = state();
	SkyCloudOpticalDepthLayer singular_previous;
	singular_previous.previous_world_to_uv.basis = Basis(Vector3(), Vector3(), Vector3());
	value.cloud_layers.push_back(singular_previous);
	CHECK_FALSE(sky_lighting_validate_state(value));
}

} // namespace TestSkyLighting
