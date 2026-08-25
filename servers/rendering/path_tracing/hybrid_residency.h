/**************************************************************************/
/*  hybrid_residency.h                                                    */
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
#include "core/templates/vector.h"
#include "core/templates/hash_map.h"

#include <cstdint>

namespace RendererPathTracing {

// Backend-neutral residency planning contract. Screen-space history is not
// represented here: a caller submits the conservative union of visible eyes.
static constexpr uint32_t HYBRID_RESIDENCY_ABI_VERSION = 1;
static constexpr uint8_t HYBRID_RESIDENCY_EYE_LEFT = 1u << 0;
static constexpr uint8_t HYBRID_RESIDENCY_EYE_RIGHT = 1u << 1;
static constexpr uint8_t HYBRID_RESIDENCY_EYE_STEREO = HYBRID_RESIDENCY_EYE_LEFT | HYBRID_RESIDENCY_EYE_RIGHT;
static constexpr uint32_t HYBRID_RESIDENCY_INVALID_SLOT = UINT32_MAX;

enum class HybridResidencyResourceKind : uint8_t {
	GEOMETRY_CLUSTER_PAGE,
	TEXTURE_MIP,
	MATERIAL_DESCRIPTOR,
	BLAS,
	TLAS_INSTANCE,
	MAX,
};

enum class HybridResidencyTextureChannel : uint8_t {
	NONE,
	ALBEDO,
	NORMAL,
	ORM,
	EMISSIVE,
	OPACITY,
	ALPHA_OCCUPANCY,
	MAX,
};

// Each resource class has an independent hard byte and slot budget. Keeping
// BLAS and TLAS instances separate avoids hiding either cost in geometry.
enum class HybridResidencyPool : uint8_t {
	GEOMETRY_CLUSTER_PAGES,
	TEXTURE_MIPS,
	MATERIAL_DESCRIPTORS,
	BLAS,
	TLAS_INSTANCES,
	MAX,
};

enum class HybridResidencyRetention : uint8_t {
	STREAMABLE,
	COARSE,
	ALWAYS_RESIDENT,
};

enum class HybridResidencyState : uint8_t {
	MISSING,
	STALE,
	RESIDENT,
	RETIRE_REQUESTED,
	RETIRING,
};

enum class HybridResidencyAdmissionStatus : uint8_t {
	ADMITTED,
	MISSING_RESOURCE,
	STALE_RESOURCE,
	GENERATION_MISMATCH,
	METADATA_MISMATCH,
	REQUEST_CAP_REJECTED,
	UPLOAD_CAP_REJECTED,
	BUDGET_REJECTED,
};

enum class HybridResidencyDiagnosticReason : uint8_t {
	MISSING_RESOURCE,
	STALE_GENERATION,
	GENERATION_MISMATCH,
	METADATA_MISMATCH,
	REQUEST_CAP_EXCEEDED,
	UPLOAD_CAP_EXCEEDED,
	POOL_BYTES_EXCEEDED,
	POOL_SLOTS_EXCEEDED,
};

struct HybridResidencyResourceKey {
	uint64_t stable_id = 0;
	uint64_t generation = 0;
	HybridResidencyResourceKind kind = HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE;
	HybridResidencyTextureChannel texture_channel = HybridResidencyTextureChannel::NONE;
	uint16_t mip_tier = 0;
};

struct HybridResidencyResourceRequest {
	uint32_t abi_version = HYBRID_RESIDENCY_ABI_VERSION;
	HybridResidencyResourceKey key;
	uint64_t bytes = 0;
	HybridResidencyRetention retention = HybridResidencyRetention::STREAMABLE;
	// A false value represents a known absent source/resource. It is retained
	// in the request so the planner can produce an exact fail-open diagnostic.
	bool available = true;
};

// Resources in one request are coupled: the request is admitted only when
// every member is resident or receives an allocation in the same commit.
struct HybridResidencyRequest {
	uint32_t abi_version = HYBRID_RESIDENCY_ABI_VERSION;
	uint64_t request_id = 0;
	uint32_t priority = 0;
	uint8_t eye_mask = 0;
	Vector<HybridResidencyResourceRequest> resources;
};

struct HybridResidencyPoolBudget {
	uint64_t maximum_resident_bytes = 256ull * 1024ull * 1024ull;
	uint64_t maximum_upload_bytes_per_frame = 32ull * 1024ull * 1024ull;
	uint32_t maximum_resident_slots = 4096;
	uint32_t maximum_requests_per_frame = 16384;
	uint32_t maximum_uploads_per_frame = 1024;
};

struct HybridResidencyBudgets {
	uint32_t abi_version = HYBRID_RESIDENCY_ABI_VERSION;
	HybridResidencyPoolBudget pools[static_cast<uint32_t>(HybridResidencyPool::MAX)];
};

struct HybridResidencyAllocation {
	HybridResidencyResourceKey key;
	HybridResidencyPool pool = HybridResidencyPool::GEOMETRY_CLUSTER_PAGES;
	uint64_t bytes = 0;
	uint32_t slot = HYBRID_RESIDENCY_INVALID_SLOT;
	uint8_t eye_mask = 0;
};

struct HybridResidencyRetirement {
	HybridResidencyResourceKey key;
	HybridResidencyPool pool = HybridResidencyPool::GEOMETRY_CLUSTER_PAGES;
	uint64_t bytes = 0;
	uint32_t slot = HYBRID_RESIDENCY_INVALID_SLOT;
};

struct HybridResidencyAdmission {
	uint64_t request_id = 0;
	uint8_t eye_mask = 0;
	HybridResidencyAdmissionStatus status = HybridResidencyAdmissionStatus::ADMITTED;
};

struct HybridResidencyDiagnostic {
	HybridResidencyDiagnosticReason reason = HybridResidencyDiagnosticReason::MISSING_RESOURCE;
	uint64_t request_id = 0;
	HybridResidencyResourceKey key;
	HybridResidencyPool pool = HybridResidencyPool::GEOMETRY_CLUSTER_PAGES;
	uint64_t requested_bytes = 0;
	uint64_t resident_generation = 0;
	bool fail_open_required = true;
};

struct HybridResidencyFrameDiagnostics {
	uint64_t frame = 0;
	uint32_t submitted_requests = 0;
	uint32_t merged_requests = 0;
	uint32_t admitted_requests = 0;
	uint32_t rejected_requests = 0;
	uint32_t deduplicated_resources = 0;
	uint32_t stale_resources = 0;
	uint32_t missing_resources = 0;
	uint32_t budget_rejections = 0;
	uint32_t retired_resources = 0;
	uint64_t resident_bytes[static_cast<uint32_t>(HybridResidencyPool::MAX)] = {};
	uint32_t resident_slots[static_cast<uint32_t>(HybridResidencyPool::MAX)] = {};
	uint64_t upload_bytes[static_cast<uint32_t>(HybridResidencyPool::MAX)] = {};
	uint32_t uploads[static_cast<uint32_t>(HybridResidencyPool::MAX)] = {};
	Vector<HybridResidencyDiagnostic> events;
};

struct HybridResidencyCommitResult {
	Vector<HybridResidencyAllocation> allocations;
	Vector<HybridResidencyRetirement> retirements;
	Vector<HybridResidencyAdmission> admissions;
};

struct HybridResidencyQuery {
	HybridResidencyState state = HybridResidencyState::MISSING;
	HybridResidencyResourceKey requested_key;
	HybridResidencyResourceKey resident_key;
	HybridResidencyPool pool = HybridResidencyPool::GEOMETRY_CLUSTER_PAGES;
	uint64_t bytes = 0;
	uint32_t slot = HYBRID_RESIDENCY_INVALID_SLOT;
	uint64_t retirement_completion_token = 0;
	bool fail_open_required = true;
};

HybridResidencyPool hybrid_residency_pool_for_kind(HybridResidencyResourceKind p_kind);

class HybridResidencyPlanner {
	struct Entry {
		HybridResidencyResourceKey key;
		uint64_t bytes = 0;
		HybridResidencyRetention retention = HybridResidencyRetention::STREAMABLE;
		HybridResidencyState state = HybridResidencyState::RESIDENT;
		uint64_t last_requested_frame = 0;
		uint64_t retirement_completion_token = 0;
		uint32_t priority = 0;
		uint32_t slot = HYBRID_RESIDENCY_INVALID_SLOT;
	};

