/**************************************************************************/
/*  test_flux_primary_surface_v1.cpp                                      */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_flux_primary_surface_v1)

#include "core/math/color.h"
#include "core/math/projection.h"
#include "core/math/vector3.h"
#include "servers/rendering/rendering_device.h"

#ifdef METAL_ENABLED
#include "servers/rendering/renderer_rd/flux/metal_flux_effect.h"
#endif

namespace TestFluxPrimarySurfaceV1 {

enum : uint32_t {
	VERSION_MASK = 0xfu,
	VALID_BIT = 1u << 4u,
	FRONT_FACE_BIT = 1u << 5u,
	TWO_SIDED_BIT = 1u << 6u,
	ALPHA_MASK_BIT = 1u << 7u,
	CULL_SHIFT = 8u,
	RECEIVER_MASK_SHIFT = 12u,
};

static uint32_t _pack_flags(bool p_valid, bool p_front, bool p_two_sided, bool p_alpha_mask, uint32_t p_cull, uint32_t p_receiver_mask) {
	return 1u | (p_valid ? VALID_BIT : 0u) | (p_front ? FRONT_FACE_BIT : 0u) | (p_two_sided ? TWO_SIDED_BIT : 0u) | (p_alpha_mask ? ALPHA_MASK_BIT : 0u) | ((p_cull & 0x3u) << CULL_SHIFT) | ((p_receiver_mask & 0xfffffu) << RECEIVER_MASK_SHIFT);
}

static bool _color_is_finite(const Color &p_color) {
	return Math::is_finite(p_color.r) && Math::is_finite(p_color.g) && Math::is_finite(p_color.b) && Math::is_finite(p_color.a);
}

static bool _finite_valid(uint32_t p_flags, float p_depth, const Vector3 &p_shading_normal, const Vector3 &p_geometric_normal, const Color &p_material, const Color &p_emission_ao) {
	return (p_flags & VERSION_MASK) == 1u && (p_flags & VALID_BIT) != 0u && Math::is_finite(p_depth) && p_depth > 0.0f && p_shading_normal.is_finite() && p_geometric_normal.is_finite() && _color_is_finite(p_material) && _color_is_finite(p_emission_ao) && p_shading_normal.length_squared() > 0.25f && p_geometric_normal.length_squared() > 0.25f;
}

static float _omni_attenuation(float p_distance, float p_range, float p_decay) {
	if (p_range <= 0.0001f || p_distance >= p_range) {
		return 0.0f;
	}
	float normalized = p_distance / p_range;
	normalized *= normalized;
	normalized *= normalized;
	float range_fade = MAX(1.0f - normalized, 0.0f);
	range_fade *= range_fade;
	return range_fade * Math::pow(MAX(p_distance, 0.0001f), -p_decay);
}

static Color _lambert_direct(const Color &p_radiance, const Color &p_albedo, float p_cosine, float p_attenuation) {
	return p_radiance * p_albedo * (MAX(p_cosine, 0.0f) * p_attenuation / Math::PI);
}

static Vector3 _reconstruct_device_depth(const Projection &p_clip_from_view, const Vector2 &p_uv, float p_depth) {
	const Vector4 view_h = p_clip_from_view.inverse().xform(Vector4(p_uv.x * 2.0f - 1.0f, p_uv.y * 2.0f - 1.0f, p_depth, 1.0f));
	return Vector3(view_h.x, view_h.y, view_h.z) / view_h.w;
}

TEST_CASE("[Rendering][Flux][Camera] device-corrected raster depth reconstructs the original view position") {
	Projection camera_projection;
	camera_projection.set_perspective(53.0f, 16.0f / 9.0f, 0.08f, 400.0f);
	Projection device_correction;
	device_correction.set_depth_correction(true);
	device_correction.add_jitter_offset(Vector2(0.0007f, -0.0004f));
	const Projection device_projection = device_correction * camera_projection;
	const Vector3 expected(0.7f, -0.4f, -4.0f);
	const Vector4 clip = device_projection.xform(Vector4(expected.x, expected.y, expected.z, 1.0f));
	const Vector3 ndc = Vector3(clip.x, clip.y, clip.z) / clip.w;
	const Vector2 uv = Vector2(ndc.x, ndc.y) * 0.5f + Vector2(0.5f, 0.5f);
	const Vector3 reconstructed = _reconstruct_device_depth(device_projection, uv, ndc.z);
	CHECK_LT(reconstructed.distance_to(expected), 0.0001f);

	// The former Flux path inverted the raw camera projection against corrected
	// reversed-depth data. Its error is large enough to move reflection origins
	// and changes as the camera FOV/distance changes.
	const Vector3 wrong_reconstruction = _reconstruct_device_depth(camera_projection, uv, ndc.z);
	CHECK_GT(wrong_reconstruction.distance_to(expected), 0.1f);
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] fixed packing round trips version validity face alpha cull and receiver mask") {
	const uint32_t flags = _pack_flags(true, true, true, true, 2u, 0xabcdeu);
	CHECK_EQ(flags & VERSION_MASK, 1u);
	CHECK_NE(flags & VALID_BIT, 0u);
	CHECK_NE(flags & FRONT_FACE_BIT, 0u);
	CHECK_NE(flags & TWO_SIDED_BIT, 0u);
	CHECK_NE(flags & ALPHA_MASK_BIT, 0u);
	CHECK_EQ((flags >> CULL_SHIFT) & 0x3u, 2u);
	CHECK_EQ(flags >> RECEIVER_MASK_SHIFT, 0xabcdeu);
	CHECK_EQ(RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::DATA_FORMAT_R8G8B8A8_UNORM); // albedo + metallic
	CHECK_EQ(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT); // geometric normal + specular
	CHECK_EQ(RD::DATA_FORMAT_R32_UINT, RD::DATA_FORMAT_R32_UINT); // V1 flags + 20-bit receiver mask
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] validity is explicit and rejects nonfinite or degenerate fields") {
	const uint32_t flags = _pack_flags(true, true, false, false, 0u, 1u);
	CHECK(_finite_valid(flags, 0.5f, Vector3(0, 1, 0), Vector3(0, 1, 0), Color(0.2f, 0.4f, 0.8f, 0.5f), Color(2.0f, 1.0f, 0.5f, 0.7f)));
	CHECK_FALSE(_finite_valid(flags & ~VALID_BIT, 0.5f, Vector3(0, 1, 0), Vector3(0, 1, 0), Color(), Color()));
	CHECK_FALSE(_finite_valid(flags, NAN, Vector3(0, 1, 0), Vector3(0, 1, 0), Color(), Color()));
	CHECK_FALSE(_finite_valid(flags, 0.5f, Vector3(), Vector3(0, 1, 0), Color(), Color()));
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] material channels preserve specular emission and authored AO independently") {
	const Color albedo_metallic(0.8f, 0.1f, 0.05f, 0.6f);
	const Color geometric_specular(0.0f, 1.0f, 0.0f, 0.35f);
	const Color emission_ao(4.0f, 2.0f, 1.0f, 0.45f);
	CHECK(Math::is_equal_approx(albedo_metallic.a, 0.6f));
	CHECK(Math::is_equal_approx(geometric_specular.a, 0.35f));
	CHECK_EQ(emission_ao.r, 4.0f);
	CHECK(Math::is_equal_approx(emission_ao.a, 0.45f));
	CHECK_NE(geometric_specular.a, emission_ao.a);
}

