/**************************************************************************/
/*  hybrid_residency.cpp                                                  */
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

#include "hybrid_residency.h"

#include "core/error/error_macros.h"
#include "core/templates/hash_map.h"

namespace RendererPathTracing {

namespace {

static constexpr uint32_t POOL_COUNT = static_cast<uint32_t>(HybridResidencyPool::MAX);

static bool _kind_valid(HybridResidencyResourceKind p_kind) {
	return static_cast<uint32_t>(p_kind) < static_cast<uint32_t>(HybridResidencyResourceKind::MAX);
}

static bool _channel_valid(HybridResidencyTextureChannel p_channel) {
	return static_cast<uint32_t>(p_channel) < static_cast<uint32_t>(HybridResidencyTextureChannel::MAX);
}

static bool _identity_equal(const HybridResidencyResourceKey &p_a, const HybridResidencyResourceKey &p_b) {
	return p_a.kind == p_b.kind && p_a.stable_id == p_b.stable_id && p_a.texture_channel == p_b.texture_channel && p_a.mip_tier == p_b.mip_tier;
}

static uint64_t _identity_hash(const HybridResidencyResourceKey &p_key) {
	uint64_t value = p_key.stable_id ^ (uint64_t(static_cast<uint32_t>(p_key.kind)) << 56u);
	value ^= uint64_t(static_cast<uint32_t>(p_key.texture_channel)) << 48u;
	value ^= uint64_t(p_key.mip_tier) << 32u;
	value ^= value >> 33u;
	value *= 0xff51afd7ed558ccdULL;
	return value ^ (value >> 33u);
}

static bool _key_equal(const HybridResidencyResourceKey &p_a, const HybridResidencyResourceKey &p_b) {
	return _identity_equal(p_a, p_b) && p_a.generation == p_b.generation;
}

static bool _identity_less(const HybridResidencyResourceKey &p_a, const HybridResidencyResourceKey &p_b) {
	if (p_a.kind != p_b.kind) {
		return static_cast<uint32_t>(p_a.kind) < static_cast<uint32_t>(p_b.kind);
	}
	if (p_a.stable_id != p_b.stable_id) {
		return p_a.stable_id < p_b.stable_id;
	}
	if (p_a.texture_channel != p_b.texture_channel) {
		return static_cast<uint32_t>(p_a.texture_channel) < static_cast<uint32_t>(p_b.texture_channel);
	}
	return p_a.mip_tier < p_b.mip_tier;
}

static bool _key_less(const HybridResidencyResourceKey &p_a, const HybridResidencyResourceKey &p_b) {
	if (_identity_less(p_a, p_b)) {
		return true;
	}
	if (_identity_less(p_b, p_a)) {
		return false;
	}
	return p_a.generation < p_b.generation;
}

static bool _fits_add(uint64_t p_used, uint64_t p_add, uint64_t p_limit) {
	return p_used <= p_limit && p_add <= p_limit - p_used;
}

static uint64_t _saturating_add(uint64_t p_a, uint64_t p_b) {
	return p_b > UINT64_MAX - p_a ? UINT64_MAX : p_a + p_b;
}

static bool _resource_valid(const HybridResidencyResourceRequest &p_resource) {
	if (p_resource.abi_version != HYBRID_RESIDENCY_ABI_VERSION || !_kind_valid(p_resource.key.kind) || !_channel_valid(p_resource.key.texture_channel) || p_resource.key.stable_id == 0 || p_resource.key.generation == 0 || p_resource.bytes == 0) {
		return false;
	}
	const bool texture = p_resource.key.kind == HybridResidencyResourceKind::TEXTURE_MIP;
	if (texture != (p_resource.key.texture_channel != HybridResidencyTextureChannel::NONE)) {
		return false;
	}
	return texture || p_resource.key.mip_tier == 0;
}

static int _find_resource_identity(const Vector<HybridResidencyResourceRequest> &p_resources, const HybridResidencyResourceKey &p_key) {
	for (int i = 0; i < p_resources.size(); i++) {
		if (_identity_equal(p_resources[i].key, p_key)) {
			return i;
		}
	}
	return -1;
}

struct _RequestIdLess {
	bool operator()(const HybridResidencyRequest &p_a, const HybridResidencyRequest &p_b) const {
		if (p_a.request_id != p_b.request_id) {
			return p_a.request_id < p_b.request_id;
		}
		if (p_a.eye_mask != p_b.eye_mask) {
			return p_a.eye_mask < p_b.eye_mask;
		}
		return p_a.priority > p_b.priority;
	}
};

struct _ResourceLess {
	bool operator()(const HybridResidencyResourceRequest &p_a, const HybridResidencyResourceRequest &p_b) const {
		return _key_less(p_a.key, p_b.key);
	}
};

struct _MergedRequest {
	HybridResidencyRequest request;
	HybridResidencyAdmissionStatus conflict_status = HybridResidencyAdmissionStatus::ADMITTED;
	HybridResidencyResourceRequest conflict_resource;
	uint64_t conflict_generation = 0;
	uint8_t retention_rank = 0;
};

struct _MergedRequestLess {
	bool operator()(const _MergedRequest &p_a, const _MergedRequest &p_b) const {
		if (p_a.retention_rank != p_b.retention_rank) {
			return p_a.retention_rank > p_b.retention_rank;
		}
		if (p_a.request.priority != p_b.request.priority) {
			return p_a.request.priority > p_b.request.priority;
		}
		return p_a.request.request_id < p_b.request.request_id;
	}
};

struct _EvictionCandidate {
	int entry_index = -1;
	uint64_t last_requested_frame = 0;
	uint32_t priority = 0;
	HybridResidencyResourceKey key;
};

struct _EvictionCandidateLess {
	bool operator()(const _EvictionCandidate &p_a, const _EvictionCandidate &p_b) const {
		if (p_a.last_requested_frame != p_b.last_requested_frame) {
			return p_a.last_requested_frame < p_b.last_requested_frame;
		}
		if (p_a.priority != p_b.priority) {
			return p_a.priority < p_b.priority;
		}
		return _key_less(p_a.key, p_b.key);
	}
};

struct _RetirementLess {
	bool operator()(const HybridResidencyRetirement &p_a, const HybridResidencyRetirement &p_b) const {
		return _key_less(p_a.key, p_b.key);
	}
};

struct _ResourceEyeUnion {
	HybridResidencyResourceKey key;
	uint8_t eye_mask = 0;
};

struct _ResourceEyeUnionLess {
	bool operator()(const _ResourceEyeUnion &p_a, const _ResourceEyeUnion &p_b) const {
		return _key_less(p_a.key, p_b.key);
	}
};

} // namespace

HybridResidencyPool hybrid_residency_pool_for_kind(HybridResidencyResourceKind p_kind) {
	switch (p_kind) {
		case HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE:
			return HybridResidencyPool::GEOMETRY_CLUSTER_PAGES;
		case HybridResidencyResourceKind::TEXTURE_MIP:
			return HybridResidencyPool::TEXTURE_MIPS;
		case HybridResidencyResourceKind::MATERIAL_DESCRIPTOR:
			return HybridResidencyPool::MATERIAL_DESCRIPTORS;
		case HybridResidencyResourceKind::BLAS:
			return HybridResidencyPool::BLAS;
		case HybridResidencyResourceKind::TLAS_INSTANCE:
			return HybridResidencyPool::TLAS_INSTANCES;
		default:
			return HybridResidencyPool::GEOMETRY_CLUSTER_PAGES;
	}
}

int HybridResidencyPlanner::_find_entry_identity(const HybridResidencyResourceKey &p_key) const {
	const Vector<int> *bucket = entry_identity_index.getptr(_identity_hash(p_key));
	if (!bucket) return -1;
	for (int index : *bucket) if (index >= 0 && index < entries.size() && _identity_equal(entries[index].key, p_key)) return index;
	return -1;
}

void HybridResidencyPlanner::_rebuild_entry_identity_index() {
	entry_identity_index.clear();
	for (int index = 0; index < entries.size(); index++) entry_identity_index[_identity_hash(entries[index].key)].push_back(index);
}

void HybridResidencyPlanner::_refresh_usage() {
	for (uint32_t i = 0; i < POOL_COUNT; i++) {
		diagnostics.resident_bytes[i] = 0;
		diagnostics.resident_slots[i] = 0;
	}
	for (const Entry &entry : entries) {
		const uint32_t pool = static_cast<uint32_t>(hybrid_residency_pool_for_kind(entry.key.kind));
		diagnostics.resident_bytes[pool] = _saturating_add(diagnostics.resident_bytes[pool], entry.bytes);
		if (diagnostics.resident_slots[pool] != UINT32_MAX) {
			diagnostics.resident_slots[pool]++;
		}
	}
}

void HybridResidencyPlanner::_append_diagnostic(HybridResidencyDiagnosticReason p_reason, uint64_t p_request_id, const HybridResidencyResourceRequest &p_resource, uint64_t p_resident_generation) {
	HybridResidencyDiagnostic event;
	event.reason = p_reason;
	event.request_id = p_request_id;
	event.key = p_resource.key;
	event.pool = hybrid_residency_pool_for_kind(p_resource.key.kind);
	event.requested_bytes = p_resource.bytes;
	event.resident_generation = p_resident_generation;
	diagnostics.events.push_back(event);
}

void HybridResidencyPlanner::clear() {
	entries.clear();
	entry_identity_index.clear();
	pending_requests.clear();
	diagnostics = HybridResidencyFrameDiagnostics();
	current_frame = 0;
	completed_retirement_token = 0;
	frame_open = false;
	frame_committed = false;
}

Error HybridResidencyPlanner::set_budgets(const HybridResidencyBudgets &p_budgets) {
	ERR_FAIL_COND_V_MSG(p_budgets.abi_version != HYBRID_RESIDENCY_ABI_VERSION, ERR_INVALID_PARAMETER, "Hybrid residency budget ABI version is incompatible.");
	uint64_t used_bytes[POOL_COUNT] = {};
	uint32_t used_slots[POOL_COUNT] = {};
	for (const Entry &entry : entries) {
		const uint32_t pool = static_cast<uint32_t>(hybrid_residency_pool_for_kind(entry.key.kind));
		used_bytes[pool] = _saturating_add(used_bytes[pool], entry.bytes);
		if (used_slots[pool] != UINT32_MAX) {
			used_slots[pool]++;
		}
	}
	for (uint32_t i = 0; i < POOL_COUNT; i++) {
		ERR_FAIL_COND_V_MSG(used_bytes[i] > p_budgets.pools[i].maximum_resident_bytes || used_slots[i] > p_budgets.pools[i].maximum_resident_slots, ERR_ALREADY_IN_USE, "Hybrid residency budgets cannot be reduced below allocations awaiting retirement completion.");
	}
	budgets = p_budgets;
	return OK;
}

Error HybridResidencyPlanner::begin_frame(uint64_t p_frame, uint64_t p_completed_retirement_token) {
	ERR_FAIL_COND_V_MSG(p_frame == 0 || p_frame <= current_frame, ERR_INVALID_PARAMETER, "Hybrid residency frames must be nonzero and strictly increasing.");
	ERR_FAIL_COND_V_MSG(p_completed_retirement_token < completed_retirement_token, ERR_INVALID_PARAMETER, "Hybrid residency completion tokens must be monotonic.");
	ERR_FAIL_COND_V_MSG(frame_open && !frame_committed, ERR_BUSY, "The previous hybrid residency frame must be committed before beginning another frame.");

	current_frame = p_frame;
	completed_retirement_token = p_completed_retirement_token;
	pending_requests.clear();
	diagnostics = HybridResidencyFrameDiagnostics();
	diagnostics.frame = p_frame;
	for (int i = entries.size() - 1; i >= 0; i--) {
		const Entry &entry = entries[i];
		if (entry.state == HybridResidencyState::RETIRING && entry.retirement_completion_token <= completed_retirement_token) {
			entries.remove_at(i);
			diagnostics.retired_resources++;
		}
	}
	_refresh_usage();
	_rebuild_entry_identity_index();
	frame_open = true;
	frame_committed = false;
	return OK;
}

bool HybridResidencyPlanner::reuse_committed_frame(uint64_t p_frame, uint64_t p_completed_retirement_token, const Vector<HybridResidencyResourceKey> &p_visible_keys) {
	if (p_frame == 0 || p_frame <= current_frame || p_completed_retirement_token < completed_retirement_token || (frame_open && !frame_committed) || p_visible_keys.is_empty()) {
		return false;
	}
	// A retirement may have completed since the last plan. Do not hide that
	// transition behind a replay; begin_frame()/commit() must remove and
	// re-admit it through the normal fail-open path.
	for (const Entry &entry : entries) {
		if (entry.state != HybridResidencyState::RESIDENT) {
			return false;
		}
	}
	for (const HybridResidencyResourceKey &key : p_visible_keys) {
		const int entry_index = _find_entry_identity(key);
		if (entry_index < 0) {
			return false;
		}
		const Entry &entry = entries[entry_index];
		if (entry.state != HybridResidencyState::RESIDENT || entry.key.generation != key.generation) {
			return false;
		}
	}
	current_frame = p_frame;
	completed_retirement_token = p_completed_retirement_token;
	pending_requests.clear();
	diagnostics = HybridResidencyFrameDiagnostics();
	diagnostics.frame = p_frame;
	for (const HybridResidencyResourceKey &key : p_visible_keys) {
		const int entry_index = _find_entry_identity(key);
		ERR_FAIL_COND_V(entry_index < 0, false);
		entries.write[entry_index].last_requested_frame = p_frame;
	}
	frame_open = true;
	frame_committed = true;
	return true;
}

Error HybridResidencyPlanner::request(const HybridResidencyRequest &p_request) {
	ERR_FAIL_COND_V_MSG(!frame_open || frame_committed, ERR_UNCONFIGURED, "Hybrid residency requests require an open, uncommitted frame.");
	ERR_FAIL_COND_V_MSG(p_request.abi_version != HYBRID_RESIDENCY_ABI_VERSION || p_request.request_id == 0 || p_request.eye_mask == 0 || (p_request.eye_mask & ~HYBRID_RESIDENCY_EYE_STEREO) != 0 || p_request.resources.is_empty(), ERR_INVALID_PARAMETER, "Hybrid residency request metadata is invalid.");
	for (const HybridResidencyResourceRequest &resource : p_request.resources) {
		ERR_FAIL_COND_V_MSG(!_resource_valid(resource), ERR_INVALID_PARAMETER, "Hybrid residency resource metadata is invalid.");
	}
	pending_requests.push_back(p_request);
	diagnostics.submitted_requests++;
	return OK;
}

HybridResidencyCommitResult HybridResidencyPlanner::commit() {
	HybridResidencyCommitResult result;
	ERR_FAIL_COND_V_MSG(!frame_open || frame_committed, result, "Hybrid residency commit requires an open, uncommitted frame.");
	frame_committed = true;

	Vector<HybridResidencyRequest> sorted_requests = pending_requests;
	sorted_requests.sort_custom<_RequestIdLess>();
	Vector<_MergedRequest> merged;
	for (const HybridResidencyRequest &source : sorted_requests) {
		if (merged.is_empty() || merged[merged.size() - 1].request.request_id != source.request_id) {
			_MergedRequest group;
			group.request.request_id = source.request_id;
			group.request.priority = source.priority;
			group.request.eye_mask = source.eye_mask;
			merged.push_back(group);
		}
		_MergedRequest &group = merged.write[merged.size() - 1];
		group.request.priority = group.request.priority > source.priority ? group.request.priority : source.priority;
		group.request.eye_mask |= source.eye_mask;
		for (const HybridResidencyResourceRequest &incoming : source.resources) {
			const int existing_index = _find_resource_identity(group.request.resources, incoming.key);
			if (existing_index < 0) {
				group.request.resources.push_back(incoming);
				continue;
			}
			diagnostics.deduplicated_resources++;
			HybridResidencyResourceRequest &existing = group.request.resources.write[existing_index];
			if (existing.key.generation != incoming.key.generation) {
				const uint64_t minimum_generation = existing.key.generation < incoming.key.generation ? existing.key.generation : incoming.key.generation;
				const uint64_t maximum_generation = existing.key.generation > incoming.key.generation ? existing.key.generation : incoming.key.generation;
				if (group.conflict_status == HybridResidencyAdmissionStatus::GENERATION_MISMATCH) {
					group.conflict_resource.key.generation = group.conflict_resource.key.generation < minimum_generation ? group.conflict_resource.key.generation : minimum_generation;
					group.conflict_generation = group.conflict_generation > maximum_generation ? group.conflict_generation : maximum_generation;
				} else {
					group.conflict_resource = existing;
					group.conflict_resource.key.generation = minimum_generation;
					group.conflict_generation = maximum_generation;
				}
				group.conflict_status = HybridResidencyAdmissionStatus::GENERATION_MISMATCH;
			} else if (existing.bytes != incoming.bytes && group.conflict_status != HybridResidencyAdmissionStatus::GENERATION_MISMATCH) {
				group.conflict_status = HybridResidencyAdmissionStatus::METADATA_MISMATCH;
				const HybridResidencyResourceRequest &smaller = existing.bytes < incoming.bytes ? existing : incoming;
				if (group.conflict_resource.bytes == 0 || smaller.bytes < group.conflict_resource.bytes) {
					group.conflict_resource = smaller;
				}
			}
			existing.available = existing.available && incoming.available;
			if (static_cast<uint32_t>(incoming.retention) > static_cast<uint32_t>(existing.retention)) {
				existing.retention = incoming.retention;
			}
		}
	}

	for (_MergedRequest &group : merged) {
		group.request.resources.sort_custom<_ResourceLess>();
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			const uint8_t rank = static_cast<uint8_t>(resource.retention);
			group.retention_rank = group.retention_rank > rank ? group.retention_rank : rank;
		}
	}
	// Gather then sort once. The former linear search per resource made this
	// union quadratic for imported scenes with many repeated materials.
	Vector<_ResourceEyeUnion> resource_eye_unions;
	for (const _MergedRequest &group : merged) {
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			resource_eye_unions.push_back({ resource.key, group.request.eye_mask });
		}
	}
	resource_eye_unions.sort_custom<_ResourceEyeUnionLess>();
	int eye_union_write = 0;
	for (int read = 0; read < resource_eye_unions.size(); read++) {
		if (eye_union_write == 0 || !_key_equal(resource_eye_unions[eye_union_write - 1].key, resource_eye_unions[read].key)) {
			resource_eye_unions.write[eye_union_write++] = resource_eye_unions[read];
		} else {
			resource_eye_unions.write[eye_union_write - 1].eye_mask |= resource_eye_unions[read].eye_mask;
		}
	}
	resource_eye_unions.resize(eye_union_write);
	merged.sort_custom<_MergedRequestLess>();
	diagnostics.merged_requests = merged.size();
	// `entries` persists across frames. Build a collision-resolved identity index
	// once per commit instead of linearly scanning it for every resource in every
	// imported-scene request. Generation deliberately is not in this key: stale
	// generation handling below must continue to find the resident identity.
		auto identity_hash = [](const HybridResidencyResourceKey &p_key) {
		uint64_t value = p_key.stable_id ^ (uint64_t(static_cast<uint32_t>(p_key.kind)) << 56u);
		value ^= uint64_t(static_cast<uint32_t>(p_key.texture_channel)) << 48u;
		value ^= uint64_t(p_key.mip_tier) << 32u;
		value ^= value >> 33u;
		value *= 0xff51afd7ed558ccdULL;
		value ^= value >> 33u;
		return value;
	};
	HashMap<uint64_t, Vector<int>> entry_identity_index;
	for (int entry_index = 0; entry_index < entries.size(); entry_index++) {
		entry_identity_index[identity_hash(entries[entry_index].key)].push_back(entry_index);
	}
	auto find_commit_entry_identity = [&](const HybridResidencyResourceKey &p_key) {
		const Vector<int> *bucket = entry_identity_index.getptr(identity_hash(p_key));
		if (!bucket) return -1;
		for (int entry_index : *bucket) {
			if (entry_index >= 0 && entry_index < entries.size() && _identity_equal(entries[entry_index].key, p_key)) return entry_index;
		}
		return -1;
	};
	using IdentityMembership = HashMap<uint64_t, Vector<HybridResidencyResourceKey>>;
	auto membership_contains = [&](const IdentityMembership &p_index, const HybridResidencyResourceKey &p_key) {
		const Vector<HybridResidencyResourceKey> *bucket = p_index.getptr(identity_hash(p_key));
		if (!bucket) return false;
		for (const HybridResidencyResourceKey &entry : *bucket) if (_identity_equal(entry, p_key)) return true;
		return false;
	};
	auto membership_insert = [&](IdentityMembership &r_index, const HybridResidencyResourceKey &p_key) {
		if (!membership_contains(r_index, p_key)) r_index[identity_hash(p_key)].push_back(p_key);
	};

	Vector<HybridResidencyResourceKey> requested_by_pool[POOL_COUNT];
	Vector<HybridResidencyResourceKey> protected_resources;
	Vector<HybridResidencyResourceKey> eviction_locked;
	IdentityMembership requested_index[POOL_COUNT];
	IdentityMembership protected_index;
	IdentityMembership eviction_locked_index;

	for (_MergedRequest &group : merged) {
		HybridResidencyAdmission admission;
		admission.request_id = group.request.request_id;
		admission.eye_mask = group.request.eye_mask;

		if (group.conflict_status != HybridResidencyAdmissionStatus::ADMITTED) {
			admission.status = group.conflict_status;
			if (group.conflict_status == HybridResidencyAdmissionStatus::GENERATION_MISMATCH) {
				_append_diagnostic(HybridResidencyDiagnosticReason::GENERATION_MISMATCH, group.request.request_id, group.conflict_resource, group.conflict_generation);
			} else {
				_append_diagnostic(HybridResidencyDiagnosticReason::METADATA_MISMATCH, group.request.request_id, group.conflict_resource);
			}
			diagnostics.rejected_requests++;
			result.admissions.push_back(admission);
			continue;
		}

		bool missing = false;
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			if (!resource.available) {
				_append_diagnostic(HybridResidencyDiagnosticReason::MISSING_RESOURCE, group.request.request_id, resource);
				diagnostics.missing_resources++;
				missing = true;
			}
		}
		if (missing) {
			admission.status = HybridResidencyAdmissionStatus::MISSING_RESOURCE;
			diagnostics.rejected_requests++;
			result.admissions.push_back(admission);
			continue;
		}

		uint32_t new_requests[POOL_COUNT] = {};
		bool request_cap_failed = false;
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			const uint32_t pool = static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind));
			if (!membership_contains(requested_index[pool], resource.key)) {
				new_requests[pool]++;
			}
		}
		for (uint32_t pool = 0; pool < POOL_COUNT; pool++) {
			if (new_requests[pool] > budgets.pools[pool].maximum_requests_per_frame - MIN(budgets.pools[pool].maximum_requests_per_frame, static_cast<uint32_t>(requested_by_pool[pool].size()))) {
				for (const HybridResidencyResourceRequest &resource : group.request.resources) {
					if (static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind)) == pool) {
						_append_diagnostic(HybridResidencyDiagnosticReason::REQUEST_CAP_EXCEEDED, group.request.request_id, resource);
						break;
					}
				}
				request_cap_failed = true;
			}
		}
		if (request_cap_failed) {
			admission.status = HybridResidencyAdmissionStatus::REQUEST_CAP_REJECTED;
			diagnostics.rejected_requests++;
			result.admissions.push_back(admission);
			continue;
		}
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			const uint32_t pool = static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind));
			if (!membership_contains(requested_index[pool], resource.key)) {
				requested_by_pool[pool].push_back(resource.key);
				membership_insert(requested_index[pool], resource.key);
			} else {
				diagnostics.deduplicated_resources++;
			}
		}

		bool stale = false;
		bool retiring = false;
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			const int entry_index = find_commit_entry_identity(resource.key);
			if (entry_index < 0) {
				continue;
			}
			Entry &entry = entries.write[entry_index];
			if (entry.key.generation != resource.key.generation) {
				_append_diagnostic(HybridResidencyDiagnosticReason::STALE_GENERATION, group.request.request_id, resource, entry.key.generation);
				diagnostics.stale_resources++;
				stale = true;
				if (!membership_contains(protected_index, entry.key)) {
					entry.state = HybridResidencyState::RETIRE_REQUESTED;
					if (!membership_contains(eviction_locked_index, entry.key)) {
						eviction_locked.push_back(entry.key);
						membership_insert(eviction_locked_index, entry.key);
					}
				}
			} else if (entry.bytes != resource.bytes) {
				_append_diagnostic(HybridResidencyDiagnosticReason::METADATA_MISMATCH, group.request.request_id, resource, entry.key.generation);
				stale = true;
			} else if (entry.state == HybridResidencyState::RETIRING || membership_contains(eviction_locked_index, entry.key)) {
				_append_diagnostic(HybridResidencyDiagnosticReason::POOL_SLOTS_EXCEEDED, group.request.request_id, resource, entry.key.generation);
				retiring = true;
			}
		}
		if (stale || retiring) {
			admission.status = stale ? HybridResidencyAdmissionStatus::STALE_RESOURCE : HybridResidencyAdmissionStatus::BUDGET_REJECTED;
			diagnostics.rejected_requests++;
			if (retiring) {
				diagnostics.budget_rejections++;
			}
			result.admissions.push_back(admission);
			continue;
		}

		Vector<HybridResidencyResourceRequest> new_resources;
		uint64_t new_bytes[POOL_COUNT] = {};
		uint32_t new_slots[POOL_COUNT] = {};
		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			if (find_commit_entry_identity(resource.key) >= 0) {
				continue;
			}
			new_resources.push_back(resource);
			const uint32_t pool = static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind));
			new_bytes[pool] = _saturating_add(new_bytes[pool], resource.bytes);
			if (new_slots[pool] != UINT32_MAX) {
				new_slots[pool]++;
			}
		}

		bool upload_cap_failed = false;
		for (uint32_t pool = 0; pool < POOL_COUNT; pool++) {
			const HybridResidencyPoolBudget &pool_budget = budgets.pools[pool];
			if (!_fits_add(diagnostics.upload_bytes[pool], new_bytes[pool], pool_budget.maximum_upload_bytes_per_frame) || new_slots[pool] > pool_budget.maximum_uploads_per_frame - MIN(pool_budget.maximum_uploads_per_frame, diagnostics.uploads[pool])) {
				for (const HybridResidencyResourceRequest &resource : new_resources) {
					if (static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind)) == pool) {
						_append_diagnostic(HybridResidencyDiagnosticReason::UPLOAD_CAP_EXCEEDED, group.request.request_id, resource);
						break;
					}
				}
				upload_cap_failed = true;
			}
		}
		if (upload_cap_failed) {
			admission.status = HybridResidencyAdmissionStatus::UPLOAD_CAP_REJECTED;
			diagnostics.rejected_requests++;
			result.admissions.push_back(admission);
			continue;
		}

		bool bytes_failed[POOL_COUNT] = {};
		bool slots_failed[POOL_COUNT] = {};
		bool budget_failed = false;
		for (uint32_t pool = 0; pool < POOL_COUNT; pool++) {
			const HybridResidencyPoolBudget &pool_budget = budgets.pools[pool];
			bytes_failed[pool] = !_fits_add(diagnostics.resident_bytes[pool], new_bytes[pool], pool_budget.maximum_resident_bytes);
			slots_failed[pool] = new_slots[pool] > pool_budget.maximum_resident_slots - MIN(pool_budget.maximum_resident_slots, diagnostics.resident_slots[pool]);
			budget_failed |= bytes_failed[pool] || slots_failed[pool];
		}

		if (budget_failed) {
			Vector<int> eviction_plan;
			bool eviction_feasible = true;
			for (uint32_t pool = 0; pool < POOL_COUNT; pool++) {
				if (!bytes_failed[pool] && !slots_failed[pool]) {
					continue;
				}
				const HybridResidencyPoolBudget &pool_budget = budgets.pools[pool];
				const uint64_t total_bytes = _saturating_add(diagnostics.resident_bytes[pool], new_bytes[pool]);
				const uint64_t needed_bytes = total_bytes > pool_budget.maximum_resident_bytes ? total_bytes - pool_budget.maximum_resident_bytes : 0;
				const uint64_t total_slots = uint64_t(diagnostics.resident_slots[pool]) + uint64_t(new_slots[pool]);
				const uint64_t needed_slots_64 = total_slots > pool_budget.maximum_resident_slots ? total_slots - pool_budget.maximum_resident_slots : 0;
				const uint32_t needed_slots = needed_slots_64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(needed_slots_64);
				Vector<_EvictionCandidate> candidates;
				for (int i = 0; i < entries.size(); i++) {
					const Entry &entry = entries[i];
					if (static_cast<uint32_t>(hybrid_residency_pool_for_kind(entry.key.kind)) != pool || entry.state == HybridResidencyState::RETIRING || entry.retention != HybridResidencyRetention::STREAMABLE || membership_contains(protected_index, entry.key)) {
						continue;
					}
					_EvictionCandidate candidate;
					candidate.entry_index = i;
					candidate.last_requested_frame = entry.last_requested_frame;
					candidate.priority = entry.priority;
					candidate.key = entry.key;
					candidates.push_back(candidate);
				}
				candidates.sort_custom<_EvictionCandidateLess>();
				uint64_t reclaimed_bytes = 0;
				uint32_t reclaimed_slots = 0;
				Vector<int> pool_plan;
				for (const _EvictionCandidate &candidate : candidates) {
					if (reclaimed_bytes >= needed_bytes && reclaimed_slots >= needed_slots) {
						break;
					}
					pool_plan.push_back(candidate.entry_index);
					reclaimed_bytes = _saturating_add(reclaimed_bytes, entries[candidate.entry_index].bytes);
					if (reclaimed_slots != UINT32_MAX) {
						reclaimed_slots++;
					}
				}
				if (reclaimed_bytes < needed_bytes || reclaimed_slots < needed_slots) {
					eviction_feasible = false;
					break;
				}
				for (int entry_index : pool_plan) {
					eviction_plan.push_back(entry_index);
				}
			}
			if (eviction_feasible) {
				for (int entry_index : eviction_plan) {
					Entry &entry = entries.write[entry_index];
					entry.state = HybridResidencyState::RETIRE_REQUESTED;
					if (!membership_contains(eviction_locked_index, entry.key)) {
						eviction_locked.push_back(entry.key);
						membership_insert(eviction_locked_index, entry.key);
					}
				}
			}
			for (uint32_t pool = 0; pool < POOL_COUNT; pool++) {
				if (!bytes_failed[pool] && !slots_failed[pool]) {
					continue;
				}
				for (const HybridResidencyResourceRequest &resource : new_resources) {
					if (static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind)) != pool) {
						continue;
					}
					_append_diagnostic(bytes_failed[pool] ? HybridResidencyDiagnosticReason::POOL_BYTES_EXCEEDED : HybridResidencyDiagnosticReason::POOL_SLOTS_EXCEEDED, group.request.request_id, resource);
					break;
				}
			}
			admission.status = HybridResidencyAdmissionStatus::BUDGET_REJECTED;
			diagnostics.budget_rejections++;
			diagnostics.rejected_requests++;
			result.admissions.push_back(admission);
			continue;
		}

		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			const int entry_index = find_commit_entry_identity(resource.key);
			if (entry_index < 0) {
				continue;
			}
			Entry &entry = entries.write[entry_index];
			entry.state = HybridResidencyState::RESIDENT;
			entry.last_requested_frame = current_frame;
			entry.priority = group.request.priority;
			if (static_cast<uint32_t>(resource.retention) > static_cast<uint32_t>(entry.retention)) {
				entry.retention = resource.retention;
			}
		}

		for (const HybridResidencyResourceRequest &resource : new_resources) {
			const uint32_t pool = static_cast<uint32_t>(hybrid_residency_pool_for_kind(resource.key.kind));
			Vector<uint32_t> used_slots;
			for (const Entry &entry : entries) {
				if (static_cast<uint32_t>(hybrid_residency_pool_for_kind(entry.key.kind)) == pool) {
					used_slots.push_back(entry.slot);
				}
			}
			used_slots.sort();
			uint32_t slot = 0;
			for (uint32_t used_slot : used_slots) {
				if (used_slot == slot) {
					slot++;
				} else if (used_slot > slot) {
					break;
				}
			}
			ERR_CONTINUE_MSG(slot >= budgets.pools[pool].maximum_resident_slots, "Hybrid residency slot allocation violated a prevalidated pool budget.");
			Entry entry;
			entry.key = resource.key;
			entry.bytes = resource.bytes;
			entry.retention = resource.retention;
			entry.state = HybridResidencyState::RESIDENT;
			entry.last_requested_frame = current_frame;
			entry.priority = group.request.priority;
			entry.slot = slot;
			entries.push_back(entry);
			entry_identity_index[identity_hash(entry.key)].push_back(entries.size() - 1);

			HybridResidencyAllocation allocation;
			allocation.key = resource.key;
			allocation.pool = static_cast<HybridResidencyPool>(pool);
			allocation.bytes = resource.bytes;
			allocation.slot = slot;
			allocation.eye_mask = group.request.eye_mask;
			int lower = 0;
			int upper = resource_eye_unions.size();
			while (lower < upper) {
				const int middle = lower + (upper - lower) / 2;
				if (_key_less(resource_eye_unions[middle].key, resource.key)) lower = middle + 1;
				else upper = middle;
			}
			if (lower < resource_eye_unions.size() && _key_equal(resource_eye_unions[lower].key, resource.key)) allocation.eye_mask = resource_eye_unions[lower].eye_mask;
			result.allocations.push_back(allocation);
			diagnostics.upload_bytes[pool] += resource.bytes;
			diagnostics.uploads[pool]++;
			diagnostics.resident_bytes[pool] += resource.bytes;
			diagnostics.resident_slots[pool]++;
		}

		for (const HybridResidencyResourceRequest &resource : group.request.resources) {
			if (!membership_contains(protected_index, resource.key)) {
				protected_resources.push_back(resource.key);
				membership_insert(protected_index, resource.key);
			}
		}
		admission.status = HybridResidencyAdmissionStatus::ADMITTED;
		diagnostics.admitted_requests++;
		result.admissions.push_back(admission);
	}

	for (const Entry &entry : entries) {
		if (entry.state != HybridResidencyState::RETIRE_REQUESTED) {
			continue;
		}
		HybridResidencyRetirement retirement;
		retirement.key = entry.key;
		retirement.pool = hybrid_residency_pool_for_kind(entry.key.kind);
		retirement.bytes = entry.bytes;
		retirement.slot = entry.slot;
		result.retirements.push_back(retirement);
	}
	result.retirements.sort_custom<_RetirementLess>();
	_refresh_usage();
	_rebuild_entry_identity_index();
	return result;
}

