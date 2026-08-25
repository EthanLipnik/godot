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

#include <array>
#include <cstring>

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

static uint32_t toroidal_distance_squared(uint32_t p_a, uint32_t p_b) {
	const uint32_t ax = p_a % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t ay = p_a / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t bx = p_b % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t by = p_b / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t forward_x = (ax + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH - bx) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t forward_y = (ay + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT - by) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
	const uint32_t dx = forward_x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH - forward_x ? forward_x : SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH - forward_x;
	const uint32_t dy = forward_y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT - forward_y ? forward_y : SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT - forward_y;
	return dx * dx + dy * dy;
}

static float progressive_nearest_neighbor_mean(const uint16_t *p_ranks, uint32_t p_rank_count) {
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT> positions = {};
	for (uint32_t position = 0; position < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT; position++) {
		positions[p_ranks[position]] = position;
	}
	uint64_t sum = 0;
	for (uint32_t rank = 0; rank < p_rank_count; rank++) {
		uint32_t nearest = UINT32_MAX;
		for (uint32_t other = 0; other < p_rank_count; other++) {
			if (rank != other) {
				const uint32_t distance = toroidal_distance_squared(positions[rank], positions[other]);
				nearest = distance < nearest ? distance : nearest;
			}
		}
		sum += nearest;
	}
	return float(sum) / float(p_rank_count);
}

static uint32_t circular_distance(uint32_t p_a, uint32_t p_b) {
	const uint32_t forward = (p_a + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH - p_b) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH;
	return forward < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH - forward ? forward : SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH - forward;
}

static float temporal_nearest_neighbor_mean(const uint16_t *p_positions, uint32_t p_count) {
	uint64_t sum = 0;
	for (uint32_t index = 0; index < p_count; index++) {
		uint32_t nearest = UINT32_MAX;
		for (uint32_t other = 0; other < p_count; other++) {
			if (index != other) {
				const uint32_t distance = circular_distance(p_positions[index], p_positions[other]);
				nearest = MIN(nearest, distance);
			}
		}
		sum += nearest;
	}
	return float(sum) / float(p_count);
}

static uint32_t hash32(uint32_t p_value) {
	p_value ^= p_value >> 16;
	p_value *= 0x7feb352dU;
	p_value ^= p_value >> 15;
	p_value *= 0x846ca68bU;
	return p_value ^ (p_value >> 16);
}

static float temporal_low_scalar_nearest_neighbor_mean(const std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT> &p_volume, uint32_t p_pixel_x, uint32_t p_pixel_y) {
	uint16_t selected_ranks[8] = { UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX };
	uint16_t selected_positions[8] = {};
	for (uint32_t frame = 0; frame < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; frame++) {
		const uint16_t rank = sample_sequence_stbn_scalar_volume_rank(p_volume.data(), p_volume.size(), p_pixel_x, p_pixel_y, frame);
		for (uint32_t index = 0; index < 8u; index++) {
			if (rank < selected_ranks[index]) {
				for (uint32_t move = 7u; move > index; move--) {
					selected_ranks[move] = selected_ranks[move - 1u];
					selected_positions[move] = selected_positions[move - 1u];
				}
				selected_ranks[index] = rank;
				selected_positions[index] = uint16_t(frame);
				break;
			}
		}
	}
	return temporal_nearest_neighbor_mean(selected_positions, 8u);
}

static float temporal_white_nearest_neighbor_mean(uint32_t p_pixel_x, uint32_t p_pixel_y) {
	uint32_t selected_scores[8] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
	uint16_t selected_positions[8] = {};
	for (uint32_t frame = 0; frame < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; frame++) {
		const uint32_t score = hash32(p_pixel_x * 0x9e3779b9U ^ p_pixel_y * 0x85ebca6bU ^ frame * 0xc2b2ae35U);
		for (uint32_t index = 0; index < 8u; index++) {
			if (score < selected_scores[index]) {
				for (uint32_t move = 7u; move > index; move--) {
					selected_scores[move] = selected_scores[move - 1u];
					selected_positions[move] = selected_positions[move - 1u];
				}
				selected_scores[index] = score;
				selected_positions[index] = uint16_t(frame);
				break;
			}
		}
	}
	return temporal_nearest_neighbor_mean(selected_positions, 8u);
}

