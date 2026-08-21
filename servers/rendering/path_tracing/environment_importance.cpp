/**************************************************************************/
/*  environment_importance.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "environment_importance.h"

#include "core/math/math_funcs.h"

#include <cmath>
#include <cstring>

namespace RendererPathTracing {

namespace {

static void hash_bytes(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; i++) {
		r_hash ^= bytes[i];
		r_hash *= 1099511628211ULL;
	}
}

static void hash_float(uint64_t &r_hash, float p_value) {
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(p_value));
	memcpy(&bits, &p_value, sizeof(bits));
	hash_bytes(r_hash, &bits, sizeof(bits));
}

} // namespace

uint64_t EnvironmentImportanceMetadata::distribution_key() const {
	uint64_t hash = 14695981039346656037ULL;
	hash_bytes(hash, &abi_version, sizeof(abi_version));
	hash_bytes(hash, &source_id, sizeof(source_id));
	hash_bytes(hash, &sample_id, sizeof(sample_id));
	hash_bytes(hash, &original_resource_id, sizeof(original_resource_id));
	hash_bytes(hash, &generation, sizeof(generation));
	hash_bytes(hash, &width, sizeof(width));
	hash_bytes(hash, &height, sizeof(height));
	hash_float(hash, border);
	hash_bytes(hash, &array_layout, sizeof(array_layout));
	return hash;
}

uint64_t EnvironmentImportanceMetadata::history_key() const {
	uint64_t hash = distribution_key();
	for (int column = 0; column < 3; column++) {
		const Vector3 value = world_from_radiance.get_column(column);
		hash_float(hash, value.x);
		hash_float(hash, value.y);
		hash_float(hash, value.z);
	}
	return hash;
}

Vector2 environment_oct_encode(const Vector3 &p_direction) {
	Vector3 n = p_direction.normalized();
	const float sum = Math::abs(n.x) + Math::abs(n.y) + Math::abs(n.z);
	if (sum <= CMP_EPSILON) {
		return Vector2(0.5f, 0.5f);
	}
	Vector2 value(n.x / sum, n.y / sum);
	if (n.z < 0.0f) {
		const Vector2 sign(value.x >= 0.0f ? 1.0f : -1.0f, value.y >= 0.0f ? 1.0f : -1.0f);
		value = Vector2(1.0f - Math::abs(value.y), 1.0f - Math::abs(value.x)) * sign;
	}
	return value * 0.5f + Vector2(0.5f, 0.5f);
}

Vector3 environment_oct_decode(const Vector2 &p_oct) {
	Vector2 oct = p_oct * 2.0f - Vector2(1.0f, 1.0f);
	Vector3 value(oct.x, oct.y, 1.0f - Math::abs(oct.x) - Math::abs(oct.y));
	const float fold = MAX(-value.z, 0.0f);
	value.x += fold * (value.x >= 0.0f ? -1.0f : 1.0f);
	value.y += fold * (value.y >= 0.0f ? -1.0f : 1.0f);
	return value.normalized();
}

Vector2 environment_oct_apply_border(const Vector2 &p_oct_uv, float p_border) {
	const float active_scale = MAX(1.0f - p_border * 2.0f, CMP_EPSILON);
	return p_oct_uv * active_scale + Vector2(p_border, p_border);
}

float environment_oct_solid_angle_jacobian(const Vector2 &p_oct_uv, float p_border) {
	const float active_scale = 1.0f - p_border * 2.0f;
	if (active_scale <= CMP_EPSILON) {
		return 0.0f;
	}
	const Vector2 oct = (p_oct_uv - Vector2(p_border, p_border)) / active_scale;
	Vector3 unnormalized(oct.x * 2.0f - 1.0f, oct.y * 2.0f - 1.0f, 1.0f - Math::abs(oct.x * 2.0f - 1.0f) - Math::abs(oct.y * 2.0f - 1.0f));
	const float fold = MAX(-unnormalized.z, 0.0f);
	unnormalized.x += fold * (unnormalized.x >= 0.0f ? -1.0f : 1.0f);
	unnormalized.y += fold * (unnormalized.y >= 0.0f ? -1.0f : 1.0f);
	const float length = unnormalized.length();
	if (length <= CMP_EPSILON) {
		return 0.0f;
	}
	return 4.0f / (active_scale * active_scale * length * length * length);
}

float environment_oct_texel_solid_angle(uint32_t p_x, uint32_t p_y, const EnvironmentImportanceMetadata &p_metadata) {
	if (p_metadata.width == 0 || p_metadata.height == 0) {
		return 0.0f;
	}
	const Vector2 uv((float(p_x) + 0.5f) / float(p_metadata.width), (float(p_y) + 0.5f) / float(p_metadata.height));
	const float border = p_metadata.border;
	if (uv.x < border || uv.x > 1.0f - border || uv.y < border || uv.y > 1.0f - border) {
		return 0.0f;
	}
	return environment_oct_solid_angle_jacobian(uv, border) / float(p_metadata.width * p_metadata.height);
}

EnvironmentImportancePaddedExtent environment_importance_padded_extent(uint32_t p_width, uint32_t p_height) {
	EnvironmentImportancePaddedExtent result;
	if (p_width == 0 || p_height == 0) return result;
	auto next_power_of_two = [](uint32_t p_value) {
		uint32_t value = 1;
		while (value < p_value && value <= UINT32_MAX / 2) value <<= 1;
		return value;
	};
	result.width = next_power_of_two(p_width);
	result.height = next_power_of_two(p_height);
	uint32_t maximum = MAX(result.width, result.height);
	result.mip_count = 1;
	while (maximum > 1) {
		maximum >>= 1;
		result.mip_count++;
	}
	return result;
}

void EnvironmentImportanceDistribution::clear() {
	metadata = EnvironmentImportanceMetadata();
	weights.clear();
	cdf.clear();
	total_weight = 0.0f;
}

bool EnvironmentImportanceDistribution::build(const EnvironmentImportanceMetadata &p_metadata, const Vector<float> &p_luminance) {
	clear();
	if (p_metadata.width == 0 || p_metadata.height == 0 || p_luminance.size() != int(p_metadata.width * p_metadata.height)) {
		return false;
	}
	metadata = p_metadata;
	weights.resize(p_luminance.size());
	double sum = 0.0;
	for (int index = 0; index < p_luminance.size(); index++) {
		const uint32_t x = index % p_metadata.width;
		const uint32_t y = index / p_metadata.width;
		const float luminance = p_luminance[index];
		const float weight = Math::is_finite(luminance) && luminance > 0.0f ? luminance * environment_oct_texel_solid_angle(x, y, p_metadata) : 0.0f;
		weights.write[index] = weight;
		sum += weight;
	}
	if (!(sum > 0.0) || !std::isfinite(sum)) {
		return false;
	}
	total_weight = (float)sum;
	cdf.resize(weights.size());
	double cumulative = 0.0;
	for (int index = 0; index < weights.size(); index++) {
		cumulative += weights[index] / sum;
		cdf.write[index] = index + 1 == weights.size() ? 1.0f : (float)cumulative;
	}
	return true;
}

EnvironmentImportanceSample EnvironmentImportanceDistribution::sample(float p_u) const {
	EnvironmentImportanceSample result;
	if (!is_selectable()) {
		return result;
	}
	p_u = CLAMP(p_u, 0.0f, 0.99999994f);
	int low = 0;
	int high = cdf.size() - 1;
	while (low < high) {
		const int middle = low + (high - low) / 2;
		if (p_u < cdf[middle]) {
			high = middle;
		} else {
			low = middle + 1;
		}
	}
	const uint32_t index = low;
	const uint32_t x = index % metadata.width;
	const uint32_t y = index / metadata.width;
	const Vector2 uv((float(x) + 0.5f) / float(metadata.width), (float(y) + 0.5f) / float(metadata.height));
	const float solid_angle = environment_oct_texel_solid_angle(x, y, metadata);
	if (solid_angle <= 0.0f || weights[index] <= 0.0f) {
		return result;
	}
	result.local_direction = environment_oct_decode((uv - Vector2(metadata.border, metadata.border)) / MAX(1.0f - metadata.border * 2.0f, CMP_EPSILON));
	result.world_direction = metadata.world_from_radiance.xform(result.local_direction).normalized();
	result.pdf_solid_angle = weights[index] / total_weight / solid_angle;
	result.texel_index = index;
	result.valid = Math::is_finite(result.pdf_solid_angle) && result.pdf_solid_angle > 0.0f;
	return result;
}

const char *environment_importance_status_name(EnvironmentImportanceStatus p_status) {
	return p_status == ENVIRONMENT_IMPORTANCE_ACTIVE ? "active" : p_status == ENVIRONMENT_IMPORTANCE_UNSUPPORTED ? "unsupported" : "fallback";
}

const char *environment_importance_cache_name(EnvironmentImportanceCacheDecision p_decision) {
	return p_decision == ENVIRONMENT_IMPORTANCE_CACHE_REBUILT ? "rebuilt" : p_decision == ENVIRONMENT_IMPORTANCE_CACHE_REUSED ? "reused" : "no-distribution";
}

} // namespace RendererPathTracing
