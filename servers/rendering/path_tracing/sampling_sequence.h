/**************************************************************************/
/*  sampling_sequence.h                                                   */
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

#pragma once

#include <cstddef>
#include <cstdint>

namespace RendererPathTracing {

// This is an engine-facing ABI, rather than a backend shader layout. Values
// are explicit so a replay remains meaningful when dimensions are added.
static constexpr uint32_t SAMPLE_SEQUENCE_ABI_VERSION = 1;

// Self-owned scalar spatiotemporal blue noise (STBN) volume contract. Each
// XY slice is a full toroidal blue-noise rank permutation. At a fixed XY
// location the Z sequence is a temporally stratified, progressive permutation.
// The volume is generated in-engine; it is neither an imported asset nor a
// vendor data set. It is intentionally bounded for low-spp realtime work.
static constexpr uint32_t SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_VERSION = 1;
static constexpr uint32_t SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH = 32;
static constexpr uint32_t SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT = 32;
static constexpr uint32_t SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH = 64;
static constexpr uint32_t SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT = 4;
static constexpr size_t SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT = size_t(SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH) * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT;
static constexpr size_t SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT = SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT * SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH * SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT;

enum SampleDimensionDomain : uint32_t {
	SAMPLE_DIMENSION_DOMAIN_DIRECT = 1,
	SAMPLE_DIMENSION_DOMAIN_LIGHT = 2,
	SAMPLE_DIMENSION_DOMAIN_TRANSPORT = 3,
	SAMPLE_DIMENSION_DOMAIN_SECONDARY_PROPOSAL = 4,
	SAMPLE_DIMENSION_DOMAIN_RECONSTRUCTION = 5,
};

enum SampleDimension : uint32_t {
	SAMPLE_DIMENSION_INVALID = 0x0000,

	// Direct-light reservoir operations. 0x0104-0x01ff are reserved.
	SAMPLE_DIMENSION_DIRECT_CANDIDATE_SELECTION = 0x0100,
	SAMPLE_DIMENSION_DIRECT_RESERVOIR_SELECTION = 0x0101,
	SAMPLE_DIMENSION_DIRECT_TEMPORAL_REUSE = 0x0102,
	SAMPLE_DIMENSION_DIRECT_SPATIAL_REUSE = 0x0103,

	// First-event light proposals. 0x0201-0x020f, 0x0212-0x021f, and
	// 0x0221-0x02ff are reserved.
	SAMPLE_DIMENSION_PUNCTUAL_LIGHT = 0x0200,
	SAMPLE_DIMENSION_ENVIRONMENT = 0x0210,
	SAMPLE_DIMENSION_ENVIRONMENT_PORTAL = 0x0211,
	SAMPLE_DIMENSION_SOLAR = 0x0220,

	// Primary transport. 0x0301-0x030f and 0x0311-0x03ff are reserved for
	// future bounce dimensions without renumbering this ABI.
	SAMPLE_DIMENSION_REFLECTION_DIRECTION = 0x0300,
	SAMPLE_DIMENSION_PRIMARY_DIFFUSE_GI_DIRECTION = 0x0310,

	// Secondary-hit proposals. 0x0403-0x04ff are reserved.
	SAMPLE_DIMENSION_SECONDARY_EMISSIVE_PROPOSAL = 0x0400,
	SAMPLE_DIMENSION_SECONDARY_DIRECT_PROPOSAL = 0x0401,
	SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT_PROPOSAL = 0x0402,