	Vector<Entry> entries;
	HashMap<uint64_t, Vector<int>> entry_identity_index;
	Vector<HybridResidencyRequest> pending_requests;
	HybridResidencyBudgets budgets;
	HybridResidencyFrameDiagnostics diagnostics;
	uint64_t current_frame = 0;
	uint64_t completed_retirement_token = 0;
	bool frame_open = false;
	bool frame_committed = false;

	int _find_entry_identity(const HybridResidencyResourceKey &p_key) const;
	void _rebuild_entry_identity_index();
	void _refresh_usage();
	void _append_diagnostic(HybridResidencyDiagnosticReason p_reason, uint64_t p_request_id, const HybridResidencyResourceRequest &p_resource, uint64_t p_resident_generation = 0);

public:
	void clear();
	Error set_budgets(const HybridResidencyBudgets &p_budgets);
	const HybridResidencyBudgets &get_budgets() const { return budgets; }

	// Completion tokens are caller-owned monotonically increasing GPU/fence
	// values. Retiring slots remain occupied until the supplied value reaches
	// the token passed to retire().
	Error begin_frame(uint64_t p_frame, uint64_t p_completed_retirement_token);
	// Advances a fully resident, previously committed request set without
	// rebuilding its admission/slot plan. The caller must provide the exact
	// visible resource keys from that committed plan; any missing, stale, or
	// retiring entry rejects the replay so the caller can perform a full frame.
	bool reuse_committed_frame(uint64_t p_frame, uint64_t p_completed_retirement_token, const Vector<HybridResidencyResourceKey> &p_visible_keys);
	Error request(const HybridResidencyRequest &p_request);
	HybridResidencyCommitResult commit();
	Error retire(const HybridResidencyResourceKey &p_key, uint64_t p_completion_token);
	HybridResidencyQuery query(const HybridResidencyResourceKey &p_key) const;

	const HybridResidencyFrameDiagnostics &get_diagnostics() const { return diagnostics; }
};

} // namespace RendererPathTracing
