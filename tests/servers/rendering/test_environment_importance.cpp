/**************************************************************************/
/*  test_environment_importance.cpp                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_environment_importance)

#include "servers/rendering/path_tracing/environment_importance.h"

namespace TestEnvironmentImportance {

using namespace RendererPathTracing;

static EnvironmentImportanceMetadata metadata(bool p_array = false) {
	EnvironmentImportanceMetadata result;
	result.source_id = 41;
	result.sample_id = 41;
	result.original_resource_id = 77;
	result.generation = 3;
	result.width = 32;
	result.height = 32;
	result.border = 0.125f;
	result.array_layout = p_array;
	return result;
}

TEST_CASE("[PathTracing][EnvironmentImportance] Octahedral encode decode and border mapping are finite") {
	const Vector3 directions[] = { Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0.2f, -0.7f, 0.5f).normalized() };
	for (const Vector3 &direction : directions) {
		const Vector2 oct = environment_oct_encode(direction);
		CHECK(direction.dot(environment_oct_decode(oct)) > 0.999f);
		const Vector2 bordered = environment_oct_apply_border(oct, 0.125f);
		CHECK(bordered.x >= 0.125f);
		CHECK(bordered.y >= 0.125f);
	}
}

TEST_CASE("[PathTracing][EnvironmentImportance] Oct texel solid angles are positive symmetric and approximate 4pi") {
	const EnvironmentImportanceMetadata value = metadata();
	double total = 0.0;
	for (uint32_t y = 0; y < value.height; y++) for (uint32_t x = 0; x < value.width; x++) total += environment_oct_texel_solid_angle(x, y, value);
	CHECK(total == doctest::Approx(Math::TAU * 2.0).epsilon(0.06));
	CHECK(environment_oct_texel_solid_angle(8, 8, value) == doctest::Approx(environment_oct_texel_solid_angle(23, 23, value)));
}

TEST_CASE("[PathTracing][EnvironmentImportance] Black constant and broad peak distributions have finite PDFs") {
	EnvironmentImportanceMetadata value = metadata();
	Vector<float> black;
	black.resize(value.width * value.height);
	EnvironmentImportanceDistribution distribution;
	CHECK_FALSE(distribution.build(value, black));
	Vector<float> constant;
	constant.resize(value.width * value.height);
	for (float &weight : constant) weight = 1.0f;
	CHECK(distribution.build(value, constant));
	CHECK(distribution.sample(0.3f).valid);
	constant.write[value.width * 16 + 16] = 100.0f;
	CHECK(distribution.build(value, constant));
	const EnvironmentImportanceSample sample = distribution.sample(0.5f);
	CHECK(sample.valid);
	CHECK(Math::is_finite(sample.pdf_solid_angle));
}

TEST_CASE("[PathTracing][EnvironmentImportance] Distribution identity excludes orientation while history includes it") {
	EnvironmentImportanceMetadata first = metadata(false);
	EnvironmentImportanceMetadata second = first;
	second.world_from_radiance = Basis(Vector3(0, 1, 0), 0.5f);
	CHECK_EQ(first.distribution_key(), second.distribution_key());
	CHECK_NE(first.history_key(), second.history_key());
	Vector<float> luminance;
	luminance.resize(first.width * first.height);
	luminance.write[16 * first.width + 22] = 10.0f;
	EnvironmentImportanceDistribution first_distribution;
	EnvironmentImportanceDistribution rotated_distribution;
	CHECK(first_distribution.build(first, luminance));
	CHECK(rotated_distribution.build(second, luminance));
	const EnvironmentImportanceSample first_sample = first_distribution.sample(0.5f);
	const EnvironmentImportanceSample rotated_sample = rotated_distribution.sample(0.5f);
	CHECK(first_sample.valid);
	CHECK(rotated_sample.valid);
	CHECK_EQ(first_sample.texel_index, rotated_sample.texel_index);
	CHECK(rotated_sample.world_direction.distance_to(second.world_from_radiance.xform(first_sample.local_direction).normalized()) < 0.0001f);
	EnvironmentImportanceMetadata recreated = first;
	recreated.original_resource_id++;
	CHECK_NE(first.distribution_key(), recreated.distribution_key());
	EnvironmentImportanceMetadata array = first;
	array.array_layout = true;
	CHECK_NE(first.distribution_key(), array.distribution_key());
	EnvironmentImportanceMetadata content = first;
	content.generation++;
	CHECK_NE(first.distribution_key(), content.distribution_key());
	EnvironmentImportanceMetadata resized_width = first;
	resized_width.width++;
	CHECK_NE(first.distribution_key(), resized_width.distribution_key());
	EnvironmentImportanceMetadata resized_height = first;
	resized_height.height++;
	CHECK_NE(first.distribution_key(), resized_height.distribution_key());
	EnvironmentImportanceMetadata bordered = first;
	bordered.border = 0.0625f;
	CHECK_NE(first.distribution_key(), bordered.distribution_key());
}

TEST_CASE("[PathTracing][EnvironmentImportance] Shared distribution metadata does not imply shared eye state") {
	const EnvironmentImportanceMetadata first_eye = metadata();
	const EnvironmentImportanceMetadata second_eye = metadata();
	CHECK_EQ(first_eye.distribution_key(), second_eye.distribution_key());
	uint32_t first_eye_reservoir = 1;
	uint32_t second_eye_reservoir = 0;
	CHECK_NE(first_eye_reservoir, second_eye_reservoir);
}

TEST_CASE("[PathTracing][EnvironmentImportance] Padded pyramid extents preserve all source texels") {
	const EnvironmentImportancePaddedExtent array_extent = environment_importance_padded_extent(2560, 2560);
	CHECK_EQ(array_extent.width, 4096);
	CHECK_EQ(array_extent.height, 4096);
	CHECK_EQ(array_extent.mip_count, 13);
	const EnvironmentImportancePaddedExtent single_extent = environment_importance_padded_extent(2304, 2304);
	CHECK_EQ(single_extent.width, 4096);
	CHECK_EQ(single_extent.height, 4096);
	const EnvironmentImportancePaddedExtent odd_extent = environment_importance_padded_extent(5, 9);
	CHECK_EQ(odd_extent.width, 8);
	CHECK_EQ(odd_extent.height, 16);
	CHECK_EQ(odd_extent.mip_count, 5);
}

} // namespace TestEnvironmentImportance
