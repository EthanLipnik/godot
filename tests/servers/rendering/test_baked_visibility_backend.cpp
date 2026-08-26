/**************************************************************************/
/*  test_baked_visibility_backend.cpp                                     */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_baked_visibility_backend)

#include "servers/rendering/baked_visibility/backend/baked_visibility_backend.h"
#include "servers/rendering/baked_visibility/backend/baked_visibility_backend_vulkan.h"

#ifdef METAL_ENABLED
#include "drivers/metal/rendering_context_driver_metal.h"
#include "servers/rendering/rendering_device.h"
#endif

TEST_CASE("[Rendering][BakedVisibility] acceleration capability fallback is truthful") {
	const BakedVisibilityBackendCapabilities cpu = BakedVisibilityBackend::probe(BakedVisibilityBackendKind::CPU_REFERENCE);
	CHECK(cpu.available);
	CHECK(cpu.can_certify_invisibility);
	const BakedVisibilityBackendCapabilities vulkan = BakedVisibilityBackend::probe(BakedVisibilityBackendKind::VULKAN);
	CHECK_FALSE(vulkan.available);
	CHECK_FALSE(vulkan.can_certify_invisibility);
	const BakedVisibilityBackendCapabilities metal = BakedVisibilityBackend::probe(BakedVisibilityBackendKind::METAL);
	CHECK_FALSE(metal.can_certify_invisibility);
	CHECK_FALSE(metal.executed);
	const BakedVisibilityBackendCapabilities invalid = BakedVisibilityBackend::probe(static_cast<BakedVisibilityBackendKind>(255));
	CHECK_FALSE(invalid.available);
	CHECK_FALSE(invalid.can_certify_invisibility);
}

TEST_CASE("[Rendering][BakedVisibility] CPU batch mask compaction and blocker oracle are canonical") {
	BakedVisibilityBackendBatchInput input;
	input.query_bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	input.candidates.push_back({ AABB(Vector3(4, 0, 0), Vector3(1, 1, 1)), 30 });
	input.candidates.push_back({ AABB(Vector3(0, 0, 0), Vector3(1, 1, 1)), 20 });
	input.candidates.push_back({ AABB(Vector3(-0.5f, 0, 0), Vector3(1, 1, 1)), 10 });
	input.blockers.push_back({ AABB(Vector3(0.15f, -1, -1), Vector3(0.1f, 2, 2)) });

	BakedVisibilityBackendBatchOutput output;
	CHECK_EQ(BakedVisibilityBackend::execute_cpu_reference(input, output), OK);
	REQUIRE_EQ(output.candidate_mask.size(), 3);
	CHECK_EQ(output.candidate_mask[0], 0);
	CHECK_EQ(output.candidate_mask[1], 1);
	CHECK_EQ(output.candidate_mask[2], 1);
	REQUIRE_EQ(output.compacted_candidate_indices.size(), 2);
	CHECK_EQ(output.compacted_candidate_indices[0], 2);
	CHECK_EQ(output.compacted_candidate_indices[1], 1);
	CHECK_EQ(output.blocker_hit_hints[0], 1);
	CHECK_EQ(output.blocker_hit_hints[1], 1);

	BakedVisibilityBackendBatchOutput repeat;
	CHECK_EQ(BakedVisibilityBackend::execute_cpu_reference(input, repeat), OK);
	CHECK_EQ(output.candidate_mask, repeat.candidate_mask);
	CHECK_EQ(output.compacted_candidate_indices, repeat.compacted_candidate_indices);
	CHECK_EQ(output.blocker_hit_hints, repeat.blocker_hit_hints);
}

TEST_CASE("[Rendering][BakedVisibility] unsupported adapters preserve CPU batch semantics") {
	BakedVisibilityBackendBatchInput input;
	input.query_bounds = AABB(Vector3(), Vector3(1, 1, 1));
	input.candidates.push_back({ AABB(Vector3(), Vector3(1, 1, 1)), 1 });
	input.blockers.push_back({ AABB(Vector3(0.25f, -1, -1), Vector3(0.1f, 2, 2)) });
	BakedVisibilityBackendBatchOutput cpu;
	BakedVisibilityBackendBatchOutput vulkan;
	CHECK_EQ(BakedVisibilityBackend::execute(BakedVisibilityBackendKind::CPU_REFERENCE, input, cpu), OK);
	CHECK_EQ(BakedVisibilityBackend::execute(BakedVisibilityBackendKind::VULKAN, input, vulkan), OK);
	CHECK_EQ(cpu.candidate_mask, vulkan.candidate_mask);
	CHECK_EQ(cpu.compacted_candidate_indices, vulkan.compacted_candidate_indices);
	CHECK_EQ(cpu.blocker_hit_hints, vulkan.blocker_hit_hints);
	CHECK_FALSE(vulkan.gpu_executed);
}

