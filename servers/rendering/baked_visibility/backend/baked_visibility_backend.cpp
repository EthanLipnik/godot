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
			result.can_certify_invisibility = false;
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

Error BakedVisibilityBackend::execute(BakedVisibilityBackendKind p_kind, const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	BakedVisibilityBackendBatchOutput cpu_output;
	const Error cpu_error = execute_cpu_reference(p_input, cpu_output, r_error);
	ERR_FAIL_COND_V(cpu_error != OK, cpu_error);
	if (p_kind == BakedVisibilityBackendKind::AUTO) {
		const BakedVisibilityBackendCapabilities selected = probe(BakedVisibilityBackendKind::AUTO);
		p_kind = selected.kind == BakedVisibilityBackendKind::AUTO ? BakedVisibilityBackendKind::CPU_REFERENCE : selected.kind;
	}
	if (p_kind == BakedVisibilityBackendKind::CPU_REFERENCE) {
		r_output = cpu_output;
		return OK;
	}

#ifdef METAL_ENABLED
	if (p_kind == BakedVisibilityBackendKind::METAL) {
		BakedVisibilityBackendBatchOutput accelerated_output;
		String accelerated_error;
		if (baked_visibility_metal_execute(p_input, accelerated_output, &accelerated_error) == OK && _outputs_equal(cpu_output, accelerated_output)) {
			r_output = accelerated_output;
			return OK;
		}
		cpu_output.diagnostic = accelerated_error.is_empty() ? "Metal accelerator output differed from CPU oracle; CPU fallback retained" : accelerated_error;
	}
#endif

	if (p_kind == BakedVisibilityBackendKind::VULKAN) {
		BakedVisibilityBackendBatchOutput accelerated_output;
		String accelerated_error;
		if (BakedVisibilityVulkanBatchContract::execute_batch(p_input, accelerated_output, &accelerated_error) == OK && _outputs_equal(cpu_output, accelerated_output)) {
			r_output = accelerated_output;
			return OK;
		}
		cpu_output.diagnostic = accelerated_error.is_empty() ? "Vulkan accelerator output differed from CPU oracle; CPU fallback retained" : accelerated_error;
	} else if (p_kind == BakedVisibilityBackendKind::METAL && cpu_output.diagnostic == "CPU reference batch executed") {
		cpu_output.diagnostic = "Metal acceleration unavailable; CPU fallback executed";
	}
	r_output = cpu_output;
	return OK;
}
