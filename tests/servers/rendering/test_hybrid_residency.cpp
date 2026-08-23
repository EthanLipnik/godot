/**************************************************************************/
/*  test_hybrid_residency.cpp                                             */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_hybrid_residency)

#include "servers/rendering/path_tracing/hybrid_residency.h"

namespace TestHybridResidency {

using namespace RendererPathTracing;

static HybridResidencyResourceRequest _resource(HybridResidencyResourceKind p_kind, uint64_t p_id, uint64_t p_generation, uint64_t p_bytes, HybridResidencyRetention p_retention = HybridResidencyRetention::STREAMABLE, HybridResidencyTextureChannel p_channel = HybridResidencyTextureChannel::NONE, uint16_t p_mip_tier = 0) {
	HybridResidencyResourceRequest resource;
	resource.key.kind = p_kind;
	resource.key.stable_id = p_id;
	resource.key.generation = p_generation;
	resource.key.texture_channel = p_channel;
	resource.key.mip_tier = p_mip_tier;
	resource.bytes = p_bytes;
	resource.retention = p_retention;
	return resource;
}

static HybridResidencyRequest _request(uint64_t p_id, uint32_t p_priority, uint8_t p_eye_mask, const Vector<HybridResidencyResourceRequest> &p_resources) {
	HybridResidencyRequest request;
	request.request_id = p_id;
	request.priority = p_priority;
	request.eye_mask = p_eye_mask;
	request.resources = p_resources;
	return request;
}

static HybridResidencyBudgets _budgets(uint64_t p_bytes = 1024, uint32_t p_slots = 16, uint32_t p_uploads = 16) {
	HybridResidencyBudgets budgets;
	for (uint32_t i = 0; i < static_cast<uint32_t>(HybridResidencyPool::MAX); i++) {
		budgets.pools[i].maximum_resident_bytes = p_bytes;
		budgets.pools[i].maximum_upload_bytes_per_frame = p_bytes;
		budgets.pools[i].maximum_resident_slots = p_slots;
		budgets.pools[i].maximum_requests_per_frame = 64;
		budgets.pools[i].maximum_uploads_per_frame = p_uploads;
	}
	return budgets;
}

TEST_CASE("[PathTracing][HybridResidency] Stereo requests form a conservative resource union") {
	HybridResidencyPlanner planner;
	CHECK(planner.set_budgets(_budgets()) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);

	const HybridResidencyResourceRequest geometry = _resource(HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE, 10, 1, 64);
	Vector<HybridResidencyResourceRequest> left;
	left.push_back(geometry);
	Vector<HybridResidencyResourceRequest> right;
	right.push_back(geometry);
	CHECK(planner.request(_request(100, 10, HYBRID_RESIDENCY_EYE_LEFT, left)) == OK);
	CHECK(planner.request(_request(200, 10, HYBRID_RESIDENCY_EYE_RIGHT, right)) == OK);

	const HybridResidencyCommitResult result = planner.commit();
	REQUIRE(result.admissions.size() == 2);
	CHECK(result.admissions[0].status == HybridResidencyAdmissionStatus::ADMITTED);
	CHECK(result.admissions[0].eye_mask == HYBRID_RESIDENCY_EYE_LEFT);
	CHECK(result.admissions[1].status == HybridResidencyAdmissionStatus::ADMITTED);
	CHECK(result.admissions[1].eye_mask == HYBRID_RESIDENCY_EYE_RIGHT);
	REQUIRE(result.allocations.size() == 1);
	CHECK(result.allocations[0].eye_mask == HYBRID_RESIDENCY_EYE_STEREO);
	CHECK(planner.get_diagnostics().deduplicated_resources >= 1);
}

TEST_CASE("[PathTracing][HybridResidency] Coupled resource kinds fail open atomically on an exact missing texture") {
	HybridResidencyPlanner planner;
	CHECK(planner.set_budgets(_budgets()) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);
	Vector<HybridResidencyResourceRequest> resources;
	resources.push_back(_resource(HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE, 1, 1, 64));
	resources.push_back(_resource(HybridResidencyResourceKind::TEXTURE_MIP, 2, 1, 32, HybridResidencyRetention::STREAMABLE, HybridResidencyTextureChannel::OPACITY, 2));
	resources.push_back(_resource(HybridResidencyResourceKind::MATERIAL_DESCRIPTOR, 3, 1, 16));
	resources.push_back(_resource(HybridResidencyResourceKind::BLAS, 4, 1, 128));
	resources.push_back(_resource(HybridResidencyResourceKind::TLAS_INSTANCE, 5, 1, 16));
	resources.write[1].available = false;
	CHECK(planner.request(_request(1, 1, HYBRID_RESIDENCY_EYE_STEREO, resources)) == OK);
	const HybridResidencyCommitResult result = planner.commit();
	CHECK(result.allocations.is_empty());
	REQUIRE(result.admissions.size() == 1);
	CHECK(result.admissions[0].status == HybridResidencyAdmissionStatus::MISSING_RESOURCE);
	REQUIRE(planner.get_diagnostics().events.size() == 1);
	CHECK(planner.get_diagnostics().events[0].reason == HybridResidencyDiagnosticReason::MISSING_RESOURCE);
	CHECK(planner.get_diagnostics().events[0].key.stable_id == 2);
	CHECK(planner.get_diagnostics().events[0].key.texture_channel == HybridResidencyTextureChannel::OPACITY);
	CHECK(planner.get_diagnostics().events[0].fail_open_required);
	CHECK(planner.query(resources[0].key).state == HybridResidencyState::MISSING);
}

TEST_CASE("[PathTracing][HybridResidency] Priority and deduplication are deterministic under upload caps") {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets();
	const uint32_t texture_pool = static_cast<uint32_t>(HybridResidencyPool::TEXTURE_MIPS);
	budgets.pools[texture_pool].maximum_uploads_per_frame = 1;
	CHECK(planner.set_budgets(budgets) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);

	Vector<HybridResidencyResourceRequest> low_resources;
	low_resources.push_back(_resource(HybridResidencyResourceKind::TEXTURE_MIP, 10, 1, 64, HybridResidencyRetention::STREAMABLE, HybridResidencyTextureChannel::ALBEDO, 1));
	Vector<HybridResidencyResourceRequest> high_resources;
	high_resources.push_back(_resource(HybridResidencyResourceKind::TEXTURE_MIP, 20, 1, 64, HybridResidencyRetention::STREAMABLE, HybridResidencyTextureChannel::NORMAL, 1));
	CHECK(planner.request(_request(10, 1, HYBRID_RESIDENCY_EYE_LEFT, low_resources)) == OK);
	CHECK(planner.request(_request(20, 100, HYBRID_RESIDENCY_EYE_LEFT, high_resources)) == OK);
	CHECK(planner.request(_request(20, 100, HYBRID_RESIDENCY_EYE_LEFT, high_resources)) == OK);

	const HybridResidencyCommitResult result = planner.commit();
	REQUIRE(result.allocations.size() == 1);
	CHECK(result.allocations[0].key.stable_id == 20);
	REQUIRE(result.admissions.size() == 2);
	CHECK(result.admissions[0].request_id == 20);
	CHECK(result.admissions[0].status == HybridResidencyAdmissionStatus::ADMITTED);
	CHECK(result.admissions[1].request_id == 10);
	CHECK(result.admissions[1].status == HybridResidencyAdmissionStatus::UPLOAD_CAP_REJECTED);
	CHECK(planner.get_diagnostics().deduplicated_resources >= 1);
}

TEST_CASE("[PathTracing][HybridResidency] Per-pool request caps keep the highest-priority request") {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets();
	const uint32_t material_pool = static_cast<uint32_t>(HybridResidencyPool::MATERIAL_DESCRIPTORS);
	budgets.pools[material_pool].maximum_requests_per_frame = 1;
	CHECK(planner.set_budgets(budgets) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);
	Vector<HybridResidencyResourceRequest> low;
	low.push_back(_resource(HybridResidencyResourceKind::MATERIAL_DESCRIPTOR, 1, 1, 16));
	Vector<HybridResidencyResourceRequest> high;
	high.push_back(_resource(HybridResidencyResourceKind::MATERIAL_DESCRIPTOR, 2, 1, 16));
	CHECK(planner.request(_request(1, 1, HYBRID_RESIDENCY_EYE_LEFT, low)) == OK);
	CHECK(planner.request(_request(2, 2, HYBRID_RESIDENCY_EYE_LEFT, high)) == OK);
	const HybridResidencyCommitResult result = planner.commit();
	REQUIRE(result.allocations.size() == 1);
	CHECK(result.allocations[0].key.stable_id == 2);
	REQUIRE(result.admissions.size() == 2);
	CHECK(result.admissions[0].status == HybridResidencyAdmissionStatus::ADMITTED);
	CHECK(result.admissions[1].status == HybridResidencyAdmissionStatus::REQUEST_CAP_REJECTED);
	REQUIRE(planner.get_diagnostics().events.size() == 1);
	CHECK(planner.get_diagnostics().events[0].reason == HybridResidencyDiagnosticReason::REQUEST_CAP_EXCEEDED);
}

TEST_CASE("[PathTracing][HybridResidency] Hard pool budgets reject without overcommit") {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets();
	const uint32_t geometry_pool = static_cast<uint32_t>(HybridResidencyPool::GEOMETRY_CLUSTER_PAGES);
	budgets.pools[geometry_pool].maximum_resident_bytes = 32;
	CHECK(planner.set_budgets(budgets) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);
	Vector<HybridResidencyResourceRequest> resources;
	resources.push_back(_resource(HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE, 10, 1, 64));
	CHECK(planner.request(_request(1, 1, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	const HybridResidencyCommitResult result = planner.commit();
	CHECK(result.allocations.is_empty());
	REQUIRE(result.admissions.size() == 1);
	CHECK(result.admissions[0].status == HybridResidencyAdmissionStatus::BUDGET_REJECTED);
	CHECK(planner.get_diagnostics().resident_bytes[geometry_pool] == 0);
	CHECK(planner.get_diagnostics().budget_rejections == 1);
	REQUIRE(planner.get_diagnostics().events.size() == 1);
	CHECK(planner.get_diagnostics().events[0].reason == HybridResidencyDiagnosticReason::POOL_BYTES_EXCEEDED);
}

TEST_CASE("[PathTracing][HybridResidency] Generation changes are stale until old storage retires") {
	HybridResidencyPlanner planner;
	CHECK(planner.set_budgets(_budgets()) == OK);
	const HybridResidencyResourceRequest generation_one = _resource(HybridResidencyResourceKind::BLAS, 40, 1, 128);
	Vector<HybridResidencyResourceRequest> resources;
	resources.push_back(generation_one);
	CHECK(planner.begin_frame(1, 0) == OK);
	CHECK(planner.request(_request(1, 1, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	CHECK(planner.commit().allocations.size() == 1);

	HybridResidencyResourceRequest generation_two = generation_one;
	generation_two.key.generation = 2;
	resources.write[0] = generation_two;
	CHECK(planner.begin_frame(2, 0) == OK);
	CHECK(planner.request(_request(2, 1, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	const HybridResidencyCommitResult stale = planner.commit();
	CHECK(stale.allocations.is_empty());
	REQUIRE(stale.admissions.size() == 1);
	CHECK(stale.admissions[0].status == HybridResidencyAdmissionStatus::STALE_RESOURCE);
	REQUIRE(stale.retirements.size() == 1);
	CHECK(stale.retirements[0].key.generation == 1);
	const HybridResidencyQuery query = planner.query(generation_two.key);
	CHECK(query.state == HybridResidencyState::STALE);
	CHECK(query.resident_key.generation == 1);
	CHECK(query.fail_open_required);
	REQUIRE(planner.get_diagnostics().events.size() == 1);
	CHECK(planner.get_diagnostics().events[0].reason == HybridResidencyDiagnosticReason::STALE_GENERATION);
	CHECK(planner.get_diagnostics().events[0].resident_generation == 1);

	HybridResidencyPlanner mismatch_planner;
	CHECK(mismatch_planner.set_budgets(_budgets()) == OK);
	CHECK(mismatch_planner.begin_frame(1, 0) == OK);
	Vector<HybridResidencyResourceRequest> first;
	first.push_back(generation_one);
	Vector<HybridResidencyResourceRequest> second;
	second.push_back(generation_two);
	CHECK(mismatch_planner.request(_request(8, 1, HYBRID_RESIDENCY_EYE_LEFT, first)) == OK);
	CHECK(mismatch_planner.request(_request(8, 1, HYBRID_RESIDENCY_EYE_RIGHT, second)) == OK);
	const HybridResidencyCommitResult mismatch = mismatch_planner.commit();
	CHECK(mismatch.allocations.is_empty());
	CHECK(mismatch.admissions[0].status == HybridResidencyAdmissionStatus::GENERATION_MISMATCH);
	CHECK(mismatch_planner.get_diagnostics().events[0].reason == HybridResidencyDiagnosticReason::GENERATION_MISMATCH);
}

TEST_CASE("[PathTracing][HybridResidency] Retiring slots cannot be reused before completion") {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets(64, 1, 4);
	CHECK(planner.set_budgets(budgets) == OK);
	const HybridResidencyResourceRequest first_resource = _resource(HybridResidencyResourceKind::TEXTURE_MIP, 1, 1, 32, HybridResidencyRetention::STREAMABLE, HybridResidencyTextureChannel::ALBEDO, 0);
	const HybridResidencyResourceRequest second_resource = _resource(HybridResidencyResourceKind::TEXTURE_MIP, 2, 1, 32, HybridResidencyRetention::STREAMABLE, HybridResidencyTextureChannel::ALBEDO, 0);
	Vector<HybridResidencyResourceRequest> resources;
	resources.push_back(first_resource);
	CHECK(planner.begin_frame(1, 0) == OK);
	CHECK(planner.request(_request(1, 1, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	const HybridResidencyCommitResult first = planner.commit();
	REQUIRE(first.allocations.size() == 1);
	CHECK(first.allocations[0].slot == 0);

	resources.write[0] = second_resource;
	CHECK(planner.begin_frame(2, 0) == OK);
	CHECK(planner.request(_request(2, 2, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	const HybridResidencyCommitResult eviction = planner.commit();
	CHECK(eviction.allocations.is_empty());
	REQUIRE(eviction.retirements.size() == 1);
	CHECK(eviction.retirements[0].key.stable_id == 1);
	CHECK(planner.retire(first_resource.key, 10) == OK);

	CHECK(planner.begin_frame(3, 9) == OK);
	CHECK(planner.query(first_resource.key).state == HybridResidencyState::RETIRING);
	CHECK(planner.request(_request(2, 2, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	CHECK(planner.commit().allocations.is_empty());

	CHECK(planner.begin_frame(4, 10) == OK);
	CHECK(planner.query(first_resource.key).state == HybridResidencyState::MISSING);
	CHECK(planner.request(_request(2, 2, HYBRID_RESIDENCY_EYE_LEFT, resources)) == OK);
	const HybridResidencyCommitResult replacement = planner.commit();
	REQUIRE(replacement.allocations.size() == 1);
	CHECK(replacement.allocations[0].key.stable_id == 2);
	CHECK(replacement.allocations[0].slot == 0);
}

TEST_CASE("[PathTracing][HybridResidency] Coarse and always-resident tiers are retained automatically") {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets(64, 2, 4);
	CHECK(planner.set_budgets(budgets) == OK);
	Vector<HybridResidencyResourceRequest> retained;
	retained.push_back(_resource(HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE, 1, 1, 32, HybridResidencyRetention::COARSE));
	retained.push_back(_resource(HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE, 2, 1, 32, HybridResidencyRetention::ALWAYS_RESIDENT));
	CHECK(planner.begin_frame(1, 0) == OK);
	CHECK(planner.request(_request(1, 1, HYBRID_RESIDENCY_EYE_LEFT, retained)) == OK);
	CHECK(planner.commit().allocations.size() == 2);

	Vector<HybridResidencyResourceRequest> replacement;
	replacement.push_back(_resource(HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE, 3, 1, 32));
	CHECK(planner.begin_frame(2, 0) == OK);
	CHECK(planner.request(_request(2, 100, HYBRID_RESIDENCY_EYE_LEFT, replacement)) == OK);
	const HybridResidencyCommitResult result = planner.commit();
	CHECK(result.allocations.is_empty());
	CHECK(result.retirements.is_empty());
	CHECK(result.admissions[0].status == HybridResidencyAdmissionStatus::BUDGET_REJECTED);
	CHECK(planner.query(retained[0].key).state == HybridResidencyState::RESIDENT);
	CHECK(planner.query(retained[1].key).state == HybridResidencyState::RESIDENT);
}

static HybridResidencyCommitResult _replay(bool p_reverse) {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets();
	const uint32_t material_pool = static_cast<uint32_t>(HybridResidencyPool::MATERIAL_DESCRIPTORS);
	budgets.pools[material_pool].maximum_uploads_per_frame = 2;
	CHECK(planner.set_budgets(budgets) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);
	Vector<HybridResidencyRequest> requests;
	for (uint64_t id : { uint64_t(30), uint64_t(10), uint64_t(20) }) {
		Vector<HybridResidencyResourceRequest> resources;
		resources.push_back(_resource(HybridResidencyResourceKind::MATERIAL_DESCRIPTOR, id, 1, 16));
		requests.push_back(_request(id, id == 30 ? 2 : 1, HYBRID_RESIDENCY_EYE_LEFT, resources));
	}
	if (p_reverse) {
		requests.reverse();
	}
	for (const HybridResidencyRequest &request : requests) {
		CHECK(planner.request(request) == OK);
	}
	return planner.commit();
}

static HybridResidencyCommitResult _eviction_replay(bool p_reverse) {
	HybridResidencyPlanner planner;
	HybridResidencyBudgets budgets = _budgets(64, 2, 4);
	CHECK(planner.set_budgets(budgets) == OK);
	CHECK(planner.begin_frame(1, 0) == OK);
	Vector<HybridResidencyRequest> initial;
	for (uint64_t id : { uint64_t(20), uint64_t(10) }) {
		Vector<HybridResidencyResourceRequest> resources;
		resources.push_back(_resource(HybridResidencyResourceKind::BLAS, id, 1, 32));
		initial.push_back(_request(id, 1, HYBRID_RESIDENCY_EYE_LEFT, resources));
	}
	if (p_reverse) {
		initial.reverse();
	}
	for (const HybridResidencyRequest &request : initial) {
		CHECK(planner.request(request) == OK);
	}
	CHECK(planner.commit().allocations.size() == 2);
	CHECK(planner.begin_frame(2, 0) == OK);
	Vector<HybridResidencyResourceRequest> replacement;
	replacement.push_back(_resource(HybridResidencyResourceKind::BLAS, 30, 1, 32));
	CHECK(planner.request(_request(30, 100, HYBRID_RESIDENCY_EYE_LEFT, replacement)) == OK);
	return planner.commit();
}

TEST_CASE("[PathTracing][HybridResidency] Replay is independent of submission order") {
	const HybridResidencyCommitResult forward = _replay(false);
	const HybridResidencyCommitResult reverse = _replay(true);
	REQUIRE(forward.allocations.size() == reverse.allocations.size());
	REQUIRE(forward.admissions.size() == reverse.admissions.size());
	for (int i = 0; i < forward.allocations.size(); i++) {
		CHECK(forward.allocations[i].key.stable_id == reverse.allocations[i].key.stable_id);
		CHECK(forward.allocations[i].slot == reverse.allocations[i].slot);
	}
	for (int i = 0; i < forward.admissions.size(); i++) {
		CHECK(forward.admissions[i].request_id == reverse.admissions[i].request_id);
		CHECK(forward.admissions[i].status == reverse.admissions[i].status);
	}
	REQUIRE(forward.allocations.size() == 2);
	CHECK(forward.allocations[0].key.stable_id == 30);
	CHECK(forward.allocations[1].key.stable_id == 10);

	const HybridResidencyCommitResult forward_eviction = _eviction_replay(false);
	const HybridResidencyCommitResult reverse_eviction = _eviction_replay(true);
	CHECK(forward_eviction.allocations.is_empty());
	REQUIRE(forward_eviction.retirements.size() == 1);
	REQUIRE(reverse_eviction.retirements.size() == 1);
	CHECK(forward_eviction.retirements[0].key.stable_id == 10);
	CHECK(reverse_eviction.retirements[0].key.stable_id == forward_eviction.retirements[0].key.stable_id);
	CHECK(reverse_eviction.retirements[0].slot == forward_eviction.retirements[0].slot);
}

} // namespace TestHybridResidency