	// Reconstruction-local stochastic work. 0x0502-0x05ff are reserved.
	SAMPLE_DIMENSION_RECONSTRUCTION_JITTER = 0x0500,
	SAMPLE_DIMENSION_RECONSTRUCTION_HISTORY = 0x0501,
};

struct SampleDimensionInfo {
	SampleDimension dimension = SAMPLE_DIMENSION_INVALID;
	SampleDimensionDomain domain = SAMPLE_DIMENSION_DOMAIN_DIRECT;
	const char *name = nullptr;
};

// The baseline is the progressive Owen-scrambled low-discrepancy sequence.
// STBN is deliberately reserved for low-spp, low-dimensional realtime events;
// progressive and high-dimensional paths retain the baseline sequence.
enum SampleSequenceMode : uint32_t {
	SAMPLE_SEQUENCE_MODE_INVALID = 0,
	SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY = 1,
	SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE = 2,
};

struct SampleSequenceReplayMetadata {
	uint32_t abi_version = SAMPLE_SEQUENCE_ABI_VERSION;
	uint64_t frame_index = 0;
	uint32_t pixel_x = 0;
	uint32_t pixel_y = 0;
	uint32_t sample_index = 0;
	SampleSequenceMode sequence_mode = SAMPLE_SEQUENCE_MODE_PROGRESSIVE_OWEN_SCRAMBLED_LOW_DISCREPANCY;
};

const SampleDimensionInfo *sample_dimension_inventory(size_t &r_count);
bool sample_dimension_is_valid(SampleDimension p_dimension);
const SampleDimensionInfo *sample_dimension_get_info(SampleDimension p_dimension);
const char *sample_dimension_name(SampleDimension p_dimension);
const char *sample_dimension_domain_name(SampleDimensionDomain p_domain);

bool sample_sequence_mode_is_valid(SampleSequenceMode p_mode);
bool sample_sequence_mode_is_supported(SampleSequenceMode p_mode);
const char *sample_sequence_mode_name(SampleSequenceMode p_mode);
bool sample_sequence_replay_metadata_is_valid(const SampleSequenceReplayMetadata &p_metadata);

// The checksum serializes fixed-width integers in a specified byte order; it
// does not depend on host struct packing, pointers, floating point, or maps.
uint64_t sample_sequence_replay_checksum(const SampleSequenceReplayMetadata &p_metadata);

// Stable fixed-width key derivation for CPU/GPU implementations. It is a key
// derivation ABI, not an estimator or a random-number generator.
uint64_t sample_sequence_key(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension);

// Generates the deterministic scalar STBN volume. p_ranks must contain at
// least SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT uint16_t values. Each
// slice is directly uploadable to one layer of an R16Uint texture2d_array.
bool sample_sequence_generate_stbn_scalar_volume(uint16_t *p_ranks, size_t p_count);

// Read helpers define wrapping, frame-to-Z advancement, and a stable per-tile
// coordinate rotation for CPU replay and backend validation. The rotation
// prevents a small source tile from repeating as visible screen-space lighting.
// Dimension keys are independently derived from the STBN rank and the engine
// sampling ABI; consumers must not reuse a ray-class salt.
uint32_t sample_sequence_stbn_scalar_volume_slice_for_frame(uint64_t p_frame_index);
uint32_t sample_sequence_stbn_scalar_volume_channel_for_dimension(uint32_t p_dimension);
uint16_t sample_sequence_stbn_scalar_volume_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index);
uint16_t sample_sequence_stbn_scalar_volume_dimension_rank(const uint16_t *p_ranks, size_t p_count, uint32_t p_pixel_x, uint32_t p_pixel_y, uint64_t p_frame_index, uint32_t p_dimension);
// Returns the normalized scalar consumed by low-spp STBN estimators. The rank
// must come from sample_sequence_stbn_scalar_volume_dimension_rank() so the
// dimension's independent channel and coordinate transform remain explicit.
float sample_sequence_stbn_dimension_value(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension, uint16_t p_stbn_rank);
uint64_t sample_sequence_stbn_dimension_key(const SampleSequenceReplayMetadata &p_metadata, SampleDimension p_dimension, uint16_t p_stbn_rank);

// FNV-1a over the rank values in little-endian byte order. The no-argument
// form generates the canonical volume and returns its stable identity.
uint64_t sample_sequence_stbn_scalar_volume_checksum(const uint16_t *p_ranks, size_t p_count);
uint64_t sample_sequence_stbn_scalar_volume_checksum();

} // namespace RendererPathTracing