TEST_CASE("[Rendering][BakedVisibility] Vulkan RTX contract is explicit and feature-gated") {
	CHECK_FALSE(BakedVisibilityVulkanBatchContract::is_supported(nullptr));
	CHECK(BakedVisibilityVulkanBatchContract::raygen_source().contains("GL_EXT_ray_tracing"));
	CHECK(BakedVisibilityVulkanBatchContract::raygen_source().contains("traceRayEXT"));
	CHECK(BakedVisibilityVulkanBatchContract::raygen_source().contains("accelerationStructureEXT"));
	CHECK(BakedVisibilityVulkanBatchContract::miss_source().contains("rayPayloadInEXT"));
	CHECK(BakedVisibilityVulkanBatchContract::closest_hit_source().contains("hitAttributeEXT"));
	BakedVisibilityBackendBatchOutput output;
	BakedVisibilityBackendBatchInput input;
	CHECK_EQ(BakedVisibilityVulkanBatchContract::execute_batch(input, output), ERR_UNAVAILABLE);
}

TEST_CASE("[Rendering][BakedVisibility] convex certificate packet requires all 64 corner pairs") {
	BakedVisibilityBackendCertificateBatchInput input;
	BakedVisibilityBackendCertificatePatch patch;
	patch.vertices[0] = Vector3(-8, -8, 0);
	patch.vertices[1] = Vector3(8, -8, 0);
	patch.vertices[2] = Vector3(8, 8, 0);
	patch.vertices[3] = Vector3(-8, 8, 0);
	patch.vertex_count = 4;
	patch.normal = Vector3(0, 0, 1);
	patch.patch_id = 17;
	input.patches.push_back(patch);
	BakedVisibilityBackendCertificateQuery query;
	query.source_bounds = AABB(Vector3(-1, -1, 2), Vector3(2, 2, 1));
	query.target_bounds = AABB(Vector3(-1, -1, -3), Vector3(2, 2, 1));
	query.patch_index = 0;
	input.queries.push_back(query);
	BakedVisibilityBackendCertificateBatchOutput output;
	CHECK_EQ(BakedVisibilityBackend::execute_cpu_certificate_reference(input, output), OK);
	REQUIRE_EQ(output.results.size(), 1);
	CHECK_EQ(output.results[0], BakedVisibilityCertificateResult::PROVEN);
	CHECK_EQ(output.witness_patch_indices[0], 0);
	CHECK_EQ(output.packet_count, uint64_t(1));

	// A boundary source box is ambiguous even though many individual rays hit.
	input.queries.write[0].source_bounds.position.z = 0.0f;
	CHECK_EQ(BakedVisibilityBackend::execute_cpu_certificate_reference(input, output), OK);
	CHECK_EQ(output.results[0], BakedVisibilityCertificateResult::AMBIGUOUS);
}

TEST_CASE("[Rendering][BakedVisibility] unsupported certificate adapters fail open to the CPU oracle") {
	BakedVisibilityBackendCertificateBatchInput input;
	BakedVisibilityBackendCertificatePatch patch;
	patch.vertex_count = 2; // Unsupported, must never prove exclusion.
	input.patches.push_back(patch);
	BakedVisibilityBackendCertificateQuery query;
	query.source_bounds = AABB(Vector3(), Vector3(1, 1, 1));
	query.target_bounds = AABB(Vector3(0, 0, 2), Vector3(1, 1, 1));
	input.queries.push_back(query);
	BakedVisibilityBackendCertificateBatchOutput output;
	CHECK_EQ(BakedVisibilityBackend::execute_certificate_batch(BakedVisibilityBackendKind::VULKAN, input, output), OK);
	REQUIRE_EQ(output.results.size(), 1);
	CHECK_EQ(output.results[0], BakedVisibilityCertificateResult::AMBIGUOUS);
	CHECK_FALSE(output.gpu_executed);
}

