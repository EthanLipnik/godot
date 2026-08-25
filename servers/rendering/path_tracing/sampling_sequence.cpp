/**************************************************************************/
/*  sampling_sequence.cpp                                                 */
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

#include "sampling_sequence.h"

#include <limits>

namespace RendererPathTracing {

namespace {

static constexpr SampleDimensionInfo SAMPLE_DIMENSIONS[] = {
	{ SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION, SAMPLE_DIMENSION_DOMAIN_DIRECT, "direct-candidate-selection" },
	{ SAMPLE_DIMENSION_DIRECT_RESERVOIR_SELECTION, SAMPLE_DIMENSION_DOMAIN_DIRECT, "direct-reservoir-selection" },
	{ SAMPLE_DIMENSION_DIRECT_TEMPORAL_REUSE, SAMPLE_DIMENSION_DOMAIN_DIRECT, "direct-temporal-reuse" },
	{ SAMPLE_DIMENSION_DIRECT_SPATIAL_REUSE, SAMPLE_DIMENSION_DOMAIN_DIRECT, "direct-spatial-reuse" },
	{ SAMPLE_DIMENSION_PUNCTUAL_LIGHT, SAMPLE_DIMENSION_DOMAIN_LIGHT, "punctual-light" },
	{ SAMPLE_DIMENSION_ENVIRONMENT, SAMPLE_DIMENSION_DOMAIN_LIGHT, "environment" },
	{ SAMPLE_DIMENSION_ENVIRONMENT_PORTAL, SAMPLE_DIMENSION_DOMAIN_LIGHT, "environment-portal" },
	{ SAMPLE_DIMENSION_SOLAR, SAMPLE_DIMENSION_DOMAIN_LIGHT, "solar" },
	{ SAMPLE_DIMENSION_REFLECTION_DIRECTION, SAMPLE_DIMENSION_DOMAIN_TRANSPORT, "reflection-direction" },
	{ SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION, SAMPLE_DIMENSION_DOMAIN_TRANSPORT, "primary-diffuse-gi-direction" },
	{ SAMPLE_DIMENSION_SECONDARY_EMISSIVE_PROPOSAL, SAMPLE_DIMENSION_DOMAIN_SECONDARY_PROPOSAL, "secondary-emissive-proposal" },
	{ SAMPLE_DIMENSION_SECONDARY_DIRECT_PROPOSAL, SAMPLE_DIMENSION_DOMAIN_SECONDARY_PROPOSAL, "secondary-direct-proposal" },
	{ SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT_PROPOSAL, SAMPLE_DIMENSION_DOMAIN_SECONDARY_PROPOSAL, "secondary-environment-proposal" },
	{ SAMPLE_DIMENSION_RECONSTRUCTION_JITTER, SAMPLE_DIMENSION_DOMAIN_RECONSTRUCTION, "reconstruction-jitter" },
	{ SAMPLE_DIMENSION_RECONSTRUCTION_HISTORY, SAMPLE_DIMENSION_DOMAIN_RECONSTRUCTION, "reconstruction-history" },
};

static uint64_t fnv1a_append_u32(uint64_t p_hash, uint32_t p_value) {
	for (uint32_t byte = 0; byte < 4; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= 1099511628211ULL;
	}
	return p_hash;
}

static uint64_t fnv1a_append_u64(uint64_t p_hash, uint64_t p_value) {
	for (uint32_t byte = 0; byte < 8; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= 1099511628211ULL;
	}
	return p_hash;
}

static uint64_t mix64(uint64_t p_value) {
	p_value += 0x9e3779b97f4a7c15ULL;
	p_value = (p_value ^ (p_value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	p_value = (p_value ^ (p_value >> 27)) * 0x94d049bb133111ebULL;
	return p_value ^ (p_value >> 31);
}

static uint32_t hash32(uint32_t p_value) {
	p_value ^= p_value >> 16;
	p_value *= 0x7feb352dU;
	p_value ^= p_value >> 15;
	p_value *= 0x846ca68bU;
	return p_value ^ (p_value >> 16);
}

static uint32_t toroidal_distance_squared(uint32_t p_x, uint32_t p_y, uint32_t p_other_x, uint32_t p_other_y) {
	const uint32_t forward_x = (p_x + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH - p_other_x) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t forward_y = (p_y + SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT - p_other_y) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
	const uint32_t dx = forward_x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH - forward_x ? forward_x : SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH - forward_x;
	const uint32_t dy = forward_y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT - forward_y ? forward_y : SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT - forward_y;
	return dx * dx + dy * dy;
}

static bool generate_spatial_rank_tile(uint16_t *p_ranks, size_t p_count, uint32_t p_seed) {
	if (p_ranks == nullptr || p_count < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT) {
		return false;
	}
	bool selected[SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT] = {};
	uint16_t selected_positions[SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT] = {};
	for (uint32_t rank = 0; rank < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT; rank++) {
		uint32_t best_position = 0;
		uint32_t best_distance = 0;
		const uint32_t candidate_count = rank == 0 ? 1u : 48u;
		for (uint32_t candidate = 0; candidate < candidate_count; candidate++) {
			uint32_t position = hash32(p_seed ^ (rank * 0x9e3779b9U) ^ (candidate * 0x85ebca6bU)) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
			while (selected[position]) {
				position = (position + 1u) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
			}
			uint32_t nearest_distance = std::numeric_limits<uint32_t>::max();
			for (uint32_t prior = 0; prior < rank; prior++) {
				const uint32_t other = selected_positions[prior];
				const uint32_t distance = toroidal_distance_squared(position % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH, position / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH, other % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH, other / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH);
				nearest_distance = distance < nearest_distance ? distance : nearest_distance;
			}
			if (candidate == 0 || nearest_distance > best_distance || (nearest_distance == best_distance && position < best_position)) {
				best_position = position;
				best_distance = nearest_distance;
			}
		}
		selected[best_position] = true;
		selected_positions[rank] = uint16_t(best_position);
		p_ranks[best_position] = uint16_t(rank);
	}
	return true;
}

static void select_temporal_stbn_translations(const uint16_t *p_spatial_ranks, uint16_t *p_translation_indices) {
	static constexpr uint32_t TRANSLATION_COLUMNS = 16;
	static constexpr uint32_t TRANSLATION_ROWS = 8;
	static constexpr uint32_t TRANSLATION_COUNT = TRANSLATION_COLUMNS * TRANSLATION_ROWS;
	static_assert(TRANSLATION_COUNT >= SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH);
	bool selected[TRANSLATION_COUNT] = {};
	uint8_t low_rank_coverage[SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT] = {};
	const uint32_t low_rank_count = SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT / 8u;
	for (uint32_t selected_count = 0; selected_count < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; selected_count++) {
		uint32_t best_translation = 0;
		uint64_t best_score = std::numeric_limits<uint64_t>::max();
		for (uint32_t translation = 0; translation < TRANSLATION_COUNT; translation++) {
			if (selected[translation]) {
				continue;
			}
			const uint32_t offset_x = (translation % TRANSLATION_COLUMNS) * (SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH / TRANSLATION_COLUMNS);
			const uint32_t offset_y = (translation / TRANSLATION_COLUMNS) * (SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT / TRANSLATION_ROWS);
			uint64_t score = 0;
			for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
				for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
					const uint32_t source_x = (x + offset_x) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
					const uint32_t source_y = (y + offset_y) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
					const uint32_t covered = low_rank_coverage[y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x] + (p_spatial_ranks[source_y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + source_x] < low_rank_count ? 1u : 0u);
					const int32_t difference = int32_t(covered * 8u) - int32_t(selected_count + 1u);
					score += uint64_t(difference * difference);
				}
			}
			if (score < best_score || (score == best_score && translation < best_translation)) {
				best_score = score;
				best_translation = translation;
			}
		}
		selected[best_translation] = true;
		p_translation_indices[selected_count] = uint16_t(best_translation);
		const uint32_t offset_x = (best_translation % TRANSLATION_COLUMNS) * (SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH / TRANSLATION_COLUMNS);
		const uint32_t offset_y = (best_translation / TRANSLATION_COLUMNS) * (SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT / TRANSLATION_ROWS);
		for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
			for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
				const uint32_t source_x = (x + offset_x) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
				const uint32_t source_y = (y + offset_y) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
				low_rank_coverage[y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x] += p_spatial_ranks[source_y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + source_x] < low_rank_count ? 1u : 0u;
			}
		}
	}
}

} // namespace

