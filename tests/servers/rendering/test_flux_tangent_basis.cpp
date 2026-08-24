/**************************************************************************/
/*  test_flux_tangent_basis.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_flux_tangent_basis)

#include "core/math/basis.h"
#include "core/math/transform_3d.h"

#ifdef METAL_ENABLED
#include "servers/rendering/renderer_rd/flux/metal_flux_effect.h"
#endif

namespace TestFluxTangentBasis {

struct TangentFrame {
	Vector3 normal;
	Vector3 tangent;
	Vector3 binormal;
};

static uint32_t _pack_unorm16_pair(const Vector2 &p_value) {
	const uint32_t x = uint32_t(CLAMP(Math::round(p_value.x * 65535.0), 0.0, 65535.0));
	const uint32_t y = uint32_t(CLAMP(Math::round(p_value.y * 65535.0), 0.0, 65535.0));
	return x | (y << 16);
}

static Vector2 _decode_uint_to_vec2(uint32_t p_packed) {
	return Vector2(p_packed & 0xffffu, p_packed >> 16u) / 65535.0 * 2.0 - Vector2(1.0, 1.0);
}

static Vector3 _decode_oct(uint32_t p_packed) {
	return Vector3::octahedron_decode((_decode_uint_to_vec2(p_packed) + Vector2(1.0, 1.0)) * 0.5);
}

static uint32_t _pack_tangent(const Vector3 &p_tangent, float p_handedness) {
	const Vector2 oct = p_tangent.normalized().octahedron_encode();
	const Vector2 signed_encoding(oct.x * 2.0 - 1.0, (p_handedness < 0.0 ? -1.0 : 1.0) * oct.y);
	return _pack_unorm16_pair((signed_encoding + Vector2(1.0, 1.0)) * 0.5);
}

static TangentFrame _decode_uncompressed(uint32_t p_normal, uint32_t p_tangent) {
	TangentFrame frame;
	frame.normal = _decode_oct(p_normal);
	const Vector2 signed_tangent = _decode_uint_to_vec2(p_tangent);
	const Vector2 tangent_oct(signed_tangent.x, Math::abs(signed_tangent.y) * 2.0 - 1.0);
	frame.tangent = Vector3::octahedron_decode((tangent_oct + Vector2(1.0, 1.0)) * 0.5);
	frame.binormal = frame.normal.cross(frame.tangent).normalized() * (signed_tangent.y < 0.0 ? -1.0 : 1.0);
	return frame;
}

static TangentFrame _decode_compressed(const Vector3 &p_axis, float p_encoded_angle) {
	const float handedness = p_encoded_angle > 0.5 ? 1.0 : -1.0;
	const float angle = Math::abs(p_encoded_angle * 2.0 - 1.0) * Math::PI;
	const Basis tbn(p_axis.normalized(), angle);
	TangentFrame frame;
	frame.tangent = tbn.rows[0];
	frame.binormal = tbn.rows[1] * handedness;
	frame.normal = tbn.rows[2];
	return frame;
}

static TangentFrame _world_frame(const TangentFrame &p_object_frame, const Basis &p_world_from_object) {
	TangentFrame frame;
	frame.normal = p_world_from_object.inverse().transposed().xform(p_object_frame.normal).normalized();
	frame.tangent = p_world_from_object.xform(p_object_frame.tangent);
	frame.tangent = (frame.tangent - frame.normal * frame.normal.dot(frame.tangent)).normalized();
	const Vector3 transformed_binormal = p_world_from_object.xform(p_object_frame.binormal);
	const float handedness = frame.normal.cross(frame.tangent).dot(transformed_binormal) < 0.0 ? -1.0 : 1.0;
	frame.binormal = frame.normal.cross(frame.tangent).normalized() * handedness;
	return frame;
}

static bool _uv_derivative_frame(const Vector3 p_positions[3], const Vector2 p_uvs[3], const Vector3 &p_normal, TangentFrame &r_frame) {
	const Vector2 duv1 = p_uvs[1] - p_uvs[0];
	const Vector2 duv2 = p_uvs[2] - p_uvs[0];
	const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
	if (!Math::is_finite(determinant) || Math::is_zero_approx(determinant)) {
		return false;
	}
	Vector3 tangent = ((p_positions[1] - p_positions[0]) * duv2.y - (p_positions[2] - p_positions[0]) * duv1.y) / determinant;
	const Vector3 candidate_binormal = ((p_positions[2] - p_positions[0]) * duv1.x - (p_positions[1] - p_positions[0]) * duv2.x) / determinant;
	tangent = (tangent - p_normal * p_normal.dot(tangent)).normalized();
	if (!tangent.is_finite() || tangent.is_zero_approx() || !candidate_binormal.is_finite() || candidate_binormal.is_zero_approx()) {
		return false;
	}
	r_frame.normal = p_normal.normalized();
	r_frame.tangent = tangent;
	const float handedness = r_frame.normal.cross(tangent).dot(candidate_binormal) < 0.0 ? -1.0 : 1.0;
	r_frame.binormal = r_frame.normal.cross(tangent).normalized() * handedness;
	return true;
}

static Vector3 _apply_open_gl_normal(const TangentFrame &p_frame, const Vector3 &p_tangent_normal) {
	return (p_frame.tangent * p_tangent_normal.x + p_frame.binormal * p_tangent_normal.y + p_frame.normal * p_tangent_normal.z).normalized();
}

TEST_CASE("[Rendering][Flux][TangentBasis] uncompressed authored tangents preserve both handedness signs and positive Y") {
	const uint32_t packed_normal = _pack_unorm16_pair(Vector3(0.0, 0.0, 1.0).octahedron_encode());
	const TangentFrame positive = _decode_uncompressed(packed_normal, _pack_tangent(Vector3(1.0, 0.0, 0.0), 1.0));
	const TangentFrame negative = _decode_uncompressed(packed_normal, _pack_tangent(Vector3(1.0, 0.0, 0.0), -1.0));
	CHECK_GT(positive.normal.dot(Vector3(0.0, 0.0, 1.0)), 0.999999);
	CHECK_GT(positive.tangent.dot(Vector3(1.0, 0.0, 0.0)), 0.999999);
	CHECK_GT(_apply_open_gl_normal(positive, Vector3(0.0, 1.0, 0.0)).y, 0.999);
	CHECK_LT(_apply_open_gl_normal(negative, Vector3(0.0, 1.0, 0.0)).y, -0.999);
	CHECK(_apply_open_gl_normal(positive, Vector3(0.0, 0.0, 1.0)).is_equal_approx(positive.normal));
}

TEST_CASE("[Rendering][Flux][TangentBasis] compressed axis-angle records reproduce authored flat frames") {
	for (const float handedness : { -1.0f, 1.0f }) {
		Basis authored;
		authored.rows[0] = Vector3(1.0, 0.0, 0.0);
		authored.rows[1] = Vector3(0.0, 1.0, 0.0);
		authored.rows[2] = Vector3(0.0, 0.0, 1.0);
		Vector3 axis;
		real_t angle = 0.0;
		authored.get_axis_angle(axis, angle);
		const float encoded_angle = handedness < 0.0 ? CLAMP((1.0 - angle / Math::PI) * 0.5, 0.0, 0.49999) : CLAMP((angle / Math::PI) * 0.5 + 0.5, 0.500008, 1.0);
		const TangentFrame decoded = _decode_compressed(axis, encoded_angle);
		CHECK_GT(decoded.normal.dot(Vector3(0.0, 0.0, 1.0)), 0.999);
		CHECK_GT(decoded.tangent.dot(Vector3(1.0, 0.0, 0.0)), 0.999);
		CHECK((decoded.binormal.dot(Vector3(0.0, 1.0, 0.0)) < 0.0 ? -1.0 : 1.0) == handedness);
	}
}

TEST_CASE("[Rendering][Flux][TangentBasis] mirrored UV fallback and nonuniform transforms remain finite and orthogonal") {
	const Vector3 positions[3] = { Vector3(0.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0) };
	const Vector2 mirrored_uvs[3] = { Vector2(0.0, 0.0), Vector2(-1.0, 0.0), Vector2(0.0, 1.0) };
	TangentFrame fallback;
	REQUIRE(_uv_derivative_frame(positions, mirrored_uvs, Vector3(0.0, 0.0, 1.0), fallback));
	CHECK_LT(fallback.tangent.x, -0.999);
	CHECK_GT(fallback.binormal.y, 0.999);

	Basis transform(Vector3(0.0, 1.0, 0.0), 0.61);
	transform = transform.scaled(Vector3(2.0, 0.5, 3.0));
	const TangentFrame transformed = _world_frame(fallback, transform);
	CHECK(transformed.normal.is_finite());
	CHECK(transformed.tangent.is_finite());
	CHECK(transformed.binormal.is_finite());
	CHECK(Math::is_zero_approx(transformed.normal.dot(transformed.tangent)));
	CHECK(Math::is_zero_approx(transformed.normal.dot(transformed.binormal)));
	CHECK(Math::is_zero_approx(transformed.tangent.dot(transformed.binormal)));
	CHECK(_apply_open_gl_normal(transformed, Vector3(0.0, 0.0, 1.0)).is_equal_approx(transformed.normal));
}

TEST_CASE("[Rendering][Flux][TangentBasis] tangentless degenerate UV fallback fails closed to the geometric normal") {
	const Vector3 positions[3] = { Vector3(0.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0) };
	const Vector2 degenerate_uvs[3] = { Vector2(0.0, 0.0), Vector2(0.0, 0.0), Vector2(0.0, 0.0) };
	TangentFrame fallback;
	CHECK_FALSE(_uv_derivative_frame(positions, degenerate_uvs, Vector3(0.0, 0.0, 1.0), fallback));
	const Vector3 geometric_normal(0.0, 0.0, 1.0);
	CHECK(geometric_normal.is_finite());
}

#ifdef METAL_ENABLED
TEST_CASE("[Rendering][Flux][TangentBasis] surface tangent availability is an explicit ABI input") {
	RendererRD::MetalFluxEffect::Surface surface;
	CHECK_FALSE(surface.has_tangents);
	surface.has_normals = true;
	surface.has_tangents = true;
	surface.normal_stride = sizeof(uint32_t) * 2;
	CHECK(surface.has_tangents);
	CHECK_EQ(surface.normal_stride, 8);
}
#endif

} // namespace TestFluxTangentBasis