Error HybridResidencyPlanner::retire(const HybridResidencyResourceKey &p_key, uint64_t p_completion_token) {
	ERR_FAIL_COND_V_MSG(p_completion_token == 0 || p_completion_token <= completed_retirement_token, ERR_INVALID_PARAMETER, "Hybrid residency retirement requires a future nonzero completion token.");
	const int entry_index = _find_entry_identity(p_key);
	ERR_FAIL_COND_V_MSG(entry_index < 0, ERR_DOES_NOT_EXIST, "Hybrid residency resource does not exist.");
	Entry &entry = entries.write[entry_index];
	ERR_FAIL_COND_V_MSG(entry.key.generation != p_key.generation, ERR_INVALID_DATA, "Hybrid residency retirement generation is stale.");
	ERR_FAIL_COND_V_MSG(entry.state == HybridResidencyState::RETIRING, ERR_ALREADY_IN_USE, "Hybrid residency resource is already retiring.");
	entry.state = HybridResidencyState::RETIRING;
	entry.retirement_completion_token = p_completion_token;
	return OK;
}

HybridResidencyQuery HybridResidencyPlanner::query(const HybridResidencyResourceKey &p_key) const {
	HybridResidencyQuery result;
	result.requested_key = p_key;
	if (_kind_valid(p_key.kind)) {
		result.pool = hybrid_residency_pool_for_kind(p_key.kind);
	}
	const int entry_index = _find_entry_identity(p_key);
	if (entry_index < 0) {
		return result;
	}
	const Entry &entry = entries[entry_index];
	result.resident_key = entry.key;
	result.bytes = entry.bytes;
	result.slot = entry.slot;
	result.retirement_completion_token = entry.retirement_completion_token;
	if (entry.key.generation != p_key.generation) {
		result.state = HybridResidencyState::STALE;
		return result;
	}
	result.state = entry.state;
	result.fail_open_required = entry.state == HybridResidencyState::RETIRING;
	return result;
}

} // namespace RendererPathTracing
