/**************************************************************************/
/*  diffuse_radiance_cache.cpp                                            */
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

#include "diffuse_radiance_cache.h"

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"

#include <limits>

namespace RendererPathTracing {

bool DiffuseRadianceCacheRevisions::matches(const DiffuseRadianceCacheRevisions &p_other) const {
	return abi_version == DIFFUSE_RADIANCE_CACHE_ABI_VERSION && p_other.abi_version == DIFFUSE_RADIANCE_CACHE_ABI_VERSION && geometry == p_other.geometry && material == p_other.material && light == p_other.light && environment == p_other.environment && grid_origin_x == p_other.grid_origin_x && grid_origin_y == p_other.grid_origin_y && grid_origin_z == p_other.grid_origin_z;
}

bool DiffuseRadianceCacheKey::operator==(const DiffuseRadianceCacheKey &p_other) const {
	return abi_version == DIFFUSE_RADIANCE_CACHE_ABI_VERSION && p_other.abi_version == DIFFUSE_RADIANCE_CACHE_ABI_VERSION && x == p_other.x && y == p_other.y && z == p_other.z && normal_bin == p_other.normal_bin && level == p_other.level;
}

int DiffuseRadianceCache::_find(const DiffuseRadianceCacheKey &p_key) const {
	for (int i = 0; i < entries.size(); i++) {
		if (entries[i].key == p_key) {
			return i;
		}
	}
	return -1;
}

void DiffuseRadianceCache::clear() {
	entries.clear();
	metrics = DiffuseRadianceCacheMetrics();
}

DiffuseRadianceCacheQuery DiffuseRadianceCache::query(const DiffuseRadianceCacheKey &p_key, const DiffuseRadianceCacheRevisions &p_revisions) {
	DiffuseRadianceCacheQuery result;
	metrics.query_count++;
	if (p_key.abi_version != DIFFUSE_RADIANCE_CACHE_ABI_VERSION || p_revisions.abi_version != DIFFUSE_RADIANCE_CACHE_ABI_VERSION) {
		metrics.revision_rejection_count++;
		return result;
	}
	const int index = _find(p_key);
	if (index < 0) {
		return result;
	}
	const DiffuseRadianceCacheEntry &entry = entries[index];
	if (!entry.valid || !entry.revisions.matches(p_revisions)) {
		metrics.revision_rejection_count++;
		return result;
	}
	if (!entry.radiance.is_finite() || entry.sample_count == 0) {
		metrics.invalid_radiance_rejection_count++;
		return result;
	}
	metrics.hit_count++;
	result.hit = true;
	result.radiance = entry.radiance;
	result.sample_count = entry.sample_count;
	return result;
}

bool DiffuseRadianceCache::update(const DiffuseRadianceCacheKey &p_key, const DiffuseRadianceCacheRevisions &p_revisions, const Vector3 &p_radiance, uint64_t p_frame) {
	if (p_key.abi_version != DIFFUSE_RADIANCE_CACHE_ABI_VERSION || p_revisions.abi_version != DIFFUSE_RADIANCE_CACHE_ABI_VERSION || !p_radiance.is_finite()) {
		metrics.invalid_radiance_rejection_count++;
		return false;
	}
	int index = _find(p_key);
	if (index < 0) {
		DiffuseRadianceCacheEntry entry;
		entry.key = p_key;
		entries.push_back(entry);
		index = entries.size() - 1;
	}
	DiffuseRadianceCacheEntry &entry = entries.write[index];
	if (!entry.valid || !entry.revisions.matches(p_revisions)) {
		entry.radiance = p_radiance;
		entry.sample_count = 1;
	} else {
		const uint32_t old_count = entry.sample_count;
		if (old_count < UINT32_MAX) {
			entry.radiance += (p_radiance - entry.radiance) / float(old_count + 1);
			entry.sample_count++;
		} else {
			entry.radiance = entry.radiance.lerp(p_radiance, 1.0f / float(UINT32_MAX));
		}
	}
	entry.revisions = p_revisions;
	entry.last_update = p_frame;
	entry.valid = true;
	metrics.update_count++;
	return true;
}

DiffuseRadianceCacheKey make_diffuse_radiance_cache_key(const Vector3 &p_world_position, const Vector3 &p_world_normal, float p_cell_size, uint32_t p_level) {
	DiffuseRadianceCacheKey result;
	result.level = p_level;
	if (!p_world_position.is_finite() || !p_world_normal.is_finite() || !Math::is_finite(p_cell_size) || p_cell_size <= CMP_EPSILON) {
		return result;
	}
	const float scale = p_cell_size * float(uint64_t(1) << MIN(p_level, 20u));
	const double maximum = double(std::numeric_limits<int32_t>::max());
	const double minimum = double(std::numeric_limits<int32_t>::min());
	auto quantize = [&](float p_value) {
		return int32_t(CLAMP(Math::floor(p_value / scale), minimum, maximum));
	};
	result.x = quantize(p_world_position.x);
	result.y = quantize(p_world_position.y);
	result.z = quantize(p_world_position.z);
	const Vector3 normal = p_world_normal.length_squared() > CMP_EPSILON ? p_world_normal.normalized() : Vector3(0, 1, 0);
	const float denominator = Math::abs(normal.x) + Math::abs(normal.y) + Math::abs(normal.z);
	Vector2 oct(normal.x / denominator, normal.y / denominator);
	if (normal.z < 0.0f) {
		oct = Vector2((1.0f - Math::abs(oct.y)) * (oct.x >= 0.0f ? 1.0f : -1.0f), (1.0f - Math::abs(oct.x)) * (oct.y >= 0.0f ? 1.0f : -1.0f));
	}
	const uint32_t x_bin = uint32_t(CLAMP(int((oct.x * 0.5f + 0.5f) * 255.0f), 0, 255));
	const uint32_t y_bin = uint32_t(CLAMP(int((oct.y * 0.5f + 0.5f) * 255.0f), 0, 255));
	result.normal_bin = (x_bin << 8) | y_bin;
	return result;
}

} // namespace RendererPathTracing
