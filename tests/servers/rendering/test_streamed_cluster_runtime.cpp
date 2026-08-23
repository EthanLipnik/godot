/**************************************************************************/
/*  test_streamed_cluster_runtime.cpp                                     */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_streamed_cluster_runtime)

#include "servers/rendering/path_tracing/hybrid_runtime.h"
#include "servers/rendering/path_tracing/streamed_cluster_runtime.h"

#ifdef METAL_ENABLED
#include "servers/rendering/renderer_rd/effects/metal_hybrid_effect.h"
#endif

namespace TestStreamedClusterRuntime {

using namespace RendererPathTracing;

static Vector<StreamedClusterPageDescriptor> _make_manifest() {
	Vector<StreamedClusterPageDescriptor> pages;
	StreamedClusterPageDescriptor coarse;
	coarse.stable_id = 10;
	coarse.revision = 100;
	coarse.child_ids.push_back(20);
	coarse.child_ids.push_back(30);
	coarse.bounds = AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	coarse.geometric_error = 1.0f;
	coarse.blob_bytes = 4096;
	coarse.vertex_bytes = 1200;
	coarse.index_bytes = 600;
	coarse.vertex_count = 100;
	coarse.index_count = 150;
	coarse.lod_level = 1;
	coarse.persistent = true;
	pages.push_back(coarse);

	for (uint64_t id : { uint64_t(20), uint64_t(30) }) {
		StreamedClusterPageDescriptor fine;
		fine.stable_id = id;
		fine.revision = id * 10;
		fine.parent_id = 10;
		fine.bounds = coarse.bounds;
		fine.blob_bytes = 3072;
		fine.vertex_bytes = 900;
		fine.index_bytes = 480;
		fine.vertex_count = 75;
		fine.index_count = 120;
		pages.push_back(fine);
	}
	return pages;
}

static void _complete_tasks(StreamedClusterResidencyManager &r_manager, const Vector<StreamedClusterLoadTask> &p_tasks, uint64_t p_first_completion_token) {
	for (int i = 0; i < p_tasks.size(); i++) {
		const StreamedClusterLoadTask &task = p_tasks[i];
		CHECK(r_manager.complete_load(task.task_token, p_first_completion_token + i, RID::from_uint64(task.stable_id * 2), RID::from_uint64(task.stable_id * 2 + 1), true) == OK);
	}
}

TEST_CASE("[Rendering][StreamedCluster] Upload completion is explicit and refinement activation is atomic") {
	StreamedClusterResidencyManager manager;
	CHECK(manager.set_manifest(_make_manifest()) == OK);

	Vector<StreamedClusterLoadTask> tasks = manager.take_load_tasks();
	REQUIRE(tasks.size() == 1);
	CHECK(tasks[0].stable_id == 10);
	_complete_tasks(manager, tasks, 5);
	CHECK(manager.get_page_state(10) == StreamedClusterPageState::RESIDENT);
	CHECK(manager.get_active_surfaces().is_empty());
	manager.update_completion_tokens(4, 0);
	CHECK(manager.get_active_surfaces().is_empty());
	manager.update_completion_tokens(5, 0);
	REQUIRE(manager.get_active_surfaces().size() == 1);
	CHECK(manager.get_active_surfaces()[0].stable_id == 10);

	StreamedClusterSelection selection;
	selection.frame = 1;
	selection.maximum_geometric_error = 0.1f;
	selection.eye_regions.push_back(AABB(Vector3(100, 100, 100), Vector3(2, 2, 2)));
	selection.eye_regions.push_back(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)));
	manager.update_selection(selection);
	tasks = manager.take_load_tasks();
	REQUIRE(tasks.size() == 2);
	CHECK(tasks[0].stable_id == 20);
	CHECK(tasks[1].stable_id == 30);

	CHECK(manager.complete_load(tasks[0].task_token, 10, RID::from_uint64(40), RID::from_uint64(41), true) == OK);
	manager.update_completion_tokens(10, 0);
	REQUIRE(manager.get_active_surfaces().size() == 1);
	CHECK(manager.get_active_surfaces()[0].stable_id == 10);
	CHECK(manager.complete_load(tasks[1].task_token, 11, RID::from_uint64(60), RID::from_uint64(61), true) == OK);
	manager.update_completion_tokens(11, 0);
	const Vector<StreamedClusterSurface> active = manager.get_active_surfaces();
	REQUIRE(active.size() == 2);
	CHECK(active[0].stable_id == 20);
	CHECK(active[1].stable_id == 30);
	CHECK(manager.get_frame_stats().coarse_fallbacks >= 1);
}

