/**************************************************************************/
/*  test_sky.cpp                                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_sky)

#ifndef _3D_DISABLED

#include "scene/3d/atmosphere_sky_clock.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/sky.h"

namespace TestSky {

TEST_CASE("[SceneTree][Sky] Constructor") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	CHECK(test_sky->get_process_mode() == Sky::PROCESS_MODE_AUTOMATIC);
	CHECK(test_sky->get_radiance_size() == Sky::RADIANCE_SIZE_256);
	CHECK(test_sky->get_material().is_null());
}

TEST_CASE("[SceneTree][Sky] Radiance size setter and getter") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	// Check default.
	CHECK(test_sky->get_radiance_size() == Sky::RADIANCE_SIZE_256);

	test_sky->set_radiance_size(Sky::RADIANCE_SIZE_1024);
	CHECK(test_sky->get_radiance_size() == Sky::RADIANCE_SIZE_1024);

	ERR_PRINT_OFF;
	// Check setting invalid radiance size.
	test_sky->set_radiance_size(Sky::RADIANCE_SIZE_MAX);
	ERR_PRINT_ON;

	CHECK(test_sky->get_radiance_size() == Sky::RADIANCE_SIZE_1024);
}

TEST_CASE("[SceneTree][Sky] Process mode setter and getter") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	// Check default.
	CHECK(test_sky->get_process_mode() == Sky::PROCESS_MODE_AUTOMATIC);

	test_sky->set_process_mode(Sky::PROCESS_MODE_INCREMENTAL);
	CHECK(test_sky->get_process_mode() == Sky::PROCESS_MODE_INCREMENTAL);
}

TEST_CASE("[SceneTree][Sky] Material setter and getter") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	Ref<Material> material;
	material.instantiate();

	SUBCASE("Material passed to the class should remain the same") {
		test_sky->set_material(material);
		CHECK(test_sky->get_material() == material);
	}
	SUBCASE("Material passed many times to the class should remain the same") {
		test_sky->set_material(material);
		test_sky->set_material(material);
		test_sky->set_material(material);
		CHECK(test_sky->get_material() == material);
	}
	SUBCASE("Material rewrite testing") {
		Ref<Material> material1;
		Ref<Material> material2;
		material1.instantiate();
		material2.instantiate();

		test_sky->set_material(material1);
		test_sky->set_material(material2);
		CHECK_MESSAGE(test_sky->get_material() != material1,
				"After rewrite, second material should be in class.");
		CHECK_MESSAGE(test_sky->get_material() == material2,
				"After rewrite, second material should be in class.");
	}

	SUBCASE("Assign same material to two skys") {
		Ref<Sky> sky2;
		sky2.instantiate();

		test_sky->set_material(material);
		sky2->set_material(material);
		CHECK_MESSAGE(test_sky->get_material() == sky2->get_material(),
				"Both skys should have the same material.");
	}

	SUBCASE("Swapping materials between two skys") {
		Ref<Sky> sky2;
		sky2.instantiate();
		Ref<Material> material1;
		Ref<Material> material2;
		material1.instantiate();
		material2.instantiate();

		test_sky->set_material(material1);
		sky2->set_material(material2);
		CHECK(test_sky->get_material() == material1);
		CHECK(sky2->get_material() == material2);

		// Do the swap.
		Ref<Material> temp = test_sky->get_material();
		test_sky->set_material(sky2->get_material());
		sky2->set_material(temp);

		CHECK(test_sky->get_material() == material2);
		CHECK(sky2->get_material() == material1);
	}
}

TEST_CASE("[SceneTree][Sky] Invalid radiance size handling") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	// Attempt to set an invalid radiance size.
	ERR_PRINT_OFF;
	test_sky->set_radiance_size(Sky::RADIANCE_SIZE_MAX);
	ERR_PRINT_ON;

	// Verify that the radiance size remains unchanged.
	CHECK(test_sky->get_radiance_size() == Sky::RADIANCE_SIZE_256);
}

TEST_CASE("[SceneTree][Sky] Process mode variations") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	// Test all process modes.
	const Sky::ProcessMode process_modes[] = {
		Sky::PROCESS_MODE_AUTOMATIC,
		Sky::PROCESS_MODE_QUALITY,
		Sky::PROCESS_MODE_INCREMENTAL,
		Sky::PROCESS_MODE_REALTIME
	};

	for (Sky::ProcessMode mode : process_modes) {
		test_sky->set_process_mode(mode);
		CHECK(test_sky->get_process_mode() == mode);
	}
}

TEST_CASE("[SceneTree][Sky] Radiance size variations") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	// Test all radiance sizes except MAX.
	const Sky::RadianceSize radiance_sizes[] = {
		Sky::RADIANCE_SIZE_32,
		Sky::RADIANCE_SIZE_64,
		Sky::RADIANCE_SIZE_128,
		Sky::RADIANCE_SIZE_256,
		Sky::RADIANCE_SIZE_512,
		Sky::RADIANCE_SIZE_1024,
		Sky::RADIANCE_SIZE_2048
	};

	for (Sky::RadianceSize size : radiance_sizes) {
		test_sky->set_radiance_size(size);
		CHECK(test_sky->get_radiance_size() == size);
	}
}

TEST_CASE("[SceneTree][Sky] Null material handling") {
	Ref<Sky> test_sky;
	test_sky.instantiate();

	SUBCASE("Setting null material") {
		test_sky->set_material(Ref<Material>());
		CHECK(test_sky->get_material().is_null());
	}

	SUBCASE("Overwriting existing material with null") {
		Ref<Material> material;
		material.instantiate();
		test_sky->set_material(material);
		test_sky->set_material(Ref<Material>());

		CHECK(test_sky->get_material().is_null());
	}
}

TEST_CASE("[SceneTree][Sky] RID generation") {
	Ref<Sky> test_sky;
	test_sky.instantiate();
	// Check validity.
	CHECK(!test_sky->get_rid().is_valid());
}

TEST_CASE("[SceneTree][Sky] Atmosphere deterministic fixed states") {
	Ref<AtmosphereSkyMaterial> atmosphere;
	atmosphere.instantiate();
	atmosphere->set_latitude(35.0f);
	atmosphere->set_day_of_year(172);

	SUBCASE("Noon, golden hour, horizon, twilight, and moonlit night are finite") {
		const float states[] = { 12.0f, 18.0f, 19.2f, 20.0f, 0.0f };
		for (float time : states) {
			atmosphere->set_time_of_day(time);
			CHECK(atmosphere->get_sun_direction().is_finite());
			CHECK(atmosphere->get_moon_direction().is_finite());
			const Color sun_color = atmosphere->get_sun_color();
			CHECK(Math::is_finite(sun_color.r));
			CHECK(Math::is_finite(sun_color.g));
			CHECK(Math::is_finite(sun_color.b));
		}
		atmosphere->set_time_of_day(12.0f);
		CHECK(atmosphere->get_sun_direction().y > 0.0f);
		atmosphere->set_time_of_day(0.0f);
		CHECK(atmosphere->get_sun_direction().y < 0.0f);
		CHECK(atmosphere->get_moonlit_night_floor() > 0.0f);
	}

	SUBCASE("Clock wrapping, deterministic state, and opposing ownership") {
		atmosphere->set_time_of_day(25.25f);
		CHECK(atmosphere->get_time_of_day() == doctest::Approx(1.25f));
		atmosphere->set_time_of_day(7.5f);
		const Vector3 first_sun = atmosphere->get_sun_direction();
		const Vector3 first_previous_sun = atmosphere->get_previous_sun_direction();
		const Vector3 first_moon = atmosphere->get_moon_direction();
		atmosphere->set_time_of_day(7.5f);
		CHECK(atmosphere->get_sun_direction().is_equal_approx(first_sun));
		CHECK(atmosphere->get_previous_sun_direction().is_equal_approx(first_sun));
		CHECK(first_previous_sun.is_finite());
		CHECK(atmosphere->get_moon_direction().is_equal_approx(first_moon));
		CHECK(atmosphere->get_sun_direction().dot(atmosphere->get_moon_direction()) < -0.999f);
	}

	SUBCASE("Clear, partial, and overcast values remain bounded") {
		atmosphere->set_cloud_seed(44);
		atmosphere->set_cloud_coverage(0.0f);
		CHECK(atmosphere->get_cloud_coverage() == 0.0f);
		atmosphere->set_cloud_coverage(0.5f);
		CHECK(atmosphere->get_cloud_coverage() == 0.5f);
		atmosphere->set_cloud_coverage(1.0f);
		CHECK(atmosphere->get_cloud_coverage() == 1.0f);
		atmosphere->set_cloud_density(2.0f);
		CHECK(atmosphere->get_cloud_density() == 1.0f);
	}

	SUBCASE("Art-direction controls round-trip and remain bounded") {
		atmosphere->set_day_brightness(20.0f);
		CHECK(atmosphere->get_day_brightness() == 16.0f);
		atmosphere->set_sun_disk_energy(-1.0f);
		CHECK(atmosphere->get_sun_disk_energy() == 0.0f);
		atmosphere->set_moon_disk_energy(300.0f);
		CHECK(atmosphere->get_moon_disk_energy() == 256.0f);
		atmosphere->set_twilight_duration(0.0f);
		CHECK(atmosphere->get_twilight_duration() == doctest::Approx(0.05f));
		const Color dawn(0.52f, 0.19f, 0.71f);
		const Color dusk(0.91f, 0.25f, 0.16f);
		const Color night(0.01f, 0.002f, 0.03f);
		const Color moonlight(0.55f, 0.63f, 0.94f);
		atmosphere->set_dawn_color(dawn);
		atmosphere->set_dusk_color(dusk);
		atmosphere->set_dark_night_sky_color(night);
		atmosphere->set_moonlight_color(moonlight);
		CHECK(atmosphere->get_dawn_color().is_equal_approx(dawn));
		CHECK(atmosphere->get_dusk_color().is_equal_approx(dusk));
		CHECK(atmosphere->get_dark_night_sky_color().is_equal_approx(night));
		CHECK(atmosphere->get_moonlight_color().is_equal_approx(moonlight));
	}

	SUBCASE("Dawn, dusk, night, and midnight wrap remain continuous") {
		const float transition_times[] = { 5.5f, 6.0f, 18.0f, 18.5f, 23.99f, 0.01f };
		for (float time : transition_times) {
			atmosphere->set_time_of_day(time);
			CHECK(atmosphere->get_sun_direction().is_finite());
			CHECK(atmosphere->get_moon_direction().is_finite());
			CHECK(Math::is_finite(atmosphere->get_sun_color().get_luminance()));
			CHECK(Math::is_finite(atmosphere->get_moon_color().get_luminance()));
		}
		atmosphere->set_time_of_day(23.99f);
		const Vector3 before_midnight = atmosphere->get_sun_direction();
		atmosphere->set_time_of_day(0.01f);
		CHECK(before_midnight.distance_to(atmosphere->get_sun_direction()) < 0.01f);
	}
}

TEST_CASE("[SceneTree][Sky] Atmosphere clock deterministic controls") {
	Ref<AtmosphereSkyMaterial> atmosphere;
	atmosphere.instantiate();
	AtmosphereSkyClock clock;
	clock.set_atmosphere(atmosphere);
	clock.set_starting_time(6.0f);
	clock.apply_initial_state();
	CHECK(clock.get_current_time() == doctest::Approx(6.0f));
	CHECK(atmosphere->get_time_of_day() == doctest::Approx(6.0f));
	clock.set_day_length(24.0f);
	CHECK(atmosphere->get_simulated_time() == doctest::Approx(6.0f));
	clock.advance(6.0f);
	CHECK(clock.get_current_time() == doctest::Approx(12.0f));
	clock.scrub(25.0f);
	CHECK(clock.get_current_time() == doctest::Approx(1.0f));
	CHECK(atmosphere->get_time_of_day() == doctest::Approx(1.0f));
	CHECK(atmosphere->get_simulated_time() == doctest::Approx(1.0f));
	clock.advance(8.0f);
	clock.scrub(1.0f);
	CHECK(atmosphere->get_simulated_time() == doctest::Approx(1.0f));
	clock.set_time_scale(-1.0f);
	clock.advance(2.0f);
	CHECK(clock.get_current_time() == doctest::Approx(23.0f));
	CHECK(atmosphere->get_simulated_time() == doctest::Approx(23.0f));
	clock.set_paused(true);
	clock.advance(6.0f);
	CHECK(clock.get_current_time() == doctest::Approx(23.0f));
	clock.set_paused(false);
	clock.reset();
	CHECK(clock.get_current_time() == doctest::Approx(6.0f));
	CHECK(atmosphere->get_simulated_time() == doctest::Approx(6.0f));

	clock.set_cloud_coverage(0.35f);
	clock.set_cloud_density(0.65f);
	clock.set_cloud_attenuation(0.75f);
	clock.set_cloud_scale(1.8f);
	clock.set_cloud_wind(Vector2(0.1f, -0.05f));
	CHECK(clock.get_cloud_coverage() == doctest::Approx(0.35f));
	CHECK(clock.get_cloud_density() == doctest::Approx(0.65f));
	CHECK(clock.get_cloud_attenuation() == doctest::Approx(0.75f));
	CHECK(clock.get_cloud_scale() == doctest::Approx(1.8f));
	CHECK(clock.get_cloud_wind().is_equal_approx(Vector2(0.1f, -0.05f)));
	clock.set_atmosphere(Ref<AtmosphereSkyMaterial>());
	CHECK(clock.get_cloud_coverage() == 0.0f);
	clock.set_cloud_coverage(0.9f); // Null-safe pass-throughs do not create state.
	clock.set_atmosphere(atmosphere);
	CHECK(clock.get_cloud_coverage() == doctest::Approx(0.35f));
}

} // namespace TestSky

#endif // _3D_DISABLED
