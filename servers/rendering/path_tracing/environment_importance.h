/**************************************************************************/
/*  environment_importance.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/basis.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/vector.h"

#include <cstdint>

namespace RendererPathTracing {

// This small, backend-neutral contract intentionally describes a single
// renderer-owned octahedral environment. It is not a ScenePacket record and
// does not expose a backend resource type.
static constexpr uint32_t ENVIRONMENT_IMPORTANCE_ABI_VERSION = 1;

enum EnvironmentImportanceStatus : uint32_t {
	ENVIRONMENT_IMPORTANCE_ACTIVE,
	ENVIRONMENT_IMPORTANCE_FALLBACK,
	ENVIRONMENT_IMPORTANCE_UNSUPPORTED,
};

enum EnvironmentImportanceCacheDecision : uint32_t {
	ENVIRONMENT_IMPORTANCE_CACHE_NO_DISTRIBUTION,
	ENVIRONMENT_IMPORTANCE_CACHE_REBUILT,
	ENVIRONMENT_IMPORTANCE_CACHE_REUSED,
};

struct EnvironmentImportanceMetadata {
	uint32_t abi_version = ENVIRONMENT_IMPORTANCE_ABI_VERSION;
	uint64_t source_id = 0;
	uint64_t sample_id = 0;
	uint64_t original_resource_id = 0;
	uint64_t generation = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	float border = 0.0f;
	bool array_layout = false;
	// World direction = world_from_radiance.xform(local direction). Orientation
	// is deliberately excluded from distribution_key()/checksum().
	Basis world_from_radiance;

	uint64_t distribution_key() const;
	uint64_t history_key() const;
	uint64_t checksum() const { return distribution_key(); }
};

struct EnvironmentImportanceDiagnostics {
	EnvironmentImportanceStatus status = ENVIRONMENT_IMPORTANCE_FALLBACK;
	EnvironmentImportanceCacheDecision cache_decision = ENVIRONMENT_IMPORTANCE_CACHE_NO_DISTRIBUTION;
	String status_reason;
	String cache_reason;
	uint64_t source_id = 0;
	uint64_t generation = 0;
	uint64_t checksum = 0;
	bool weights_selectable = false;
	String weight_state;
};

struct EnvironmentImportanceSample {
	Vector3 local_direction;
	Vector3 world_direction;
	float pdf_solid_angle = 0.0f;
	uint32_t texel_index = UINT32_MAX;
	bool valid = false;
};

struct EnvironmentImportancePaddedExtent {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mip_count = 0;
};

// CPU reference only. Runtime GPU sampling uses its own hierarchical pyramid,
// rather than this flat CDF, so tests can verify the octahedral PDF separately.
class EnvironmentImportanceDistribution {
	EnvironmentImportanceMetadata metadata;
	Vector<float> weights;
	Vector<float> cdf;
	float total_weight = 0.0f;

public:
	void clear();
	bool build(const EnvironmentImportanceMetadata &p_metadata, const Vector<float> &p_luminance);
	bool is_selectable() const { return total_weight > 0.0f && !cdf.is_empty(); }
	float get_total_weight() const { return total_weight; }
	const EnvironmentImportanceMetadata &get_metadata() const { return metadata; }
	EnvironmentImportanceSample sample(float p_u) const;
};

Vector2 environment_oct_encode(const Vector3 &p_direction);
Vector3 environment_oct_decode(const Vector2 &p_oct);
Vector2 environment_oct_apply_border(const Vector2 &p_oct_uv, float p_border);
float environment_oct_solid_angle_jacobian(const Vector2 &p_oct_uv, float p_border);
float environment_oct_texel_solid_angle(uint32_t p_x, uint32_t p_y, const EnvironmentImportanceMetadata &p_metadata);
EnvironmentImportancePaddedExtent environment_importance_padded_extent(uint32_t p_width, uint32_t p_height);
const char *environment_importance_status_name(EnvironmentImportanceStatus p_status);
const char *environment_importance_cache_name(EnvironmentImportanceCacheDecision p_decision);

} // namespace RendererPathTracing