TEST_CASE("[Rendering][StreamedCluster] Stereo selection is a conservative union") {
	StreamedClusterResidencyManager manager;
	CHECK(manager.set_manifest(_make_manifest()) == OK);
	StreamedClusterSelection selection;
	selection.frame = 7;
	selection.maximum_geometric_error = 0.2f;
	selection.eye_regions.push_back(AABB(Vector3(50, 50, 50), Vector3(1, 1, 1)));
	selection.eye_regions.push_back(AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1, 1, 1)));
	manager.update_selection(selection);
	const Vector<StreamedClusterLoadTask> tasks = manager.take_load_tasks();
	REQUIRE(tasks.size() == 3);
	CHECK(tasks[0].stable_id == 10);
	CHECK(tasks[1].stable_id == 20);
	CHECK(tasks[2].stable_id == 30);
}

TEST_CASE("[Rendering][StreamedCluster] Retirement waits for an explicit GPU token") {
	StreamedClusterResidencyManager manager;
	StreamedClusterBudgets budgets;
	budgets.minimum_resident_frames = 0;
	manager.set_budgets(budgets);
	CHECK(manager.set_manifest(_make_manifest()) == OK);

	StreamedClusterSelection near_selection;
	near_selection.frame = 1;
	near_selection.maximum_geometric_error = 0.1f;
	near_selection.eye_regions.push_back(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)));
	manager.update_selection(near_selection);
	const Vector<StreamedClusterLoadTask> loads = manager.take_load_tasks();
	_complete_tasks(manager, loads, 1);
	manager.update_completion_tokens(10, 0);
	CHECK(manager.get_active_surfaces().size() == 2);

	StreamedClusterSelection far_selection;
	far_selection.frame = 2;
	far_selection.maximum_geometric_error = 0.1f;
	far_selection.eye_regions.push_back(AABB(Vector3(100, 100, 100), Vector3(1, 1, 1)));
	manager.update_selection(far_selection);
	CHECK(manager.get_page_state(20) == StreamedClusterPageState::RETIRING);
	Vector<StreamedClusterRetireTask> retires = manager.take_retire_tasks();
	REQUIRE(retires.size() == 2);
	CHECK(manager.submit_retirement(retires[0].stable_id, 20) == OK);
	CHECK(manager.submit_retirement(retires[1].stable_id, 21) == OK);
	manager.update_completion_tokens(10, 19);
	CHECK(manager.get_page_state(20) == StreamedClusterPageState::RETIRING);
	manager.update_completion_tokens(10, 21);
	CHECK(manager.get_page_state(20) == StreamedClusterPageState::UNLOADED);
	CHECK(manager.get_page_state(30) == StreamedClusterPageState::UNLOADED);
	REQUIRE(manager.get_active_surfaces().size() == 1);
	CHECK(manager.get_active_surfaces()[0].stable_id == 10);
}