#ifdef METAL_ENABLED
TEST_CASE("[Rendering][BakedVisibility][Metal] native batch matches CPU candidate semantics") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RenderingContextDriverMetal *context = nullptr;
	if (!rd) {
		context = memnew(RenderingContextDriverMetal);
		rd = memnew(RenderingDevice);
		Error initialize_error = context->initialize();
		if (initialize_error == OK) {
			initialize_error = rd->initialize(context);
		}
		if (initialize_error != OK) {
			memdelete(rd);
			memdelete(context);
			REQUIRE_EQ(initialize_error, OK);
			return;
		}
	}
	const bool owns_device = context != nullptr;
	if (rd->get_device_api_name() != "Metal") {
		if (owns_device) {
			memdelete(rd);
			memdelete(context);
		}
		FAIL_CHECK("Metal-enabled fixture did not create a Metal RenderingDevice");
		return;
	}
	const BakedVisibilityBackendCapabilities capabilities = BakedVisibilityBackend::probe(BakedVisibilityBackendKind::METAL);
	if (!capabilities.available) {
		if (owns_device) {
			memdelete(rd);
			memdelete(context);
		}
		FAIL_CHECK("Metal-enabled fixture requires a hardware-ray-capable Metal device");
		return;
	}
	BakedVisibilityBackendBatchInput input;
	input.query_bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	input.candidates.push_back({ AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f)), 40 });
	input.candidates.push_back({ AABB(Vector3(4, 0, 0), Vector3(1, 1, 1)), 10 });
	input.candidates.push_back({ AABB(Vector3(0.25f, 0.25f, 0.25f), Vector3(0.5f, 0.5f, 0.5f)), 20 });
	input.blockers.push_back({ AABB(Vector3(0.05f, -1, -1), Vector3(0.1f, 2, 2)) });
	BakedVisibilityBackendBatchOutput cpu;
	BakedVisibilityBackendBatchOutput metal;
	CHECK_EQ(BakedVisibilityBackend::execute_cpu_reference(input, cpu), OK);
	String error;
	CHECK_EQ(BakedVisibilityBackend::execute(BakedVisibilityBackendKind::METAL, input, metal, &error), OK);
	INFO(error);
	CHECK(metal.gpu_executed);
	CHECK(metal.hardware_ray_queries_executed);
	CHECK_EQ(metal.candidate_mask, cpu.candidate_mask);
	CHECK_EQ(metal.compacted_candidate_indices, cpu.compacted_candidate_indices);
	CHECK_EQ(metal.blocker_hit_hints, cpu.blocker_hit_hints);
	CHECK_EQ(metal.hardware_blocker_hit_hints.size(), input.candidates.size());
	CHECK_EQ(metal.dispatch_count, 3);
	CHECK_EQ(metal.ray_query_count, input.candidates.size());
	BakedVisibilityBackendCertificateBatchInput certificates;
	BakedVisibilityBackendCertificatePatch patch;
	patch.vertices[0] = Vector3(-8, -8, 0);
	patch.vertices[1] = Vector3(8, -8, 0);
	patch.vertices[2] = Vector3(8, 8, 0);
	patch.vertices[3] = Vector3(-8, 8, 0);
	patch.normal = Vector3(0, 0, 1);
	patch.vertex_count = 4;
	certificates.patches.push_back(patch);
	BakedVisibilityBackendCertificateQuery certificate_query;
	certificate_query.source_bounds = AABB(Vector3(-1, -1, 2), Vector3(2, 2, 1));
	certificate_query.target_bounds = AABB(Vector3(-1, -1, -3), Vector3(2, 2, 1));
	certificates.queries.push_back(certificate_query);
	certificate_query.target_id = 1;
	certificates.queries.push_back(certificate_query); // One dispatch, two packets.
	BakedVisibilityBackendCertificateBatchOutput certificate_output;
	CHECK_EQ(BakedVisibilityBackend::execute_certificate_batch(BakedVisibilityBackendKind::METAL, certificates, certificate_output), OK);
	CHECK(certificate_output.gpu_executed);
	CHECK_EQ(certificate_output.dispatch_count, 1);
	CHECK_EQ(certificate_output.packet_count, uint64_t(2));
	CHECK_EQ(certificate_output.results[0], BakedVisibilityCertificateResult::PROVEN);
	CHECK_EQ(certificate_output.results[1], BakedVisibilityCertificateResult::PROVEN);
	if (owns_device) {
		memdelete(rd);
		memdelete(context);
	}
}
#endif
