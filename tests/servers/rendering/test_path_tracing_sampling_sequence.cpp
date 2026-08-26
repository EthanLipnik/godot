/**************************************************************************/
/*  test_path_tracing_sampling_sequence.cpp                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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

TEST_FORCE_LINK(test_path_tracing_sampling_sequence)

#include "servers/rendering/path_tracing/sampling_sequence.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace TestPathTracingSamplingSequence {

using namespace RendererPathTracing;

static SampleSequenceReplayMetadata make_metadata() {
	SampleSequenceReplayMetadata metadata;
	metadata.frame_index = 0x0102030405060708ULL;
	metadata.pixel_x = 1920;
	metadata.pixel_y = 1080;
	metadata.sample_index = 7;
	return metadata;
}

TEST_CASE("[PathTracing][SamplingSequence] Dimension inventory is explicit unique and covers every required domain") {
	size_t count = 0;
	const SampleDimensionInfo *inventory = sample_dimension_inventory(count);
	CHECK(inventory != nullptr);
	CHECK_EQ(count, size_t(15));
	bool domains[6] = {};
	for (size_t index = 0; index < count; index++) {
		CHECK(sample_dimension_is_valid(inventory[index].dimension));
		CHECK(inventory[index].name != nullptr);
		CHECK(std::strlen(inventory[index].name) > 0);
		domains[uint32_t(inventory[index].domain)] = true;
		for (size_t other = 0; other < index; other++) {
			CHECK_NE(uint32_t(inventory[index].dimension), uint32_t(inventory[other].dimension));
		}
	}
	CHECK(domains[SAMPLE_DIMENSION_DOMAIN_DIRECT]);
	CHECK(domains[SAMPLE_DIMENSION_DOMAIN_LIGHT]);
	CHECK(domains[SAMPLE_DIMENSION_DOMAIN_TRANSPORT]);
	CHECK(domains[SAMPLE_DIMENSION_DOMAIN_SECONDARY_PROPOSAL]);
	CHECK(domains[SAMPLE_DIMENSION_DOMAIN_RECONSTRUCTION]);
	CHECK(sample_dimension_is_valid(SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION));
	CHECK(sample_dimension_is_valid(SAMPLE_DIMENSION_ENVIRONMENT_PORTAL));
	CHECK(sample_dimension_is_valid(SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION));
	CHECK(sample_dimension_is_valid(SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT_PROPOSAL));
}

TEST_CASE("[PathTracing][SamplingSequence] Reserved and unknown dimensions fail closed") {
	CHECK_FALSE(sample_dimension_is_valid(SAMPLE_DIMENSION_INVALID));
	CHECK_FALSE(sample_dimension_is_valid(SampleDimension(0x0104)));
	CHECK_FALSE(sample_dimension_is_valid(SampleDimension(0x0201)));
	CHECK_FALSE(sample_dimension_is_valid(SampleDimension(0xffffffffU)));
	CHECK_EQ(std::strcmp(sample_dimension_name(SampleDimension(0x0104)), "invalid"), 0);
}

TEST_CASE("[PathTracing][SamplingSequence] Replay metadata and mode support are explicit") {
	const SampleSequenceReplayMetadata metadata = make_metadata();
	CHECK(sample_sequence_replay_metadata_is_valid(metadata));
	CHECK(sample_sequence_mode_is_valid(SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY));
	CHECK(sample_sequence_mode_is_valid(SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE));
	CHECK(sample_sequence_mode_is_supported(SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY));
	CHECK(sample_sequence_mode_is_supported(SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE));
	CHECK_EQ(std::strcmp(sample_sequence_mode_name(SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE), "spatiotemporal-blue-noise"), 0);
	SampleSequenceReplayMetadata invalid_abi = metadata;
	invalid_abi.abi_version++;
	CHECK_FALSE(sample_sequence_replay_metadata_is_valid(invalid_abi));
	CHECK_EQ(sample_sequence_replay_checksum(invalid_abi), 0ULL);
}

TEST_CASE("[PathTracing][SamplingSequence] Checksum and baseline keys retain fixed golden values") {
	const SampleSequenceReplayMetadata metadata = make_metadata();
	CHECK_EQ(sample_sequence_replay_checksum(metadata), 0x712d8c7b43e8d453ULL);
	CHECK_EQ(sample_sequence_key(metadata, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION), 0xbe5abf3a600db6ceULL);
	CHECK_EQ(sample_sequence_key(metadata, SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT_PROPOSAL), 0x010aa66484ba80ad6ULL);
}

TEST_CASE("[PathTracing][SamplingSequence] Keys differentiate replay coordinates and dimension") {
	const SampleSequenceReplayMetadata metadata = make_metadata();
	const uint64_t baseline = sample_sequence_key(metadata, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION);
	CHECK_NE(baseline, 0ULL);
	SampleSequenceReplayMetadata changed = metadata;
	changed.pixel_x++;
	CHECK_NE(baseline, sample_sequence_key(changed, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION));
	changed = metadata;
	changed.pixel_y++;
	CHECK_NE(baseline, sample_sequence_key(changed, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION));
	changed = metadata;
	changed.frame_index++;
	CHECK_NE(baseline, sample_sequence_key(changed, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION));
	changed = metadata;
	changed.sample_index++;
	CHECK_NE(baseline, sample_sequence_key(changed, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION));
	CHECK_NE(baseline, sample_sequence_key(metadata, SAMPLE_DIMENSION_DIRECT_RESERVOIR_SELECTION));
	changed = metadata;
	changed.sequence_mode = SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE;
	CHECK_NE(sample_sequence_key(changed, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION), 0ULL);
	CHECK_EQ(sample_sequence_key(metadata, SampleDimension(0x0104)), 0ULL);
}

TEST_CASE("[PathTracing][SamplingSequence] Scalar STBN volume is deterministic, complete, and stable") {
	std::vector<uint16_t> first(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	std::vector<uint16_t> second(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	CHECK(sample_sequence_generate_stbn_scalar_volume(first.data(), first.size()));
	CHECK(sample_sequence_generate_stbn_scalar_volume(second.data(), second.size()));
	CHECK(first == second);
	for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
		for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
			std::array<uint32_t, 256> histogram = {};
			for (uint32_t index = 0; index < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT; index++) {
				const uint16_t value = first[(size_t(channel) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + index];
				CHECK(value < 256u);
				histogram[value]++;
			}
			uint32_t occupied = 0;
			for (uint32_t count : histogram) occupied += count != 0u;
			CHECK(occupied >= 240u);
		}
	}
	const uint64_t checksum = sample_sequence_stbn_scalar_volume_checksum(first.data(), first.size());
	CHECK_EQ(checksum, 0x37731ac2531d3b4aULL);
	CHECK_EQ(checksum, sample_sequence_stbn_scalar_volume_checksum());
	CHECK_EQ(checksum, sample_sequence_stbn_scalar_volume_checksum());
	CHECK_EQ(sample_sequence_stbn_scalar_volume_checksum(nullptr, 0), 0ULL);
}

TEST_CASE("[PathTracing][SamplingSequence] STBN virtual tiles use the canonical toroidal address") {
	std::vector<uint16_t> volume(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
		for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
			for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
				for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
					const size_t index = (size_t(channel) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x;
					volume[index] = uint16_t(y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x);
				}
			}
		}
	}
	CHECK_EQ(sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 128u, 0u, 0u), uint16_t(56u * 128u + 37u));
	CHECK_EQ(sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 0u, 128u, 0u), uint16_t(29u * 128u + 73u));
	CHECK_EQ(sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 128u, 128u, 0u), uint16_t(85u * 128u + 110u));

	std::array<bool, 128u * 128u> seen = {};
	for (uint32_t tile_y = 0; tile_y < 128u; tile_y++) {
		for (uint32_t tile_x = 0; tile_x < 128u; tile_x++) {
			const uint16_t address = sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), tile_x * 128u, tile_y * 128u, 0u);
			CHECK(address < seen.size());
			CHECK_FALSE(seen[address]);
			seen[address] = true;
		}
	}
}

TEST_CASE("[PathTracing][SamplingSequence] STBN CPU phase matches the canonical circular GPU sequence") {
	std::vector<uint16_t> volume(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
		for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
			const size_t base = (size_t(channel) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
			std::fill(volume.begin() + base, volume.begin() + base + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT, uint16_t(channel * 64u + slice));
		}
	}
	constexpr uint32_t FRAME = 19u;
	constexpr uint32_t SAMPLE_INDEX = 3u;
	constexpr uint32_t SAMPLE_COUNT = 4u;
	constexpr uint32_t DIMENSION = 22u;
	constexpr uint32_t CHANNEL = 2u;
	const uint32_t phase = (FRAME + DIMENSION * 13u + CHANNEL * 29u + SAMPLE_INDEX * 17u + SAMPLE_COUNT * 7u) & 63u;
	const uint16_t expected = uint16_t(CHANNEL * 64u + phase);
	CHECK_EQ(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), 11u, 13u, FRAME, SAMPLE_INDEX, SAMPLE_COUNT, DIMENSION), expected);
	CHECK_EQ(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), 11u, 13u, FRAME + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH, SAMPLE_INDEX, SAMPLE_COUNT, DIMENSION), expected);
	for (uint32_t frame = 0; frame < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; frame++) {
		CHECK_EQ(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), 11u, 13u, frame, SAMPLE_INDEX, SAMPLE_COUNT, DIMENSION), sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), 11u, 13u, frame + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH, SAMPLE_INDEX, SAMPLE_COUNT, DIMENSION));
	}
}

TEST_CASE("[PathTracing][SamplingSequence] Canonical STBN slices are not translated copies") {
	std::vector<uint16_t> volume(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	CHECK(sample_sequence_generate_stbn_scalar_volume(volume.data(), volume.size()));
	for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
		const size_t first_offset = (size_t(channel) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
		const size_t second_offset = first_offset + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
		bool translated = false;
		for (uint32_t offset_y = 0; offset_y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT && !translated; offset_y++) {
			for (uint32_t offset_x = 0; offset_x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH && !translated; offset_x++) {
				bool match = true;
				for (uint32_t sample = 0; sample < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT; sample += 257u) {
					const uint32_t x = sample % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
					const uint32_t y = sample / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
					const uint32_t source = ((y + offset_y) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + (x + offset_x) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
					match &= volume[first_offset + sample] == volume[second_offset + source];
				}
				translated |= match;
			}
		}
		CHECK_FALSE(translated);
	}
	for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y += 16u) {
		for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x += 16u) {
			bool differs = false;
			const uint16_t first = sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), x, y, 0u);
			for (uint32_t frame = 1; frame < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; frame++) differs |= first != sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), x, y, frame);
			CHECK(differs);
		}
	}
}

TEST_CASE("[PathTracing][SamplingSequence] Scalar STBN wraps frames and dimension keys are replay-stable") {
	std::vector<uint16_t> volume(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	CHECK(sample_sequence_generate_stbn_scalar_volume(volume.data(), volume.size()));
	CHECK_EQ(sample_sequence_stbn_scalar_volume_slice_for_frame(0u), 0u);
	CHECK_EQ(sample_sequence_stbn_scalar_volume_slice_for_frame(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH), 0u);
	CHECK_EQ(sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 35u, 67u, 19u), sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 35u, 67u, 19u + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH));
	SampleSequenceReplayMetadata metadata = make_metadata();
	metadata.sequence_mode = SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE;
	const uint16_t rank = sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), metadata.pixel_x, metadata.pixel_y, metadata.frame_index);
	const uint16_t direct_rank = sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), metadata.pixel_x, metadata.pixel_y, metadata.frame_index, uint32_t(SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION));
	const uint16_t adjacent_rank = sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), metadata.pixel_x, metadata.pixel_y, metadata.frame_index, uint32_t(SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION) + 1u);
	const float direct_value = sample_sequence_stbn_dimension_value(metadata, SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION, direct_rank);
	CHECK(direct_value >= 0.0f);
	CHECK(direct_value < 1.0f);
	CHECK_NE(direct_value, sample_sequence_stbn_dimension_value(metadata, SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION, adjacent_rank));
	CHECK_EQ(direct_value, sample_sequence_stbn_dimension_value(metadata, SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION, direct_rank));
	CHECK_NE(direct_rank, adjacent_rank);
	const uint64_t direct = sample_sequence_stbn_dimension_key(metadata, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION, rank);
	CHECK_NE(direct, 0ULL);
	CHECK_NE(direct, sample_sequence_stbn_dimension_key(metadata, SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION, rank));
	CHECK_EQ(direct, sample_sequence_stbn_dimension_key(metadata, SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION, rank));
}

TEST_CASE("[PathTracing][SamplingSequence] Adjacent STBN dimensions are decorrelated and occupy a two-dimensional domain") {
	std::vector<uint16_t> volume(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	CHECK(sample_sequence_generate_stbn_scalar_volume(volume.data(), volume.size()));
	constexpr uint32_t DIMENSION_U = 22u;
	constexpr uint32_t DIMENSION_V = 23u;
	float sum_u = 0.0f;
	float sum_v = 0.0f;
	float sum_uu = 0.0f;
	float sum_vv = 0.0f;
	float sum_uv = 0.0f;
	bool occupied[8][8] = {};
	for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
		for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
			const float u = (float(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), x, y, 11u, DIMENSION_U) & 0xffu) + 0.5f) / 256.0f;
			const float v = (float(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), x, y, 11u, DIMENSION_V) & 0xffu) + 0.5f) / 256.0f;
			sum_u += u;
			sum_v += v;
			sum_uu += u * u;
			sum_vv += v * v;
			sum_uv += u * v;
			occupied[MIN(uint32_t(u * 8.0f), 7u)][MIN(uint32_t(v * 8.0f), 7u)] = true;
		}
	}
	constexpr float SAMPLE_COUNT = float(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT);
	const float covariance = sum_uv / SAMPLE_COUNT - (sum_u / SAMPLE_COUNT) * (sum_v / SAMPLE_COUNT);
	const float variance_u = sum_uu / SAMPLE_COUNT - (sum_u / SAMPLE_COUNT) * (sum_u / SAMPLE_COUNT);
	const float variance_v = sum_vv / SAMPLE_COUNT - (sum_v / SAMPLE_COUNT) * (sum_v / SAMPLE_COUNT);
	const float correlation_squared = covariance * covariance / MAX(variance_u * variance_v, 0.000001f);
	CHECK(correlation_squared < 0.04f);
	uint32_t occupied_count = 0;
	for (uint32_t u = 0; u < 8u; u++) {
		for (uint32_t v = 0; v < 8u; v++) {
			occupied_count += occupied[u][v] ? 1u : 0u;
		}
	}
	CHECK(occupied_count >= 48u);
}

} // namespace TestPathTracingSamplingSequence
