/**************************************************************************/
/*  diffuse_radiance_cache.h                                              */
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

#include "core/math/vector3.h"
#include "core/templates/vector.h"

#include <cstdint>

namespace RendererPathTracing {

// The cache is explicitly diffuse-only and scene-owned: it has no view ID,
// screen coordinate, temporal jitter, or eye-reuse semantics.
static constexpr uint32_t DIFFUSE_RADIANCE_CACHE_ABI_VERSION = 1;

struct DiffuseRadianceCacheRevisions {
	uint32_t abi_version = DIFFUSE_RADIANCE_CACHE_ABI_VERSION;
	uint64_t geometry = 0;
	uint64_t material = 0;
	uint64_t light = 0;
	uint64_t environment = 0;
	// Snapped camera-grid origin is part of cache space, not view history.
	int32_t grid_origin_x = 0;
	int32_t grid_origin_y = 0;
	int32_t grid_origin_z = 0;

	bool matches(const DiffuseRadianceCacheRevisions &p_other) const;
};

struct DiffuseRadianceCacheKey {
	uint32_t abi_version = DIFFUSE_RADIANCE_CACHE_ABI_VERSION;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint32_t normal_bin = 0;
	uint32_t level = 0;

	bool operator==(const DiffuseRadianceCacheKey &p_other) const;
};

struct DiffuseRadianceCacheEntry {
	DiffuseRadianceCacheKey key;
	DiffuseRadianceCacheRevisions revisions;
	Vector3 radiance;
	uint32_t sample_count = 0;
	uint64_t last_update = 0;
	bool valid = false;
};

struct DiffuseRadianceCacheQuery {
	bool hit = false;
	Vector3 radiance;
	uint32_t sample_count = 0;
};

struct DiffuseRadianceCacheMetrics {
	uint64_t query_count = 0;
	uint64_t hit_count = 0;
	uint64_t revision_rejection_count = 0;
	uint64_t invalid_radiance_rejection_count = 0;
	uint64_t update_count = 0;
};

class DiffuseRadianceCache {
	uint32_t abi_version = DIFFUSE_RADIANCE_CACHE_ABI_VERSION;
	Vector<DiffuseRadianceCacheEntry> entries;
	DiffuseRadianceCacheMetrics metrics;

	int _find(const DiffuseRadianceCacheKey &p_key) const;

public:
	uint32_t get_abi_version() const { return abi_version; }
	const DiffuseRadianceCacheMetrics &get_metrics() const { return metrics; }
	void clear();
	DiffuseRadianceCacheQuery query(const DiffuseRadianceCacheKey &p_key, const DiffuseRadianceCacheRevisions &p_revisions);
	bool update(const DiffuseRadianceCacheKey &p_key, const DiffuseRadianceCacheRevisions &p_revisions, const Vector3 &p_radiance, uint64_t p_frame);
};

DiffuseRadianceCacheKey make_diffuse_radiance_cache_key(const Vector3 &p_world_position, const Vector3 &p_world_normal, float p_cell_size, uint32_t p_level = 0);

} // namespace RendererPathTracing