const SampleDimensionInfo *sample_dimension_inventory(size_t &r_count) {
	r_count = sizeof(SAMPLE_DIMENSIONS) / sizeof(SAMPLE_DIMENSIONS[0]);
	return SAMPLE_DIMENSIONS;
}

const SampleDimensionInfo *sample_dimension_get_info(SampleDimension p_dimension) {
	for (const SampleDimensionInfo &info : SAMPLE_DIMENSIONS) {
		if (info.dimension == p_dimension) {
			return &info;
		}
	}
	return nullptr;
}

bool sample_dimension_is_valid(SampleDimension p_dimension) {
	return sample_dimension_get_info(p_dimension) != nullptr;
}

const char *sample_dimension_name(SampleDimension p_dimension) {
	const SampleDimensionInfo *info = sample_dimension_get_info(p_dimension);
	return info != nullptr ? info->name : "invalid";
}

const char *sample_dimension_domain_name(SampleDimensionDomain p_domain) {
	switch (p_domain) {
		case SAMPLE_DIMENSION_DOMAIN_DIRECT:
			return "direct";
		case SAMPLE_DIMENSION_DOMAIN_LIGHT:
			return "light";
		case SAMPLE_DIMENSION_DOMAIN_TRANSPORT:
			return "transport";
		case SAMPLE_DIMENSION_DOMAIN_SECONDARY_PROPOSAL:
			return "secondary-proposal";
		case SAMPLE_DIMENSION_DOMAIN_RECONSTRUCTION:
			return "reconstruction";
		default:
			return "invalid";
	}
}

