/**************************************************************************/
/*  indoor_lighting.cpp                                                   */
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

#include "indoor_lighting.h"

#include "core/math/math_funcs.h"

#include <cmath>
#include <limits>

namespace RendererPathTracing {

namespace {

static Error _fail(Error p_error, const char *p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static bool _is_valid_portal(const EnvironmentPortal &p_portal) {
	return p_portal.abi_version == INDOOR_LIGHTING_ABI_VERSION && p_portal.portal_id != 0 && p_portal.center.is_finite() && p_portal.axis_u.is_finite() && p_portal.axis_v.is_finite() &&
			Math::is_finite(p_portal.weight) && p_portal.weight >= 0.0f && p_portal.axis_u.cross(p_portal.axis_v).length() > CMP_EPSILON;
}

static float _portal_area(const EnvironmentPortal &p_portal) {
	return 4.0f * p_portal.axis_u.cross(p_portal.axis_v).length();
}

} // namespace

RayConeTriangleLodResult ray_cone_triangle_mip_lod(const RayConeTriangleLodInput &p_input) {
	RayConeTriangleLodResult result;
	if (p_input.abi_version != INDOOR_LIGHTING_ABI_VERSION || p_input.texture_is_constant || p_input.texture_width == 0 || p_input.texture_height == 0 || !Math::is_finite(p_input.cone_width) || !Math::is_finite(p_input.max_lod) || p_input.max_lod < 0.0f) {
		return result;
	}
	for (uint32_t i = 0; i < 3; i++) {
		if (!p_input.position[i].is_finite() || !p_input.uv[i].is_finite()) {
			return result;
		}
	}
	const float world_area = 0.5f * (p_input.position[1] - p_input.position[0]).cross(p_input.position[2] - p_input.position[0]).length();
	const Vector2 uv_a = p_input.uv[1] - p_input.uv[0];
	const Vector2 uv_b = p_input.uv[2] - p_input.uv[0];
	const float uv_area = 0.5f * Math::abs(uv_a.x * uv_b.y - uv_a.y * uv_b.x);
	const double texel_count = double(p_input.texture_width) * double(p_input.texture_height);
	if (!(world_area > CMP_EPSILON) || !(uv_area > CMP_EPSILON) || !(texel_count > 0.0)) {
		return result;
	}
	const double texel_area = double(world_area) / (double(uv_area) * texel_count);
	if (!(texel_area > 0.0) || !std::isfinite(texel_area)) {
		return result;
	}
	result.world_texel_size = (float)std::sqrt(texel_area);
	if (!Math::is_finite(result.world_texel_size) || result.world_texel_size <= 0.0f) {
		return RayConeTriangleLodResult();
	}
	result.valid = true;
	if (p_input.cone_width > 0.0f) {
		result.lod = CLAMP((float)std::log2(p_input.cone_width / result.world_texel_size), 0.0f, p_input.max_lod);
	}
	return result;
}

Error build_local_light_proposal_cell(const LightSamplingBuildResult &p_distribution, const LocalLightProposalCellInput &p_input, LocalLightProposalCell &r_cell, String *r_error) {
	r_cell = LocalLightProposalCell();
	if (p_input.abi_version != INDOOR_LIGHTING_ABI_VERSION || p_input.cell_id == 0 || p_input.maximum_candidates == 0) {
		return _fail(ERR_INVALID_PARAMETER, "Local light proposal cells require a non-zero ID and candidate bound.", r_error);
	}
	if (p_input.record_indices.size() > int(p_input.maximum_candidates)) {
		return _fail(ERR_INVALID_PARAMETER, "Local light proposal cells exceed their declared candidate bound.", r_error);
	}
	r_cell.cell_id = p_input.cell_id;
	double total_weight = 0.0;
	for (uint32_t index : p_input.record_indices) {
		if (index >= uint32_t(p_distribution.records.size())) {
			return _fail(ERR_INVALID_PARAMETER, "Local light proposal candidate index is out of range.", r_error);
		}
		for (uint32_t existing : r_cell.record_indices) {
			if (existing == index) {
				return _fail(ERR_INVALID_PARAMETER, "Local light proposal candidates must be unique.", r_error);
			}
		}
		const LightSamplingGpuRecord &record = p_distribution.records[index];
		if (record.domain != LIGHT_SAMPLING_DOMAIN_LOCAL || !Math::is_finite(record.weight) || record.weight <= 0.0f) {
			return _fail(ERR_INVALID_PARAMETER, "Local light proposal candidates must reference selectable local records.", r_error);
		}
		r_cell.record_indices.push_back(index);
		total_weight += record.weight;
	}
	if (total_weight > 0.0 && std::isfinite(total_weight)) {
		r_cell.selectable = true;
		r_cell.total_weight = total_weight > std::numeric_limits<float>::max() ? std::numeric_limits<float>::max() : (float)total_weight;
		r_cell.pdf.resize(r_cell.record_indices.size());
		for (int i = 0; i < r_cell.record_indices.size(); i++) {
			r_cell.pdf.write[i] = (float)(p_distribution.records[r_cell.record_indices[i]].weight / total_weight);
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

LocalLightProposalSample sample_local_light_proposal_cell(const LocalLightProposalCell &p_cell, float p_u) {
	LocalLightProposalSample result;
	if (!p_cell.selectable || p_cell.record_indices.is_empty() || p_cell.record_indices.size() != p_cell.pdf.size()) {
		return result;
	}
	p_u = CLAMP(p_u, 0.0f, 0.99999994f);
	float cumulative = 0.0f;
	for (int i = 0; i < p_cell.pdf.size(); i++) {
		cumulative += p_cell.pdf[i];
		if (p_u < cumulative || i + 1 == p_cell.pdf.size()) {
			result.record_index = p_cell.record_indices[i];
			result.pdf = p_cell.pdf[i];
			result.valid = result.pdf > 0.0f && Math::is_finite(result.pdf);
			return result;
		}
	}
	return result;
}

Error build_environment_portal_mixture(float p_environment_weight, const Vector<EnvironmentPortal> &p_portals, EnvironmentPortalMixture &r_mixture, String *r_error) {
	r_mixture = EnvironmentPortalMixture();
	if (!Math::is_finite(p_environment_weight) || p_environment_weight < 0.0f) {
		return _fail(ERR_INVALID_PARAMETER, "Environment proposal weight must be finite and non-negative.", r_error);
	}
	double total_weight = p_environment_weight;
	for (const EnvironmentPortal &portal : p_portals) {
		if (!_is_valid_portal(portal)) {
			return _fail(ERR_INVALID_PARAMETER, "Environment portals must have finite geometry, a non-zero ID, and a non-negative weight.", r_error);
		}
		for (const EnvironmentPortal &existing : r_mixture.portals) {
			if (existing.portal_id == portal.portal_id) {
				return _fail(ERR_INVALID_PARAMETER, "Environment portal IDs must be unique.", r_error);
			}
		}
		if (portal.weight > 0.0f) {
			r_mixture.portals.push_back(portal);
			total_weight += portal.weight;
		}
	}
	if (total_weight > 0.0 && std::isfinite(total_weight)) {
		r_mixture.selectable = true;
		r_mixture.environment_selection_pdf = (float)(p_environment_weight / total_weight);
		r_mixture.portal_selection_pdf.resize(r_mixture.portals.size());
		for (int i = 0; i < r_mixture.portals.size(); i++) {
			r_mixture.portal_selection_pdf.write[i] = (float)(r_mixture.portals[i].weight / total_weight);
		}
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

float environment_portal_pdf_solid_angle(const EnvironmentPortal &p_portal, const Vector3 &p_shading_position, const Vector3 &p_direction) {
	if (!_is_valid_portal(p_portal) || !p_shading_position.is_finite() || !p_direction.is_finite() || p_direction.length_squared() <= CMP_EPSILON) {
		return 0.0f;
	}
	const Vector3 direction = p_direction.normalized();
	const Vector3 normal = p_portal.axis_u.cross(p_portal.axis_v).normalized();
	const float denominator = normal.dot(direction);
	if (Math::abs(denominator) <= CMP_EPSILON) {
		return 0.0f;
	}
	const float distance = normal.dot(p_portal.center - p_shading_position) / denominator;
	if (!Math::is_finite(distance) || distance <= CMP_EPSILON) {
		return 0.0f;
	}
	const Vector3 offset = p_shading_position + direction * distance - p_portal.center;
	const float uu = p_portal.axis_u.dot(p_portal.axis_u);
	const float uv = p_portal.axis_u.dot(p_portal.axis_v);
	const float vv = p_portal.axis_v.dot(p_portal.axis_v);
	const float determinant = uu * vv - uv * uv;
	if (determinant <= CMP_EPSILON) {
		return 0.0f;
	}
	const float du = offset.dot(p_portal.axis_u);
	const float dv = offset.dot(p_portal.axis_v);
	const float u = (du * vv - dv * uv) / determinant;
	const float v = (dv * uu - du * uv) / determinant;
	if (Math::abs(u) > 1.0f + CMP_EPSILON || Math::abs(v) > 1.0f + CMP_EPSILON) {
		return 0.0f;
	}
	const float area = _portal_area(p_portal);
	const float cosine = Math::abs(normal.dot(-direction));
	const float pdf = distance * distance / (area * cosine);
	return Math::is_finite(pdf) && pdf > 0.0f ? pdf : 0.0f;
}

float environment_portal_mixture_pdf_solid_angle(const EnvironmentPortalMixture &p_mixture, const Vector3 &p_shading_position, const Vector3 &p_direction, float p_environment_pdf_solid_angle) {
	if (!p_mixture.selectable || !Math::is_finite(p_environment_pdf_solid_angle) || p_environment_pdf_solid_angle < 0.0f || p_mixture.portals.size() != p_mixture.portal_selection_pdf.size()) {
		return 0.0f;
	}
	double pdf = p_mixture.environment_selection_pdf * p_environment_pdf_solid_angle;
	for (int i = 0; i < p_mixture.portals.size(); i++) {
		pdf += p_mixture.portal_selection_pdf[i] * environment_portal_pdf_solid_angle(p_mixture.portals[i], p_shading_position, p_direction);
	}
	return std::isfinite(pdf) && pdf > 0.0 ? (float)pdf : 0.0f;
}

EnvironmentPortalSample sample_environment_portal(const EnvironmentPortalMixture &p_mixture, const Vector3 &p_shading_position, float p_selection_u, const Vector2 &p_surface_u, float p_environment_pdf_solid_angle) {
	EnvironmentPortalSample result;
	if (!p_mixture.selectable || !p_shading_position.is_finite() || p_mixture.portals.size() != p_mixture.portal_selection_pdf.size()) {
		return result;
	}
	p_selection_u = CLAMP(p_selection_u, 0.0f, 0.99999994f);
	float cumulative = p_mixture.environment_selection_pdf;
	int portal_index = -1;
	for (int i = 0; i < p_mixture.portal_selection_pdf.size(); i++) {
		cumulative += p_mixture.portal_selection_pdf[i];
		if (p_selection_u < cumulative || i + 1 == p_mixture.portal_selection_pdf.size()) {
			portal_index = i;
			break;
		}
	}
	if (portal_index < 0) { // The environment technique was selected.
		return result;
	}
	const EnvironmentPortal &portal = p_mixture.portals[portal_index];
	const Vector2 surface_uv(CLAMP(p_surface_u.x, 0.0f, 1.0f), CLAMP(p_surface_u.y, 0.0f, 1.0f));
	result.position = portal.center + portal.axis_u * (surface_uv.x * 2.0f - 1.0f) + portal.axis_v * (surface_uv.y * 2.0f - 1.0f);
	const Vector3 offset = result.position - p_shading_position;
	result.distance = offset.length();
	if (result.distance <= CMP_EPSILON || !Math::is_finite(result.distance)) {
		return EnvironmentPortalSample();
	}
	result.direction = offset / result.distance;
	result.portal_pdf_solid_angle = environment_portal_pdf_solid_angle(portal, p_shading_position, result.direction);
	result.mixture_pdf_solid_angle = environment_portal_mixture_pdf_solid_angle(p_mixture, p_shading_position, result.direction, p_environment_pdf_solid_angle);
	result.portal_index = portal_index;
	result.valid = result.portal_pdf_solid_angle > 0.0f && result.mixture_pdf_solid_angle > 0.0f;
	return result;
}

float environment_portal_mis_balance_weight(float p_primary_pdf, float p_other_pdf) {
	if (!Math::is_finite(p_primary_pdf) || !Math::is_finite(p_other_pdf) || p_primary_pdf < 0.0f || p_other_pdf < 0.0f) {
		return 0.0f;
	}
	const float total = p_primary_pdf + p_other_pdf;
	return total > 0.0f && Math::is_finite(total) ? p_primary_pdf / total : 0.0f;
}

TransportAdaptiveBudget compute_transport_adaptive_budget(const TransportAdaptiveBudgetInput &p_input) {
	TransportAdaptiveBudget result;
	if (p_input.abi_version != INDOOR_LIGHTING_ABI_VERSION) {
		return result;
	}
	const uint32_t minimum = MAX(1u, p_input.minimum_samples);
	const uint32_t maximum = MAX(minimum, p_input.maximum_samples);
	const float reference = Math::is_finite(p_input.variance_reference) && p_input.variance_reference > CMP_EPSILON ? p_input.variance_reference : 1.0f;
	const float variance = Math::is_finite(p_input.variance) && p_input.variance > 0.0f ? p_input.variance : 0.0f;
	const float variance_demand = CLAMP(variance / reference, 0.0f, 1.0f);
	const float validity_demand = (!p_input.reservoir_valid ? 0.35f : 0.0f) + (!p_input.cache_valid ? 0.25f : 0.0f);
	result.demand = CLAMP(variance_demand + validity_demand, 0.0f, 1.0f);
	const uint32_t count = minimum + uint32_t(std::round(double(maximum - minimum) * result.demand));
	result.direct_samples = CLAMP(count, minimum, maximum);
	result.diffuse_samples = CLAMP(!p_input.cache_valid ? MAX(minimum, (count + 1) / 2) : minimum, minimum, maximum);
	return result;
}

} // namespace RendererPathTracing
