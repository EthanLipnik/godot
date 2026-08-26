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

#include "thirdparty/stbn/stbn_packed_data.h"

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
	static_assert(FLUX_STBN_PACKED_DATA_SIZE == SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
	for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
		for (uint32_t y = 0; y < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT; y++) {
			for (uint32_t x = 0; x < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH; x++) {
				const size_t packed_index = (size_t(slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x) * SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT;
				for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
					p_ranks[size_t(channel * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x] = FLUX_STBN_PACKED_DATA[packed_index + channel];
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
	// Fixed semantic mapping. Do not modulo dimensions: that would silently
	// alias two estimators onto the same stochastic field.
	switch (p_dimension) {
		case 0u: return 0u; // direct candidate/select
		case 1u: return 1u; // direct emissive select
		case 2u: return 2u; // direct barycentric U
		case 3u: return 3u; // direct barycentric V
		case 4u: return 0u; // direct reservoir
		case 5u: return 1u; // direct reuse
		case 6u: return 2u; // reflection U
		case 7u: return 3u; // reflection V
		case 8u: return 0u; // primary environment
		case 9u: return 1u; // primary environment U
		case 10u: return 2u; // primary environment V
		case 11u: return 3u; // secondary environment
		case 12u: return 0u; // secondary environment U
		case 13u: return 1u; // secondary environment V
		case 14u: return 2u; // reflection environment
		case 15u: return 3u; // solar U / paired area source
		case 16u: return 0u; // solar V is paired with channel 0 only in legacy callers
		case 17u: return 1u; // solar U
		case 18u: return 2u; // solar V
		case 20u: return 0u; // punctual/area U
		case 21u: return 1u; // punctual/area V
		case 22u: return 2u; // primary GI U
		case 23u: return 3u; // primary GI V
		case 24u: return 0u; // caustic mirror
		case 25u: return 1u; // caustic source
		case 26u: return 2u; // caustic barycentric U
		case 27u: return 3u; // caustic barycentric V
		case 28u: return 0u; // visibility U
		case 29u: return 1u; // visibility V
		case 30u: return 2u; // alpha/visibility angle
		case SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION: return 0u;
		case SAMPLE_DIMENSION_DIRECT_RESERVOIR_SELECTION: return 0u;
		case SAMPLE_DIMENSION_DIRECT_TEMPORAL_REUSE: return 1u;
		case SAMPLE_DIMENSION_DIRECT_SPATIAL_REUSE: return 1u;
		case SAMPLE_DIMENSION_PUNCTUAL_LIGHT: return 0u;
		case SAMPLE_DIMENSION_ENVIRONMENT: return 0u;
		case SAMPLE_DIMENSION_ENVIRONMENT_PORTAL: return 1u;
		case SAMPLE_DIMENSION_SOLAR: return 2u;
		case SAMPLE_DIMENSION_REFLECTION_DIRECTION: return 2u;
		case SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION: return 2u;
		case SAMPLE_DIMENSION_SECONDARY_EMISSIVE_PROPOSAL: return 3u;
		case SAMPLE_DIMENSION_SECONDARY_DIRECT_PROPOSAL: return 0u;
		case SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT_PROPOSAL: return 1u;
		default: return SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT;
	}
}

static void sample_sequence_stbn_virtual_xy(uint32_t p_pixel_x, uint32_t p_pixel_y, uint32_t &r_x, uint32_t &r_y) {
	const uint32_t local_x = p_pixel_x & 127u;
	const uint32_t local_y = p_pixel_y & 127u;
	const uint32_t tile_x = p_pixel_x >> 7u;
	const uint32_t tile_y = p_pixel_y >> 7u;
	const uint32_t offset_x = (37u * tile_x + 73u * tile_y) & 127u;
	const uint32_t offset_y = (56u * tile_x + 29u * tile_y) & 127u;
	r_x = (local_x + offset_x) & 127u;
	r_y = (local_y + offset_y) & 127u;
}

static uint32_t sample_sequence_stbn_slice(uint64_t p_frame_index, uint32_t p_sample_index, uint32_t p_sample_count, uint32_t p_dimension, uint32_t p_channel) {
	const uint32_t semantic_phase = (p_dimension * 13u + p_channel * 29u + p_sample_index * 17u + p_sample_count * 7u) & (SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH - 1u);
	return (sample_sequence_stbn_scalar_volume_slice_for_frame(p_frame_index) + semantic_phase) & (SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH - 1u);
}

uint16_t sample_sequence_stbn_scalar_volume_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index) {
	return sample_sequence_stbn_scalar_volume_rank(p_ranks, p_count, p_pixel_x, p_pixel_y, p_frame_index, 0u, 1u);
}

uint16_t sample_sequence_stbn_scalar_volume_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index, uint32_t p_sample_index, uint32_t p_sample_count) {
	if (p_ranks == nullptr || p_count != SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) {
		return 0;
	}
	uint32_t x = 0;
	uint32_t y = 0;
	sample_sequence_stbn_virtual_xy(p_pixel_x, p_pixel_y, x, y);
	const uint32_t channel = 0u;
	const uint32_t slice = sample_sequence_stbn_slice(p_frame_index, p_sample_index, p_sample_count, 0u, channel);
	const size_t index = size_t(slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x;
	return p_ranks[index];
}

uint16_t sample_sequence_stbn_scalar_volume_dimension_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index, uint32_t p_dimension) {
	return sample_sequence_stbn_scalar_volume_dimension_rank(p_ranks, p_count, p_pixel_x, p_pixel_y, p_frame_index, 0u, 1u, p_dimension);
}

uint16_t sample_sequence_stbn_scalar_volume_dimension_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index, uint32_t p_sample_index, uint32_t p_sample_count, uint32_t p_dimension) {
	if (p_ranks == nullptr || p_count != SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT) {
		return 0;
	}
	const uint32_t channel = sample_sequence_stbn_scalar_volume_channel_for_dimension(p_dimension);
	if (channel >= SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT) {
		return 0;
	}
	uint32_t x = 0;
	uint32_t y = 0;
	sample_sequence_stbn_virtual_xy(p_pixel_x, p_pixel_y, x, y);
	const uint32_t slice = sample_sequence_stbn_slice(p_frame_index, p_sample_index, p_sample_count, p_dimension, channel);
	const size_t index = size_t(channel * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + size_t(y) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH + x;
	return p_ranks[index];
}

float sample_sequence_stbn_dimension_value(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension, uint16_t p_stbn_rank) {
	if (!sample_sequence_replay_metadata_is_valid(p_metadata) || p_metadata.sequence_mode != SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE || !sample_dimension_is_valid(p_dimension) || p_stbn_rank >= 256u) {
		return -1.0f;
	}
	return (float(p_stbn_rank & 0xffu) + 0.5f) / 256.0f;
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
	for (uint32_t slice = 0; slice < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH; slice++) {
		for (size_t texel = 0; texel < SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT; texel++) {
			for (uint32_t channel = 0; channel < SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; channel++) {
				const size_t index = (size_t(channel) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH + slice) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT + texel;
				hash ^= uint8_t(p_ranks[index] & 0xffU);
				hash *= 1099511628211ULL;
			}
		}
	}
	return hash;
}

uint64_t sample_sequence_stbn_scalar_volume_checksum() {
	uint64_t hash = 14695981039346656037ULL;
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_VERSION);
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH);
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT);
	hash = fnv1a_append_u32(hash, SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH);
	for (uint32_t index = 0; index < FLUX_STBN_PACKED_DATA_SIZE; index++) {
		hash ^= FLUX_STBN_PACKED_DATA[index];
		hash *= 1099511628211ULL;
	}
	return hash;
}

} // namespace RendererPathTracing