bool sample_sequence_mode_is_valid(SampleSequenceMode p_mode) {
	return p_mode == SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY || p_mode == SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE;
}

bool sample_sequence_mode_is_supported(SampleSequenceMode p_mode) {
	return sample_sequence_mode_is_valid(p_mode);
}

const char *sample_sequence_mode_name(SampleSequenceMode p_mode) {
	switch (p_mode) {
		case SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY:
			return "progressive-owen-scrambled-low-discrepancy";
		case SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE:
			return "spatiotemporal-blue-noise";
		default:
			return "invalid";
	}
}

bool sample_sequence_replay_metadata_is_valid(const SampleSequenceReplayMetadata &p_metadata) {
	return p_metadata.abi_version == SAMPLE_SEQUENCE_ABI_VERSION && sample_sequence_mode_is_valid(p_metadata.sequence_mode);
}

uint64_t sample_sequence_replay_checksum(const SampleSequenceReplayMetadata &p_metadata) {
	if (!sample_sequence_replay_metadata_is_valid(p_metadata)) {
		return 0;
	}
	uint64_t hash = 14695981039346656037ULL;
	hash = fnv1a_append_u32(hash, p_metadata.abi_version);
	hash = fnv1a_append_u64(hash, p_metadata.frame_index);
	hash = fnv1a_append_u32(hash, p_metadata.pixel_x);
	hash = fnv1a_append_u32(hash, p_metadata.pixel_y);
	hash = fnv1a_append_u32(hash, p_metadata.sample_index);
	hash = fnv1a_append_u32(hash, uint32_t(p_metadata.sequence_mode));
	return hash;
}

uint64_t sample_sequence_key(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension) {
	if (!sample_sequence_replay_metadata_is_valid(p_metadata) || !sample_sequence_mode_is_supported(p_metadata.sequence_mode) || !sample_dimension_is_valid(p_dimension)) {
		return 0;
	}
	const uint64_t checksum = sample_sequence_replay_checksum(p_metadata);
	return mix64(checksum ^ (uint64_t(uint32_t(p_dimension)) << 32) ^ uint64_t(uint32_t(p_dimension)));
}

bool sample_sequence_generate_stbn_scalar_volume(uint16_t *p_ranks, size_t p_count) {
	if (p_ranks == nullptr || p_count < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) {
		return false;
	}
	static_assert(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT <= std::numeric_limits<uint16_t>::max());
	static_assert(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH == 0);
	for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
		uint16_t spatial_ranks[SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT] = {};
		uint16_t temporal_translations[SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH] = {};
		if (!generate_spatial_rank_tile(spatial_ranks, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT, 0x6d2b79f5U ^ (channel * 0x9e3779b9U))) {
			return false;
		}
		select_temporal_stbn_translations(spatial_ranks, temporal_translations);
		for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
		// Each slice is a translated spatial rank permutation. The translations are
		// selected against the low-rank mask so every fixed pixel receives a
		// balanced temporal sequence. The greedy prefix objective is a 1D
		// progressive blue-noise schedule: every prefix suppresses clumping of
		// low scalar ranks, rather than independently animating slice indices.
			const uint32_t translation = temporal_translations[slice];
			const uint32_t offset_x = (translation % 16u) * 2u;
			const uint32_t offset_y = (translation / 16u) * 4u;
			uint16_t *slice_ranks = p_ranks + size_t(channel * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT;
			for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
				for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
				const uint32_t source_x = (x + offset_x) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
				const uint32_t source_y = (y + offset_y) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
					slice_ranks[y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x] = spatial_ranks[source_y * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + source_x];
				}
			}
		}
	}
	return true;
}