TEST_CASE("[Rendering][StreamedCluster] Successful transfers reject zero completion tokens without changing state") {
	StreamedClusterResidencyManager manager;
	StreamedClusterBudgets budgets;
	budgets.minimum_resident_frames = 0;
	manager.set_budgets(budgets);
	CHECK(manager.set_manifest(_make_manifest()) == OK);

	const Vector<StreamedClusterLoadTask> initial_loads = manager.take_load_tasks();
	REQUIRE(initial_loads.size() == 1);
	ERR_PRINT_OFF;
	CHECK(manager.complete_load(initial_loads[0].task_token, 0, RID::from_uint64(20), RID::from_uint64(21), true) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
	CHECK(manager.get_page_state(10) == StreamedClusterPageState::LOADING);
	CHECK(manager.get_frame_stats().reserved_bytes == 1800);
	CHECK(manager.complete_load(initial_loads[0].task_token, 1, RID::from_uint64(20), RID::from_uint64(21), true) == OK);
	manager.update_completion_tokens(1, 0);
	CHECK(manager.get_page_state(10) == StreamedClusterPageState::ACTIVE);

	StreamedClusterSelection near_selection;
	near_selection.frame = 1;
	near_selection.maximum_geometric_error = 0.1f;
	near_selection.eye_regions.push_back(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)));
	manager.update_selection(near_selection);
	const Vector<StreamedClusterLoadTask> refinement_loads = manager.take_load_tasks();
	REQUIRE(refinement_loads.size() == 2);
	_complete_tasks(manager, refinement_loads, 2);
	manager.update_completion_tokens(3, 0);
	CHECK(manager.get_page_state(20) == StreamedClusterPageState::ACTIVE);

	StreamedClusterSelection far_selection;
	far_selection.frame = 2;
	far_selection.maximum_geometric_error = 0.1f;
	far_selection.eye_regions.push_back(AABB(Vector3(100, 100, 100), Vector3(1, 1, 1)));
	manager.update_selection(far_selection);
	const Vector<StreamedClusterRetireTask> retires = manager.take_retire_tasks();
	REQUIRE(retires.size() == 2);
	ERR_PRINT_OFF;
	CHECK(manager.submit_retirement(retires[0].stable_id, 0) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
	CHECK(manager.get_page_state(retires[0].stable_id) == StreamedClusterPageState::RETIRING);
	manager.update_completion_tokens(3, UINT64_MAX);
	CHECK(manager.get_page_state(retires[0].stable_id) == StreamedClusterPageState::RETIRING);
	CHECK(manager.submit_retirement(retires[0].stable_id, 4) == OK);
	CHECK(manager.submit_retirement(retires[1].stable_id, 5) == OK);
	manager.update_completion_tokens(3, 5);
	CHECK(manager.get_page_state(20) == StreamedClusterPageState::UNLOADED);
	CHECK(manager.get_page_state(30) == StreamedClusterPageState::UNLOADED);
}

TEST_CASE("[Rendering][StreamedCluster] Load budgets are deterministic and never overcommit") {
	StreamedClusterResidencyManager manager;
	StreamedClusterBudgets budgets;
	budgets.maximum_load_tasks_per_frame = 1;
	budgets.maximum_load_bytes_per_frame = 2048;
	budgets.maximum_resident_bytes = 4096;
	manager.set_budgets(budgets);
	CHECK(manager.set_manifest(_make_manifest()) == OK);
	StreamedClusterSelection selection;
	selection.frame = 1;
	selection.maximum_geometric_error = 0.0f;
	selection.eye_regions.push_back(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)));
	manager.update_selection(selection);
	const Vector<StreamedClusterLoadTask> tasks = manager.take_load_tasks();
	REQUIRE(tasks.size() == 1);
	CHECK(tasks[0].stable_id == 10);
	CHECK(manager.get_frame_stats().reserved_bytes == 1800);
	CHECK(manager.get_frame_stats().budget_misses >= 2);
}

TEST_CASE("[Rendering][StreamedCluster] Manifests reject unreachable roots and cycles") {
	Vector<StreamedClusterPageDescriptor> unreachable = _make_manifest();
	StreamedClusterPageDescriptor orphan = unreachable[1];
	orphan.stable_id = 40;
	orphan.revision = 400;
	orphan.parent_id = 0;
	orphan.child_ids.clear();
	orphan.persistent = false;
	unreachable.push_back(orphan);
	StreamedClusterResidencyManager manager;
	ERR_PRINT_OFF;
	CHECK(manager.set_manifest(unreachable) == ERR_INVALID_DATA);
	ERR_PRINT_ON;

	Vector<StreamedClusterPageDescriptor> cyclic = _make_manifest();
	cyclic.write[0].child_ids.clear();
	StreamedClusterPageDescriptor first = cyclic[1];
	first.stable_id = 40;
	first.revision = 400;
	first.parent_id = 50;
	first.child_ids.clear();
	first.child_ids.push_back(50);
	StreamedClusterPageDescriptor second = cyclic[2];
	second.stable_id = 50;
	second.revision = 500;
	second.parent_id = 40;
	second.child_ids.clear();
	second.child_ids.push_back(40);
	cyclic.remove_at(2);
	cyclic.remove_at(1);
	cyclic.push_back(first);
	cyclic.push_back(second);
	ERR_PRINT_OFF;
	CHECK(manager.set_manifest(cyclic) == ERR_INVALID_DATA);
	ERR_PRINT_ON;
}

