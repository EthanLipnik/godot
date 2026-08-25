/**************************************************************************/
/*  test_virtual_geometry_runtime.h                                       */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

#include "modules/meshoptimizer/virtual_geometry.h"
#include "modules/meshoptimizer/virtual_geometry_compiler.h"
#include "modules/meshoptimizer/virtual_geometry_instance_3d.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_storage.h"

namespace TestVirtualGeometryRuntime {

using namespace RendererVirtualGeometry;

static VirtualGeometryCompiler::Input _runtime_input() {
	VirtualGeometryCompiler::Input input;
	input.source_asset_identity = 10;
	input.source_primitive_identity = 20;
	for (int z = 0; z < 5; z++) for (int x = 0; x < 5; x++) input.positions.push_back(Vector3(x, 0, z));
	for (int z = 0; z < 4; z++) for (int x = 0; x < 4; x++) {
		int a = z * 5 + x, b = a + 1, c = a + 5, d = c + 1;
		input.indices.push_back(a); input.indices.push_back(c); input.indices.push_back(b);
		input.indices.push_back(b); input.indices.push_back(c); input.indices.push_back(d);
	}
	return input;
}

static Package _runtime_package() {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_triangles_per_cluster = 2;
	settings.max_vertices_per_cluster = 8;
	settings.clusters_per_group = 2;
	settings.max_decoded_page_bytes = 512;
	Package package;
	CHECK(compiler.compile(_runtime_input(), settings, package) == OK);
	return package;
}

TEST_CASE("[VirtualGeometry] VG2 heap allocations align, never overlap, and report fragmentation") {
	VirtualGeometryHeap heap(128);
	VirtualGeometryHeap::Allocation a, b, c;
	CHECK(heap.allocate(17, 16, a));
	CHECK(heap.allocate(17, 32, b));
	CHECK(a.offset % 16 == 0);
	CHECK(b.offset % 32 == 0);
	CHECK((a.offset + a.size <= b.offset || b.offset + b.size <= a.offset));
	CHECK(heap.free(a));
	CHECK(heap.allocate(8, 8, c));
	CHECK(heap.free(b));
	const VirtualGeometryHeap::Diagnostics d = heap.get_diagnostics();
	CHECK(d.free > 0);
	CHECK(d.largest_free_block <= d.free);
}

TEST_CASE("[VirtualGeometry] VG2 corrupt and cancelled worker completions fail closed") {
	Package package = _runtime_package();
	VirtualGeometryStorage storage;
	REQUIRE(storage.set_package(package, 1) == OK);
	Vector<uint64_t> requests = storage.take_io_requests(8);
	REQUIRE(!requests.is_empty());
	const uint64_t page = requests[0];
	const uint64_t old_generation = storage.get_page_diagnostics(page).generation;
	storage.enqueue_worker_completion(page, old_generation + 1, PackedByteArray(), ERR_FILE_CORRUPT, "corrupt");
	storage.render_process(0, 1);
	CHECK(storage.get_page_diagnostics(page).failure_count == 0); // stale generation discarded
	PackedByteArray decoded;
	String error;
	CHECK(storage.decode_page_on_worker(page, old_generation, decoded, error) == OK);
	storage.enqueue_worker_completion(page, old_generation, decoded, OK);
	storage.render_process(0, 2);
	CHECK(storage.get_page_state(page) == VirtualGeometryPageState::UPLOAD_PENDING);
}

TEST_CASE("[VirtualGeometry] VG2 keeps coarse coverage until every fine page completes and retires after real completion") {
	Package package = _runtime_package();
	VirtualGeometryStorage storage;
	REQUIRE(storage.set_package(package, 2) == OK);
	Vector<uint64_t> requests = storage.take_io_requests(64);
	for (uint64_t page : requests) {
		const uint64_t generation = storage.get_page_diagnostics(page).generation;
		PackedByteArray decoded; String error;
		REQUIRE(storage.decode_page_on_worker(page, generation, decoded, error) == OK);
		storage.enqueue_worker_completion(page, generation, decoded, OK);
	}
	storage.render_process(0, 10);
	storage.notify_submission_completed(10);
	for (const PageDescriptor &page : package.manifest.pages) {
		if (!page.persistent) CHECK(storage.request_page(page.stable_id, VirtualGeometryRequestReason::RASTER_VIEW, 1) == OK);
	}
	requests = storage.take_io_requests(64);
	for (uint64_t page : requests) {
		const uint64_t generation = storage.get_page_diagnostics(page).generation;
		PackedByteArray decoded; String error;
		REQUIRE(storage.decode_page_on_worker(page, generation, decoded, error) == OK);
		storage.enqueue_worker_completion(page, generation, decoded, OK);
	}
	storage.render_process(10, 11);
	storage.notify_submission_completed(11);
	const VirtualGeometryRuntimeDiagnostics ready = storage.get_diagnostics();
	CHECK(ready.active_pages > 0);
	for (const PageDescriptor &page : package.manifest.pages) {
		if (storage.get_page_state(page.stable_id) == VirtualGeometryPageState::ACTIVE && !page.persistent) {
			storage.mark_page_used(page.stable_id, 12, 13, 14);
			CHECK(storage.retire_page(page.stable_id) == OK);
			storage.process_retirements(13);
			CHECK(storage.get_page_state(page.stable_id) == VirtualGeometryPageState::RETIRING);
			storage.process_retirements(14);
			CHECK(storage.get_page_state(page.stable_id) == VirtualGeometryPageState::UNLOADED);
			break;
		}
	}
}

TEST_CASE("[VirtualGeometry] VG2 scene boundary has no page children and resource revisions propagate") {
	Package package = _runtime_package();
	Ref<VirtualGeometry> resource;
	resource.instantiate();
	REQUIRE(resource->set_compiled_package(package) == OK);
	VirtualGeometryInstance3D instance;
	instance.set_virtual_geometry(resource);
	instance.set_semantic_instance_id(0x12345678);
	const uint64_t before = instance.get_instance_revision();
	CHECK(instance.get_base() == resource->get_rid());
	CHECK(RenderingServer::get_singleton()->is_virtual_geometry(instance.get_base()));
	instance.set_virtual_geometry_visibility_layer(0x4);
	CHECK(instance.get_layer_mask() == 0x4);
	CHECK(instance.get_child_count() == 0);
	CHECK_EQ(instance.get_semantic_instance_id(), int64_t(0x12345678));
	CHECK(resource->set_compiled_package(package) == OK);
	CHECK(instance.get_child_count() == 0);
	CHECK_EQ(instance.get_semantic_instance_id(), int64_t(0x12345678));
	CHECK(instance.get_instance_revision() > before);
}

TEST_CASE("[VirtualGeometry] F6 fixed descriptor slots publish one generation and clear retired pages") {
	Package package = _runtime_package();
	VirtualGeometryStorage storage;
	VirtualGeometryBudgets budgets = storage.get_budgets();
	budgets.io_tasks_per_frame = 1024;
	storage.set_budgets(budgets);
	REQUIRE_EQ(storage.set_package(package, 7), OK);
	auto pump_requests = [&storage](uint64_t p_completed, uint64_t p_pending) {
		const Vector<uint64_t> requests = storage.take_io_requests(64);
		for (uint64_t page_id : requests) {
			const uint64_t generation = storage.get_page_diagnostics(page_id).generation;
			PackedByteArray decoded;
			String error;
			REQUIRE_EQ(storage.decode_page_on_worker(page_id, generation, decoded, error), OK);
			storage.enqueue_worker_completion(page_id, generation, decoded, OK);
		}
		storage.render_process(p_completed, p_pending);
		storage.notify_submission_completed(p_pending);
	};
	pump_requests(0, 10);
	const uint64_t root_generation = storage.get_active_descriptor_generation();
	REQUIRE(root_generation != 0);
	for (const PageDescriptor &page : package.manifest.pages) {
		if (!page.persistent) REQUIRE_EQ(storage.request_page(page.stable_id, VirtualGeometryRequestReason::RASTER_VIEW, 100), OK);
	}
	pump_requests(10, 20);
	const uint64_t full_generation = storage.get_active_descriptor_generation();
	CHECK_NE(full_generation, root_generation);
	const Vector<VirtualGeometryGPUClusterDescriptor> &descriptors = storage.get_gpu_cluster_descriptors();
	REQUIRE_EQ(descriptors.size(), package.manifest.clusters.size());
	for (uint32_t slot = 0; slot < uint32_t(descriptors.size()); slot++) {
		CHECK_EQ(*storage.get_gpu_cluster_descriptor_slot(package.manifest.clusters[slot].stable_id), slot);
		CHECK_EQ(descriptors[slot].stable_id, package.manifest.clusters[slot].stable_id);
		CHECK_EQ(descriptors[slot].generation, uint32_t(full_generation));
		CHECK_EQ(descriptors[slot].position_offset / 12, descriptors[slot].attribute_offset / sizeof(VirtualGeometryGPUVertexAttributes));
		CHECK_EQ(descriptors[slot].base_vertex, descriptors[slot].position_offset / 12);
	}
	uint64_t retired_page = 0;
	for (const PageDescriptor &page : package.manifest.pages) {
		if (!page.persistent && storage.get_page_state(page.stable_id) == VirtualGeometryPageState::ACTIVE && storage.retire_page(page.stable_id) == OK) {
			retired_page = page.stable_id;
			break;
		}
	}
	REQUIRE(retired_page != 0);
	storage.process_retirements(20);
	storage.render_process(20, 21);
	storage.notify_submission_completed(21);
	for (const ClusterDescriptor &cluster : package.manifest.clusters) {
		if (cluster.page_id != retired_page) continue;
		const uint32_t slot = *storage.get_gpu_cluster_descriptor_slot(cluster.stable_id);
		CHECK_EQ(storage.get_gpu_cluster_descriptors()[slot].index_count, 0);
		CHECK(storage.get_gpu_cluster_descriptor(cluster.stable_id) == nullptr);
	}
	CHECK(storage.get_diagnostics().descriptor_publications >= 3);
}

TEST_CASE("[VirtualGeometry] VG2 rendering-server resources validate, update, query, and free") {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	REQUIRE(rendering_server != nullptr);
	const Package package = _runtime_package();
	const RID resource = rendering_server->virtual_geometry_create();
	REQUIRE(resource.is_valid());
	CHECK(rendering_server->is_virtual_geometry(resource));
	CHECK(rendering_server->virtual_geometry_set_package(resource, package, 1) == OK);
	CHECK(rendering_server->virtual_geometry_get_aabb(resource) == package.manifest.resource_bounds);
	CHECK(rendering_server->virtual_geometry_get_revision(resource) == 1);
	Vector<RID> materials;
	materials.push_back(RID());
	rendering_server->virtual_geometry_set_material_bindings(resource, materials, 2);
	CHECK(rendering_server->virtual_geometry_get_revision(resource) == 2);
	rendering_server->free_rid(resource);
	CHECK_FALSE(rendering_server->is_virtual_geometry(resource));
}

} // namespace TestVirtualGeometryRuntime