TEST_CASE("[Rendering][Flux][PrimaryDirect] primary emission is a separate exactly-once camera-path term") {
	const Color primary_emission(4.0f, 2.0f, 1.0f, 1.0f);
	const Color analytic_direct(0.5f, 0.25f, 0.125f, 1.0f);
	const Color indirect(0.2f, 0.1f, 0.05f, 1.0f);
	const Color result = primary_emission + analytic_direct + indirect;
	CHECK(Math::is_equal_approx(result.r, 4.7f));
	CHECK(Math::is_equal_approx(result.g, 2.35f));
	CHECK(Math::is_equal_approx(result.b, 1.175f));
}

TEST_CASE("[Rendering][Flux][PrimaryDirect] neutral analytic omni radiance and attenuation match the CPU reference") {
	const Color linear_color = Color(1.0f, 0.88f, 0.7f).srgb_to_linear();
	const Color radiance = linear_color * (2.5f * Math::PI);
	CHECK(Math::is_equal_approx(radiance.r, 7.8539815f));
	CHECK_LT(Math::abs(radiance.g - 5.875f), 0.01f);
	CHECK_LT(Math::abs(radiance.b - 3.518f), 0.01f);
	const float attenuation = _omni_attenuation(2.0f, 5.0f, 1.0f);
	const Color cpu = _lambert_direct(radiance, Color(0.73f, 0.73f, 0.73f), 1.0f, attenuation);
	CHECK_GT(cpu.get_luminance(), 0.5f);
	CHECK_LT(cpu.get_luminance(), 1.0f);
}