TEST_CASE("[Rendering][StreamedCluster] Resident surfaces use stable revisions for BLAS reuse") {
	StreamedClusterResidencyManager manager;
	CHECK(manager.set_manifest(_make_manifest()) == OK);
	const Vector<StreamedClusterLoadTask> loads = manager.take_load_tasks();
	_complete_tasks(manager, loads, 1);
	manager.update_completion_tokens(1, 0);
	const Vector<StreamedClusterSurface> active = manager.get_active_surfaces();
	REQUIRE(active.size() == 1);

	HybridSceneTracker tracker(1);
	HybridSceneRevision revision;
	revision.topology = active[0].topology_revision;
	tracker.begin_frame(1);
	CHECK((tracker.touch(active[0].stable_id, revision, false) & HYBRID_SCENE_UPDATE_BUILD_BLAS) != 0);
	tracker.end_frame();
	tracker.begin_frame(2);
	CHECK(tracker.touch(active[0].stable_id, revision, false) == HYBRID_SCENE_UPDATE_NONE);
	tracker.end_frame();
	CHECK(tracker.get_frame_stats().blas_reused == 1);
}

#ifdef METAL_ENABLED
TEST_CASE("[Rendering][StreamedCluster] Metal adapter preserves streamed page and material contracts") {
	StreamedClusterSurface resident;
	resident.stable_id = 0x1234;
	resident.topology_revision = 0x5678;
	resident.vertex_buffer = RID::from_uint64(0x300);
	resident.index_buffer = RID::from_uint64(0x301);
	resident.vertex_count = 72;
	resident.index_count = 120;
	resident.vertex_stride = 12;
	resident.index_stride = 4;
	resident.material_index = 0;
	resident.bounds = AABB(Vector3(-3.0f, -2.0f, -1.0f), Vector3(6.0f, 4.0f, 2.0f));
	Vector<StreamedClusterSurface> residents;
	residents.push_back(resident);

	RendererRD::MetalHybridEffect::Instance material;
	material.albedo = Color(0.2f, 0.4f, 0.6f, 1.0f);
	material.emission = Color(80.0f, 24.0f, 5.0f, 1.0f);
	material.metallic = 0.7f;
	material.roughness = 0.18f;
	Vector<RendererRD::MetalHybridEffect::Instance> materials;
	materials.push_back(material);
	Transform3D world_transform(Basis(), Vector3(4.0f, 5.0f, 6.0f));

	RendererRD::MetalHybridEffect::FrameRequest request;
	CHECK(RendererRD::MetalHybridEffect::append_streamed_cluster_surfaces(request, residents, world_transform, materials) == OK);
	REQUIRE(request.surfaces.size() == 1);
	REQUIRE(request.instances.size() == 1);
	const RendererRD::MetalHybridEffect::Surface &surface = request.surfaces[0];
	CHECK(surface.stable_id == resident.stable_id);
	CHECK(surface.topology_revision == resident.topology_revision);
	CHECK(surface.vertex_buffer == resident.vertex_buffer);
	CHECK(surface.index_buffer == resident.index_buffer);
	CHECK(surface.vertex_count == resident.vertex_count);
	CHECK(surface.index_count == resident.index_count);
	CHECK(surface.vertex_stride == 12);
	CHECK(surface.index_stride == 4);
	CHECK(surface.compressed_aabb == resident.bounds);
	const RendererRD::MetalHybridEffect::Instance &instance = request.instances[0];
	CHECK(instance.surface_id == resident.stable_id);
	CHECK(instance.transform == world_transform);
	CHECK(instance.albedo == material.albedo);
	CHECK(instance.emission == material.emission);
	CHECK(instance.metallic == material.metallic);
	CHECK(instance.roughness == material.roughness);

	residents.write[0].material_index = 1;
	RendererRD::MetalHybridEffect::FrameRequest invalid_request;
	ERR_PRINT_OFF;
	CHECK(RendererRD::MetalHybridEffect::append_streamed_cluster_surfaces(invalid_request, residents, world_transform, materials) == ERR_UNAVAILABLE);
	ERR_PRINT_ON;
	CHECK(invalid_request.surfaces.is_empty());
	CHECK(invalid_request.instances.is_empty());
}
#endif

} // namespace TestStreamedClusterRuntime
