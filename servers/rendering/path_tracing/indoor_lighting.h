/**************************************************************************/
/*  indoor_lighting.h                                                     */
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

#include "core/error/error_list.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/vector.h"
#include "servers/rendering/path_tracing/light_sampling.h"

#include <cstdint>

namespace RendererPathTracing {

// Backend-neutral CPU reference contracts for transport estimators. They do
// not contain screen ownership; callers must keep screen-space state per view.
static constexpr uint32_t INDOOR_LIGHTING_ABI_VERSION = 1;
static constexpr uint32_t INDOOR_LIGHTING_INVALID_INDEX = UINT32_MAX;

struct RayConeTriangleLodInput {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	Vector3 position[3];
	Vector2 uv[3];
	float cone_width = 0.0f; // World-space diameter at the hit.
	uint32_t texture_width = 0;
	uint32_t texture_height = 0;
	bool texture_is_constant = false;
	float max_lod = 0.0f;
};

struct RayConeTriangleLodResult {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	float lod = 0.0f;
	float world_texel_size = 0.0f;
	bool valid = false;
};

RayConeTriangleLodResult ray_cone_triangle_mip_lod(const RayConeTriangleLodInput &p_input);

// A finite camera-centered proposal cell. Candidate identities refer to
// LightSamplingBuildResult records, avoiding a second light-distribution ABI.
struct LocalLightProposalCellInput {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	uint64_t cell_id = 0;
	Vector<uint32_t> record_indices;
	uint32_t maximum_candidates = 64;
};

struct LocalLightProposalCell {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	uint64_t cell_id = 0;
	Vector<uint32_t> record_indices;
	Vector<float> pdf;
	float total_weight = 0.0f;
	bool selectable = false;
};

struct LocalLightProposalSample {
	uint32_t record_index = INDOOR_LIGHTING_INVALID_INDEX;
	float pdf = 0.0f;
	bool valid = false;
};

Error build_local_light_proposal_cell(const LightSamplingBuildResult &p_distribution, const LocalLightProposalCellInput &p_input, LocalLightProposalCell &r_cell, String *r_error = nullptr);
LocalLightProposalSample sample_local_light_proposal_cell(const LocalLightProposalCell &p_cell, float p_u);

// A portal is a two-sided rectangular proposal surface. axis_u/v are world
// vectors from its center to an edge, so area is 4 * |cross(axis_u, axis_v)|.
struct EnvironmentPortal {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	uint64_t portal_id = 0;
	Vector3 center;
	Vector3 axis_u;
	Vector3 axis_v;
	float weight = 0.0f;
};

struct EnvironmentPortalMixture {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	float environment_selection_pdf = 0.0f;
	Vector<EnvironmentPortal> portals;
	Vector<float> portal_selection_pdf;
	bool selectable = false;
};

struct EnvironmentPortalSample {
	uint32_t portal_index = INDOOR_LIGHTING_INVALID_INDEX;
	Vector3 position;
	Vector3 direction;
	float distance = 0.0f;
	float portal_pdf_solid_angle = 0.0f;
	float mixture_pdf_solid_angle = 0.0f;
	bool valid = false;
};

Error build_environment_portal_mixture(float p_environment_weight, const Vector<EnvironmentPortal> &p_portals, EnvironmentPortalMixture &r_mixture, String *r_error = nullptr);
EnvironmentPortalSample sample_environment_portal(const EnvironmentPortalMixture &p_mixture, const Vector3 &p_shading_position, float p_selection_u, const Vector2 &p_surface_u, float p_environment_pdf_solid_angle = 0.0f);
float environment_portal_pdf_solid_angle(const EnvironmentPortal &p_portal, const Vector3 &p_shading_position, const Vector3 &p_direction);
float environment_portal_mixture_pdf_solid_angle(const EnvironmentPortalMixture &p_mixture, const Vector3 &p_shading_position, const Vector3 &p_direction, float p_environment_pdf_solid_angle);
float environment_portal_mis_balance_weight(float p_primary_pdf, float p_other_pdf);

struct TransportAdaptiveBudgetInput {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	uint32_t minimum_samples = 1;
	uint32_t maximum_samples = 1;
	float variance = 0.0f;
	float variance_reference = 1.0f;
	bool reservoir_valid = false;
	bool cache_valid = false;
};

struct TransportAdaptiveBudget {
	uint32_t abi_version = INDOOR_LIGHTING_ABI_VERSION;
	uint32_t direct_samples = 1;
	uint32_t diffuse_samples = 1;
	float demand = 0.0f;
};

TransportAdaptiveBudget compute_transport_adaptive_budget(const TransportAdaptiveBudgetInput &p_input);

} // namespace RendererPathTracing