uint32_t sample_sequence_stbn_scalar_volume_slice_for_frame(uint64_t p_frame_index) {
	return uint32_t(p_frame_index % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH);
}

uint32_t sample_sequence_stbn_scalar_volume_channel_for_dimension(uint32_t p_dimension) {
	return p_dimension % SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT;
}

// The source volume is deliberately a small, cache-friendly toroidal tile.
// Do not repeat that tile verbatim over the render target: a one-spp estimator
// turns the repeated ranks into visible lighting bands. Each tile gets a stable
// Cranley-Patterson-style coordinate and temporal rotation instead. The source
// ranks stay intact, so a pixel still sees its full 64-frame permutation.
static uint32_t stbn_tile_scramble(uint32_t p_pixel_x, uint32_t p_pixel_y, uint32_t p_dimension) {
	const uint32_t tile_x = p_pixel_x / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t tile_y = p_pixel_y / SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
	return hash32(tile_x * 0x9e3779b9U ^ tile_y * 0x85ebca6bU ^ (p_dimension + 1u) * 0xc2b2ae35U);
}

uint16_t sample_sequence_stbn_scalar_volume_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index) {
	if (p_ranks == nullptr || p_count != SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) {
		return 0;
	}
	const uint32_t scramble = stbn_tile_scramble(p_pixel_x, p_pixel_y, 0u);
	const uint32_t x = (p_pixel_x + scramble) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t y = (p_pixel_y + (scramble >> 8u)) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
	const uint32_t slice = (sample_sequence_stbn_scalar_volume_slice_for_frame(p_frame_index) + (scramble >> 16u)) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH;
	const size_t index = size_t(slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x;
	return p_ranks[index];
}

uint16_t sample_sequence_stbn_scalar_volume_dimension_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index, uint32_t p_dimension) {
	if (p_ranks == nullptr || p_count != SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) {
		return 0;
	}
	const uint32_t channel = sample_sequence_stbn_scalar_volume_channel_for_dimension(p_dimension);
	const uint32_t scramble = stbn_tile_scramble(p_pixel_x, p_pixel_y, p_dimension);
	const uint32_t x = (p_pixel_x + p_dimension * 7u + scramble) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH;
	const uint32_t y = (p_pixel_y + p_dimension * 11u + (scramble >> 8u)) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
	const uint32_t slice = (sample_sequence_stbn_scalar_volume_slice_for_frame(p_frame_index) + p_dimension * 13u + (scramble >> 16u)) % SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH;
	const size_t index = size_t(channel * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x;
	return p_ranks[index];
}

float sample_sequence_stbn_dimension_value(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension, uint16_t p_stbn_rank) {
	if (!sample_sequence_replay_metadata_is_valid(p_metadata) || p_metadata.sequence_mode != SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE || !sample_dimension_is_valid(p_dimension) || p_stbn_rank >= SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT) {
		return -1.0f;
	}
	return (float(p_stbn_rank) + 0.5f) / float(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT);
}

uint64_t sample_sequence_stbn_dimension_key(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension, uint16_t p_stbn_rank) {
	const uint64_t sequence_key = sample_sequence_key(p_metadata, p_dimension);
	return sequence_key == 0 ? 0 : mix64(sequence_key ^ (uint64_t(p_stbn_rank) << 32) ^ uint64_t(p_stbn_rank));
}

uint64_t sample_sequence_stbn_scalar_volume_checksum(const uint16_t *p_ranks, size_t p_count) {
	if (p_ranks == nullptr || p_count != SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) {
		return 0;
	}
	uint64_t hash = 14695981039346656037ULL;
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_VERSION);
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH);
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT);
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH);
	for (size_t index = 0; index < p_count; index++) {
		hash ^= uint8_t(p_ranks[index] & 0xffU);
		hash *= 1099511628211ULL;
		hash ^= uint8_t(p_ranks[index] >> 8);
		hash *= 1099511628211ULL;
	}
	return hash;
}

uint64_t sample_sequence_stbn_scalar_volume_checksum() {
	uint16_t ranks[SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT] = {};
	return sample_sequence_generate_stbn_scalar_volume(ranks, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) ? sample_sequence_stbn_scalar_volume_checksum(ranks, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) : 0;
}

} // namespace RendererPathTracing