TEST_CASE("[PathTracing][SamplingSequence] Scalar STBN volume is deterministic, complete, and stable") {
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT> first = {};
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT> second = {};
	CHECK(sample_sequence_generate_stbn_scalar_volume(first.data(), first.size()));
	CHECK(sample_sequence_generate_stbn_scalar_volume(second.data(), second.size()));
	CHECK(first == second);
	for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
		std::array<bool, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT> seen = {};
		const uint16_t *slice_ranks = first.data() + size_t(slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
		for (uint32_t index = 0; index < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT; index++) {
			CHECK(slice_ranks[index] < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT);
			CHECK_FALSE(seen[slice_ranks[index]]);
			seen[slice_ranks[index]] = true;
		}
		for (bool rank_seen : seen) {
			CHECK(rank_seen);
		}
	}
	const uint64_t checksum = sample_sequence_stbn_scalar_volume_checksum(first.data(), first.size());
	CHECK_EQ(checksum, 0x2be55a61e567b9c8ULL);
	CHECK_EQ(checksum, sample_sequence_stbn_scalar_volume_checksum());
	CHECK_EQ(sample_sequence_stbn_scalar_volume_checksum(nullptr, 0), 0ULL);
}

TEST_CASE("[PathTracing][SamplingSequence] Scalar STBN has spatial and temporal low-frequency voids") {
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT> volume = {};
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT> white = {};
	CHECK(sample_sequence_generate_stbn_scalar_volume(volume.data(), volume.size()));
	for (uint32_t index = 0; index < white.size(); index++) {
		white[index] = uint16_t(index);
	}
	for (uint32_t index = 0; index < white.size(); index++) {
		const uint32_t other = (uint32_t(index) * 40503u + 7919u) % white.size();
		const uint16_t value = white[index];
		white[index] = white[other];
		white[other] = value;
	}
	const float blue_mean = progressive_nearest_neighbor_mean(volume.data(), SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT / 8u);
	const float white_mean = progressive_nearest_neighbor_mean(white.data(), SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT / 8u);
	CHECK(blue_mean > white_mean * 1.5f);
	// Every XY location sees a 64-frame permutation of distinct scalar ranks.
	// The Z order traverses its 8x8 source lattice with the 1D progressive
	// blue-noise permutation generated by the same deterministic contract.
	for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
		for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
			std::array<bool, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT> seen = {};
			for (uint32_t frame = 0; frame < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; frame++) {
				const uint16_t rank = sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), x, y, frame);
				CHECK_FALSE(seen[rank]);
				seen[rank] = true;
			}
		}
	}
	float stbn_temporal_mean = 0.0f;
	float white_temporal_mean = 0.0f;
	for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y += 4u) {
		for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x += 4u) {
			stbn_temporal_mean += temporal_low_scalar_nearest_neighbor_mean(volume, x, y);
			white_temporal_mean += temporal_white_nearest_neighbor_mean(x, y);
		}
	}
	CHECK(stbn_temporal_mean > white_temporal_mean * 1.2f);
}

TEST_CASE("[PathTracing][SamplingSequence] Scalar STBN wraps frames and dimension keys are replay-stable") {
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT> volume = {};
	CHECK(sample_sequence_generate_stbn_scalar_volume(volume.data(), volume.size()));
	CHECK_EQ(sample_sequence_stbn_scalar_volume_slice_for_frame(0u), 0u);
	CHECK_EQ(sample_sequence_stbn_scalar_volume_slice_for_frame(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH), 0u);
	CHECK_EQ(sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 35u, 67u, 19u), sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 35u, 67u, 19u + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH));
	// The small canonical tile must not become a visible screen-space repeat.
	CHECK_NE(sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 3u, 5u, 19u), sample_sequence_stbn_scalar_volume_rank(volume.data(), volume.size(), 3u + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH, 5u, 19u));
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
	std::array<uint16_t, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT> volume = {};
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
			const float u = (float(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), x, y, 11u, DIMENSION_U)) + 0.5f) / float(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT);
			const float v = (float(sample_sequence_stbn_scalar_volume_dimension_rank(volume.data(), volume.size(), x, y, 11u, DIMENSION_V)) + 0.5f) / float(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT);
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
