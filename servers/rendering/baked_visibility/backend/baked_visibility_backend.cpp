/**************************************************************************/
/*  baked_visibility_backend.cpp                                          */
/**************************************************************************/

#include "baked_visibility_backend.h"

#include "core/error/error_macros.h"
#include "core/templates/sort_array.h"
#include "baked_visibility_backend_vulkan.h"
#include "servers/rendering/rendering_device.h"

#ifdef METAL_ENABLED
#include "baked_visibility_backend_metal.h"
#endif

namespace {

struct CanonicalCandidateOrder {
	const Vector<BakedVisibilityBackendCandidate> *candidates = nullptr;

	bool operator()(uint32_t p_a, uint32_t p_b) const {
		const BakedVisibilityBackendCandidate &a = (*candidates)[p_a];
		const BakedVisibilityBackendCandidate &b = (*candidates)[p_b];
		if (a.canonical_index != b.canonical_index) {
			return a.canonical_index < b.canonical_index;
		}
		return p_a < p_b;
	}
};

static bool _segment_intersects_aabb(const Vector3 &p_from, const Vector3 &p_to, const AABB &p_bounds) {
	const Vector3 direction = p_to - p_from;
	float t_min = 0.0f;
	float t_max = 1.0f;
	for (uint32_t axis = 0; axis < 3; axis++) {
		const float origin = p_from[axis];
		const float delta = direction[axis];
		const float minimum = p_bounds.position[axis];
		const float maximum = p_bounds.position[axis] + p_bounds.size[axis];
		if (Math::is_zero_approx(delta)) {
			if (origin < minimum || origin > maximum) {
				return false;
			}
			continue;
		}
		float first = (minimum - origin) / delta;
		float last = (maximum - origin) / delta;
		if (first > last) {
			SWAP(first, last);
		}
		t_min = MAX(t_min, first);
		t_max = MIN(t_max, last);
		if (t_min > t_max) {
			return false;
		}
	}
	return true;
}

static bool _outputs_equal(const BakedVisibilityBackendBatchOutput &p_a, const BakedVisibilityBackendBatchOutput &p_b) {
	return p_a.candidate_mask == p_b.candidate_mask &&
			p_a.compacted_candidate_indices == p_b.compacted_candidate_indices &&
			p_a.blocker_hit_hints == p_b.blocker_hit_hints;
}

static bool _certificate_intersects_patch(const BakedVisibilityBackendCertificatePatch &p_patch, const Vector3 &p_from, const Vector3 &p_to, float p_margin) {
	const Vector3 direction = p_to - p_from;
	const float denominator = p_patch.normal.dot(direction);
	if (!Math::is_finite(denominator) || Math::abs(denominator) <= p_margin) {
		return false;
	}
	const float t = p_patch.normal.dot(p_patch.vertices[0] - p_from) / denominator;
	if (!Math::is_finite(t) || t <= p_margin || t >= 1.0f - p_margin) {
		return false;
	}
	const Vector3 point = p_from + direction * t;
	if (!point.is_finite()) {
		return false;
	}
	for (uint32_t vertex = 0; vertex < p_patch.vertex_count; vertex++) {
		const Vector3 &a = p_patch.vertices[vertex];
		const Vector3 &b = p_patch.vertices[(vertex + 1) % p_patch.vertex_count];
		if (p_patch.normal.dot((b - a).cross(point - a)) < p_margin) {
			return false;
		}
	}
	return true;
}

static bool _cpu_certificate_proven(const BakedVisibilityBackendCertificatePatch &p_patch, const BakedVisibilityBackendCertificateQuery &p_query) {
	constexpr float certificate_epsilon = 0.0001f;
	if (p_patch.vertex_count < 3 || p_patch.vertex_count > 4 || !p_patch.normal.is_finite() || p_patch.normal.length_squared() <= certificate_epsilon * certificate_epsilon || !p_query.source_bounds.is_finite() || !p_query.target_bounds.is_finite()) {
		return false;
	}
	for (uint32_t vertex = 0; vertex < p_patch.vertex_count; vertex++) {
		if (!p_patch.vertices[vertex].is_finite()) {
			return false;
		}
	}
	bool source_front = true;
	bool source_back = true;
	bool target_front = true;
	bool target_back = true;
	for (uint32_t corner = 0; corner < 8; corner++) {
		const float source_side = p_patch.normal.dot(p_query.source_bounds.get_endpoint(corner) - p_patch.vertices[0]);
		const float target_side = p_patch.normal.dot(p_query.target_bounds.get_endpoint(corner) - p_patch.vertices[0]);
		if (!Math::is_finite(source_side) || !Math::is_finite(target_side)) {
			return false;
		}
		source_front &= source_side > certificate_epsilon;
		source_back &= source_side < -certificate_epsilon;
		target_front &= target_side > certificate_epsilon;
		target_back &= target_side < -certificate_epsilon;
	}
	const bool separates = (source_front && target_back) || (source_back && target_front);
	const bool orientation_matches = p_patch.source_side == 0 ||
			(p_patch.source_side > 0 && source_front && target_back) ||
			(p_patch.source_side < 0 && source_back && target_front);
	if (!separates || !orientation_matches) {
		return false;
	}
	for (uint32_t source = 0; source < 8; source++) {
		for (uint32_t target = 0; target < 8; target++) {
			if (!_certificate_intersects_patch(p_patch, p_query.source_bounds.get_endpoint(source), p_query.target_bounds.get_endpoint(target), certificate_epsilon)) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

BakedVisibilityBackendCapabilities BakedVisibilityBackend::probe(BakedVisibilityBackendKind p_kind) {
	BakedVisibilityBackendCapabilities result;
	result.kind = p_kind;
	result.available = false;
	result.can_discover_candidates = false;
	result.can_certify_invisibility = false;
	result.diagnostic = "Unknown baked visibility backend; CPU fallback required";
	switch (p_kind) {
		case BakedVisibilityBackendKind::AUTO: {
			const BakedVisibilityBackendCapabilities metal = probe(BakedVisibilityBackendKind::METAL);
			if (metal.available) {
				return metal;
			}
			const BakedVisibilityBackendCapabilities vulkan = probe(BakedVisibilityBackendKind::VULKAN);
			if (vulkan.available) {
				return vulkan;
			}
			result.kind = BakedVisibilityBackendKind::AUTO;
			result.available = true;
			result.can_discover_candidates = true;
			result.can_certify_invisibility = true;
			result.diagnostic = "No GPU acceleration adapter available; CPU reference selected";
		} break;
		case BakedVisibilityBackendKind::CPU_REFERENCE:
			result.available = true;
			result.can_discover_candidates = true;
			result.can_certify_invisibility = true;
			result.diagnostic = "CPU reference certification available";
			break;
		case BakedVisibilityBackendKind::VULKAN:
			if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
				result.available = BakedVisibilityVulkanBatchContract::is_supported(rd);
				result.can_discover_candidates = result.available;
				result.supports_gpu_batches = result.available;
				result.supports_hardware_ray_queries = result.available;
				result.diagnostic = result.available ? "Vulkan ray-tracing batch adapter is schedulable" : "Vulkan ray-tracing feature unavailable; CPU fallback required";
			} else {
				result.diagnostic = "No live RenderingDevice for Vulkan acceleration; CPU fallback required";
			}
			result.can_certify_invisibility = false;
			break;
		case BakedVisibilityBackendKind::METAL:
#ifdef METAL_ENABLED
			result = baked_visibility_metal_probe();
#else
			result.diagnostic = "Metal native callback adapter was not built; CPU fallback required";
#endif
			break;
	}
	return result;
}

Error BakedVisibilityBackend::execute_cpu_reference(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	r_output = BakedVisibilityBackendBatchOutput();
	r_output.candidate_mask.resize(p_input.candidates.size());
	r_output.blocker_hit_hints.resize(p_input.candidates.size());
	const Vector3 query_center = p_input.query_bounds.get_center();
	for (uint32_t candidate = 0; candidate < p_input.candidates.size(); candidate++) {
		const BakedVisibilityBackendCandidate &entry = p_input.candidates[candidate];
		const bool discovered = p_input.query_bounds.intersects(entry.bounds) || p_input.query_bounds.encloses(entry.bounds) || entry.bounds.encloses(p_input.query_bounds);
		r_output.candidate_mask.write[candidate] = discovered ? 1 : 0;
		if (discovered) {
			r_output.compacted_candidate_indices.push_back(candidate);
		}
		const Vector3 candidate_center = entry.bounds.get_center();
		for (const BakedVisibilityBackendBlocker &blocker : p_input.blockers) {
			if (_segment_intersects_aabb(query_center, candidate_center, blocker.bounds)) {
				r_output.blocker_hit_hints.write[candidate] = 1;
				break;
			}
		}
	}
	CanonicalCandidateOrder order;
	order.candidates = &p_input.candidates;
	SortArray<uint32_t, CanonicalCandidateOrder> sorter;
	sorter.compare = order;
	sorter.sort(r_output.compacted_candidate_indices.ptrw(), r_output.compacted_candidate_indices.size());
	r_output.diagnostic = "CPU reference batch executed";
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error BakedVisibilityBackend::execute_cpu_certificate_reference(const BakedVisibilityBackendCertificateBatchInput &p_input, BakedVisibilityBackendCertificateBatchOutput &r_output, String *r_error) {
	r_output = BakedVisibilityBackendCertificateBatchOutput();
	r_output.results.resize(p_input.queries.size());
	r_output.witness_patch_indices.resize(p_input.queries.size());
	for (uint32_t index = 0; index < p_input.queries.size(); index++) {
		const BakedVisibilityBackendCertificateQuery &query = p_input.queries[index];
		r_output.witness_patch_indices.write[index] = query.patch_index;
		if (query.patch_index >= uint32_t(p_input.patches.size())) {
			r_output.results.write[index] = BakedVisibilityCertificateResult::AMBIGUOUS;
			continue;
		}
		r_output.results.write[index] = _cpu_certificate_proven(p_input.patches[query.patch_index], query) ? BakedVisibilityCertificateResult::PROVEN : BakedVisibilityCertificateResult::AMBIGUOUS;
	}
	r_output.packet_count = p_input.queries.size();
	r_output.diagnostic = "CPU convex-patch certificate oracle executed";
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error BakedVisibilityBackend::execute_certificate_batch(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendCertificateBatchInput &p_input, BakedVisibilityBackendCertificateBatchOutput &r_output, bool p_deterministic_validation, String *r_error) {
	if (p_kind == BakedVisibilityBackendKind::AUTO) {
		const BakedVisibilityBackendCapabilities selected = probe(BakedVisibilityBackendKind::AUTO);
		p_kind = selected.kind == BakedVisibilityBackendKind::AUTO ? BakedVisibilityBackendKind::CPU_REFERENCE : selected.kind;
	}
	if (p_kind == BakedVisibilityBackendKind::CPU_REFERENCE) {
		return execute_cpu_certificate_reference(p_input, r_output, r_error);
	}
#ifdef METAL_ENABLED
	if (p_kind == BakedVisibilityBackendKind::METAL) {
		BakedVisibilityBackendCertificateBatchOutput metal_output;
		String metal_error;
		if (baked_visibility_metal_execute_certificates(p_input, metal_output, &metal_error) == OK) {
			if (!p_deterministic_validation) {
				r_output = metal_output;
				return OK;
			}
			BakedVisibilityBackendCertificateBatchOutput cpu_output;
			execute_cpu_certificate_reference(p_input, cpu_output, nullptr);
			bool mismatch = metal_output.results.size() != cpu_output.results.size();
			for (uint32_t index = 0; !mismatch && index < uint32_t(cpu_output.results.size()); index++) {
				// A GPU false positive or a different canonical witness is never
				// admissible in validation mode. GPU false negatives stay fail-open.
				mismatch = (metal_output.results[index] == BakedVisibilityCertificateResult::PROVEN && cpu_output.results[index] != BakedVisibilityCertificateResult::PROVEN) ||
						(metal_output.results[index] == BakedVisibilityCertificateResult::PROVEN && metal_output.witness_patch_indices[index] != cpu_output.witness_patch_indices[index]);
			}
			r_output = cpu_output;
			r_output.gpu_executed = metal_output.gpu_executed;
			r_output.dispatch_count = metal_output.dispatch_count;
			r_output.validation_mismatch = mismatch;
			r_output.diagnostic = mismatch ? "Metal certificate validation mismatch; CPU oracle retained" : "Metal certificate CPU parity validated";
			if (mismatch && r_error) *r_error = r_output.diagnostic;
			return OK;
		}
		if (r_error) *r_error = metal_error;
	}
#endif
	return execute_cpu_certificate_reference(p_input, r_output, r_error);
}

Error BakedVisibilityBackend::execute(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	if (p_kind == BakedVisibilityBackendKind::AUTO) {
		const BakedVisibilityBackendCapabilities selected = probe(BakedVisibilityBackendKind::AUTO);
		p_kind = selected.kind == BakedVisibilityBackendKind::AUTO ? BakedVisibilityBackendKind::CPU_REFERENCE : selected.kind;
	}
	if (p_kind == BakedVisibilityBackendKind::CPU_REFERENCE) {
		return execute_cpu_reference(p_input, r_output, r_error);
	}

#ifdef METAL_ENABLED
	if (p_kind == BakedVisibilityBackendKind::METAL) {
		BakedVisibilityBackendBatchOutput accelerated_output;
		String accelerated_error;
		if (baked_visibility_metal_execute(p_input, accelerated_output, &accelerated_error) == OK) {
			r_output = accelerated_output;
			return OK;
		}
		if (r_error) *r_error = accelerated_error;
	}
#endif

	if (p_kind == BakedVisibilityBackendKind::VULKAN) {
		BakedVisibilityBackendBatchOutput accelerated_output;
		String accelerated_error;
		if (BakedVisibilityVulkanBatchContract::execute_batch(p_input, accelerated_output, &accelerated_error) == OK) {
			r_output = accelerated_output;
			return OK;
		}
		if (r_error) *r_error = accelerated_error;
	}
	Error fallback_error = execute_cpu_reference(p_input, r_output, nullptr);
	if (fallback_error == OK) {
		r_output.diagnostic = r_error && !r_error->is_empty() ? *r_error : "Optional acceleration unavailable; CPU fallback executed";
	}
	return fallback_error;
}

Error BakedVisibilityBackend::execute_deterministic_validation(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	BakedVisibilityBackendBatchOutput cpu_output;
	Error error = execute_cpu_reference(p_input, cpu_output, r_error);
	ERR_FAIL_COND_V(error != OK, error);
	BakedVisibilityBackendBatchOutput accelerated_output;
	String accelerated_error;
	error = execute(p_kind, p_input, accelerated_output, &accelerated_error);
	ERR_FAIL_COND_V(error != OK, error);
	if (!_outputs_equal(cpu_output, accelerated_output)) {
		r_output = cpu_output;
		r_output.diagnostic = "Deterministic backend validation mismatch; CPU authority retained";
		if (r_error) *r_error = r_output.diagnostic;
		return OK;
	}
	r_output = accelerated_output;
	r_output.diagnostic += "; deterministic CPU parity validated";
	if (r_error) r_error->clear();
	return OK;
}
