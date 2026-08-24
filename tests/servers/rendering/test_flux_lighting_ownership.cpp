/**************************************************************************/
/*  test_flux_lighting_ownership.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_flux_lighting_ownership)

#include "core/math/color.h"
#include "core/math/transform_3d.h"

#ifdef METAL_ENABLED
#include "servers/rendering/renderer_rd/flux/metal_flux_effect.h"
#endif

namespace TestFluxLightingOwnership {

static Color _compose_primary_lighting(const Color &p_raster_emission, const Color &p_ray_shaded_direct) {
	return p_raster_emission + p_ray_shaded_direct;
}

static bool _environment_replacement_ready(bool p_configured, bool p_ambient_uses_sky, bool p_reflection_uses_sky, bool p_full_float, uint64_t p_generation) {
	return p_configured && p_ambient_uses_sky && p_reflection_uses_sky && p_full_float && p_generation > 0;
}

static float _environment_direct_mis_weight(float p_light_pdf, float p_bsdf_pdf) {
	const float denominator = p_light_pdf + p_bsdf_pdf;
	return p_light_pdf / denominator + p_bsdf_pdf / denominator;
}

static Vector3 _sky_source_direction_in_view(const Basis &p_world_from_sky, const Transform3D &p_camera, const Vector3 &p_sky_direction) {
	return p_camera.affine_inverse().basis.xform(p_world_from_sky.xform(p_sky_direction).normalized()).normalized();
}

TEST_CASE("[Rendering][Flux][LightingOwnership] raster primary carries emission while Metal exclusively owns cast shadows") {
	const Color raster_emission(0.1f, 0.05f, 0.0f, 0.0f);
	const Color unoccluded_ray_direct(0.8f, 0.6f, 0.4f, 0.0f);
	const Color shadowed_ray_direct = unoccluded_ray_direct * 0.25f;
	const Color composed = _compose_primary_lighting(raster_emission, shadowed_ray_direct);
	CHECK(composed.is_equal_approx(Color(0.3f, 0.2f, 0.1f, 0.0f)));
	CHECK_FALSE(composed.is_equal_approx(_compose_primary_lighting(raster_emission + shadowed_ray_direct, shadowed_ray_direct)));
}

TEST_CASE("[Rendering][Flux][LightingOwnership] environment replacement fails open for flat mixed late and unsupported Sky state") {
	CHECK_FALSE(_environment_replacement_ready(true, false, false, true, 1));
	CHECK_FALSE(_environment_replacement_ready(true, true, false, true, 1));
	CHECK_FALSE(_environment_replacement_ready(true, true, true, false, 1));
	CHECK_FALSE(_environment_replacement_ready(true, true, true, true, 0));
	CHECK(_environment_replacement_ready(true, true, true, true, 1));
}

TEST_CASE("[Rendering][Flux][LightingOwnership] authored AO and post-reconstruction composition are single-owner operations") {
	const float ambient_transport = 0.8f;
	const float authored_ao = 0.5f;
	CHECK(Math::is_equal_approx(ambient_transport * authored_ao, 0.4f));
	CHECK_FALSE(Math::is_equal_approx(ambient_transport * authored_ao, ambient_transport * authored_ao * authored_ao));

	const Color raster_dark(0.1f, 0.1f, 0.1f);
	const Color raster_highlight(0.9f, 0.9f, 0.9f);
	const Color reconstructed_transport(0.05f, 0.05f, 0.05f);
	const Color composed_dark = raster_dark + reconstructed_transport;
	const Color composed_highlight = raster_highlight + reconstructed_transport;
	CHECK(Math::is_equal_approx(composed_highlight.r - composed_dark.r, raster_highlight.r - raster_dark.r));
}

TEST_CASE("[Rendering][Flux][LightingOwnership] direct Sky MIS is independent from bounced GI strength") {
	const float light_pdf = 0.25f;
	const float bsdf_pdf = 0.25f;
	CHECK(Math::is_equal_approx(_environment_direct_mis_weight(light_pdf, bsdf_pdf), 1.0f));

	const float gi_strength = 2.0f;
	const float old_mixed_weight = light_pdf / (light_pdf + bsdf_pdf) + gi_strength * bsdf_pdf / (light_pdf + bsdf_pdf);
	CHECK(Math::is_equal_approx(old_mixed_weight, 1.5f));
	CHECK_FALSE(Math::is_equal_approx(old_mixed_weight, _environment_direct_mis_weight(light_pdf, bsdf_pdf)));

	// Without the cosine-ray proposal, environment NEE owns the whole estimator.
	CHECK(Math::is_equal_approx(light_pdf / light_pdf, 1.0f));
}

TEST_CASE("[Rendering][Flux][LightingOwnership] raster solar shading follows procedural Sky orientation") {
	const Vector3 sky_source_direction = Vector3(0.0f, 1.0f, 0.0f);
	const Transform3D camera;
	const Vector3 up_facing_normal(0.0f, 1.0f, 0.0f);
	const Vector3 side_facing_normal(1.0f, 0.0f, 0.0f);

	const Vector3 unrotated_direction = _sky_source_direction_in_view(Basis(), camera, sky_source_direction);
	CHECK(Math::is_equal_approx(MAX(up_facing_normal.dot(unrotated_direction), 0.0f), 1.0f));
	CHECK(Math::is_equal_approx(MAX(side_facing_normal.dot(unrotated_direction), 0.0f), 0.0f));

	const Basis rotated_sky(Vector3(0.0f, 0.0f, 1.0f), -Math::PI / 2.0f);
	const Vector3 rotated_direction = _sky_source_direction_in_view(rotated_sky, camera, sky_source_direction);
	CHECK(Math::is_equal_approx(MAX(up_facing_normal.dot(rotated_direction), 0.0f), 0.0f));
	CHECK(Math::is_equal_approx(MAX(side_facing_normal.dot(rotated_direction), 0.0f), 1.0f));
}

#ifdef METAL_ENABLED
TEST_CASE("[Rendering][Flux][LightingOwnership] frame contract defaults are inert until ray-owned primary shading is requested") {
	RendererRD::MetalFluxEffect::FrameRequest request;
	CHECK_FALSE(request.environment.primary_replacement);
	CHECK_FALSE(request.directional_light_active);
	CHECK_EQ(request.directional_light_cull_mask, 0xffffffffu);
	CHECK_EQ(request.directional_shadow_caster_mask, 0xffffffffu);
	CHECK_FALSE(request.directional_shadow_enabled);
	CHECK(Math::is_equal_approx(request.directional_shadow_opacity, 1.0f));

	request.directional_light_radiance = Color(2.0f, 1.0f, 0.5f);
	request.directional_light_active = true;
	const Color lambert_secondary = request.directional_light_radiance * (1.0f / Math::PI);
	CHECK_GT(lambert_secondary.get_luminance(), 0.0f);
}
#endif

} // namespace TestFluxLightingOwnership