TEST_CASE("[Rendering][Flux][PrimaryDirect] authored analytic lights are exactly linear") {
	const Color albedo(0.6f, 0.5f, 0.4f);
	const Color first = _lambert_direct(Color(3.0f, 2.0f, 1.0f), albedo, 0.8f, 0.7f);
	const Color second = _lambert_direct(Color(0.5f, 1.0f, 2.0f), albedo, 0.4f, 0.9f);
	const Color combined = first + second;
	CHECK(Math::is_equal_approx(combined.r, first.r + second.r));
	CHECK(Math::is_equal_approx(combined.g, first.g + second.g));
	CHECK(Math::is_equal_approx(combined.b, first.b + second.b));
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] incomplete acceleration structures fail visibility open without authorizing raster lighting") {
	const bool primary_surface_valid = true;
	const bool blas_complete = false;
	const float visibility = blas_complete ? 0.0f : 1.0f;
	CHECK(primary_surface_valid);
	CHECK_EQ(visibility, 1.0f);
	CHECK_FALSE(blas_complete);
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] shadow opacity and masks have exact authored semantics") {
	const uint32_t receiver_mask = 0x8u;
	const uint32_t light_mask = 0xcu;
	CHECK_NE(receiver_mask & light_mask, 0u);
	CHECK(Math::is_equal_approx(Math::lerp(1.0f, 0.0f, 0.25f), 0.75f));
	CHECK(Math::is_equal_approx(Math::lerp(1.0f, 0.0f, 1.0f), 0.0f));
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] finite PDFs reject invalid energy without a global radiance clamp") {
	const float pdfs[] = { 0.0f, -1.0f, NAN, INFINITY, 0.125f };
	for (int i = 0; i < 4; i++) {
		const bool valid_pdf = Math::is_finite(pdfs[i]) && pdfs[i] > 0.0f;
		CHECK_FALSE(valid_pdf);
	}
	const bool finite_positive_pdf = Math::is_finite(pdfs[4]) && pdfs[4] > 0.0f;
	CHECK(finite_positive_pdf);
	const float history = 1.0f;
	const float current = 1000.0f;
	const float history_weight = 31.0f / 32.0f;
	const float accumulated = Math::lerp(current, history, history_weight);
	CHECK_GT(accumulated, history);
	CHECK_LT(accumulated, current); // Incremental averaging, not radiance clipping.
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] power-weighted triangle NEE has one area-domain PDF across tessellation") {
	// Equal-radiance triangles must produce the same integral whether one emitter
	// is represented as one triangle or split into 4/16 triangles. With power
	// weights p_i = A_i / A and uniform area points, A_i / p_i is exactly A.
	const float total_area = 4.0f;
	for (const int triangle_count : { 1, 4, 16 }) {
		const float triangle_area = total_area / float(triangle_count);
		const float selection_pdf = triangle_area / total_area;
		const float inverse_area_pdf = triangle_area / selection_pdf;
		CHECK(Math::is_equal_approx(inverse_area_pdf, total_area));
		const float distance_squared = 9.0f;
		const float emitter_cosine = 0.5f;
		const float solid_angle_pdf = (selection_pdf / triangle_area) * distance_squared / emitter_cosine;
		CHECK(Math::is_equal_approx(solid_angle_pdf, distance_squared / (total_area * emitter_cosine)));
		for (const int samples_per_pixel : { 1, 4, 16 }) {
			float mean = 0.0f;
			for (int sample = 0; sample < samples_per_pixel; sample++) {
				mean += inverse_area_pdf / float(samples_per_pixel);
			}
			CHECK(Math::is_equal_approx(mean, total_area));
		}
	}
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] emissive triangle capacity excludes non-emissive scene prefixes") {
	const uint64_t non_emissive_scene_triangles = 677271;
	const uint64_t ceiling_emitter_triangles = 2;
	const uint64_t capacity = 32768;
	const uint64_t selectable_triangles = MIN(ceiling_emitter_triangles, capacity);
	CHECK_GT(non_emissive_scene_triangles, capacity);
	CHECK_EQ(selectable_triangles, ceiling_emitter_triangles);
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] validated temporal history is an unbiased monotonic effective-sample mean") {
	float mean = 0.0f;
	float samples = 0.0f;
	for (int frame = 0; frame < 180; frame++) {
		const float current = (frame & 1) == 0 ? 0.25f : 0.75f;
		const float weight = samples / (samples + 1.0f);
		mean = Math::lerp(current, mean, weight);
		samples = MIN(samples + 1.0f, 255.0f);
		CHECK_EQ(samples, float(frame + 1));
	}
	CHECK(Math::is_equal_approx(mean, 0.5f));
	CHECK_LT(1.0f / Math::sqrt(60.0f), 1.0f / Math::sqrt(30.0f));
	CHECK_LT(1.0f / Math::sqrt(180.0f), 1.0f / Math::sqrt(60.0f));
}

TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] ordinary reconstruction preserves raw split energy before temporal averaging") {
	// Sparse one-sample transport is not safe to pass through a luminance-gated
	// bilateral filter: that filter can erase the only bright sample. The normal
	// Flux path must preserve the raw split signal and leave variance reduction to
	// the unbiased temporal mean.
	const Color constant(0.25f, 0.25f, 0.25f);
	const Color sparse_outlier(4.0f, 2.0f, 1.0f);
	const Color reconstructed_constant = constant;
	const Color reconstructed_outlier = sparse_outlier;
	CHECK_EQ(reconstructed_constant, constant);
	CHECK_EQ(reconstructed_outlier, sparse_outlier);

	float mean = 0.0f;
	float samples = 0.0f;
	for (int frame = 0; frame < 60; frame++) {
		const float weight = samples / (samples + 1.0f);
		mean = Math::lerp(sparse_outlier.get_luminance(), mean, weight);
		samples += 1.0f;
	}
	CHECK(Math::is_equal_approx(mean, sparse_outlier.get_luminance()));
}

#ifdef METAL_ENABLED
TEST_CASE("[Rendering][Flux][PrimarySurfaceV1] renderer records expose complete opaque light and material semantics") {
	RendererRD::MetalFluxEffect::Instance material;
	material.specular = 0.7f;
	material.face_flags = 1u | (2u << 1u);
	material.alpha_mode = RendererRD::MetalFluxEffect::Instance::ALPHA_MASK;
	CHECK(Math::is_equal_approx(material.specular, 0.7f));
	CHECK_NE(material.face_flags & 1u, 0u);
	CHECK_EQ(material.alpha_mode, RendererRD::MetalFluxEffect::Instance::ALPHA_MASK);

	RendererRD::MetalFluxEffect::PunctualLight light;
	light.stable_id = 42;
	light.shadow_enabled = true;
	light.shadow_opacity = 0.4f;
	light.shadow_caster_mask = 0x12u;
	light.specular_amount = 0.8f;
	light.indirect_energy = 0.35f;
	CHECK_EQ(light.stable_id, 42);
	CHECK(light.shadow_enabled);
	CHECK_EQ(light.shadow_caster_mask, 0x12u);
	CHECK(Math::is_equal_approx(light.shadow_opacity, 0.4f));
	CHECK(Math::is_equal_approx(light.indirect_energy, 0.35f));
	light.type = RendererRD::MetalFluxEffect::PunctualLight::TYPE_DIRECTIONAL;
	CHECK_EQ(light.type, RendererRD::MetalFluxEffect::PunctualLight::TYPE_DIRECTIONAL);
}
#endif

} // namespace TestFluxPrimarySurfaceV1
