/**************************************************************************/
/*  test_virtual_geometry_stereo_long_run.cpp                              */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_geometry_stereo_long_run)

#include "modules/meshoptimizer/virtual_geometry_compiler.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_raster.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_ray.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_spatial.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_storage.h"

namespace TestVirtualGeometryStereoLongRun {

using namespace RendererVirtualGeometry;

static Package _package() {
	VirtualGeometryCompiler::Input input;
	input.source_asset_identity = 0x3001;
	input.source_primitive_identity = 0x4001;
	for (int z = 0; z < 9; z++) {
		for (int x = 0; x < 9; x++) {
			input.positions.push_back(Vector3(real_t(x), 0.15 * Math::sin(real_t(x * 2 + z)), real_t(z)));
		}
	}
	for (int z = 0; z < 8; z++) {
		for (int x = 0; x < 8; x++) {
			const int a = z * 9 + x;
			const int b = a + 1;
			const int c = a + 9;
			const int d = c + 1;
			input.indices.push_back(a);
			input.indices.push_back(c);
			input.indices.push_back(b);
			input.indices.push_back(b);
			input.indices.push_back(c);
			input.indices.push_back(d);
		}
	}
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_vertices_per_cluster = 16;
	settings.max_triangles_per_cluster = 4;
	settings.clusters_per_group = 2;
	settings.max_decoded_page_bytes = 512;
	Package package;
	CHECK_EQ(compiler.compile(input, settings, package), OK);
	return package;
}

static void _pump(VirtualGeometryStorage &r_storage, uint64_t p_serial) {
	for (;;) {
		const Vector<uint64_t> requests = r_storage.take_io_requests(64);
		if (requests.is_empty()) {
			break;
		}
		for (uint64_t page_id : requests) {
			const VirtualGeometryPageDiagnostics diagnostics = r_storage.get_page_diagnostics(page_id);
			PackedByteArray decoded;
			String error;
			REQUIRE_EQ(r_storage.decode_page_on_worker(page_id, diagnostics.generation, decoded, error), OK);
			r_storage.enqueue_worker_completion(page_id, diagnostics.generation, decoded, OK);
		}
		r_storage.render_process(p_serial, p_serial);
		r_storage.notify_submission_completed(p_serial);
	}
}

TEST_CASE("[Rendering][VirtualGeometry] VG9 stereo traversal remains bounded with independent eye history") {
	const Package package = _package();
	REQUIRE_EQ(validate_package(package, true), OK);
	uint32_t persistent_pages = 0;
	for (const PageDescriptor &page : package.manifest.pages) {
		persistent_pages += page.persistent;
	}
	REQUIRE(persistent_pages > 0);

	VirtualGeometryBudgets budgets;
	budgets.compressed_cpu_bytes = 4 * 1024 * 1024;
	budgets.decoded_cpu_bytes = 4 * 1024 * 1024;
	budgets.position_heap_bytes = 128 * 1024;
	budgets.index_heap_bytes = 128 * 1024;
	budgets.attribute_heap_bytes = 256 * 1024;
	budgets.upload_bytes_per_frame = 64 * 1024;
	budgets.io_tasks_per_frame = 8;
	VirtualGeometryStorage storage;
	storage.set_budgets(budgets);
	REQUIRE_EQ(storage.set_package(package, 1), OK);
	_pump(storage, 1);
	const VirtualGeometryRuntimeDiagnostics initial = storage.get_diagnostics();
	REQUIRE(initial.active_pages >= persistent_pages);
	const uint64_t active_page_limit = package.manifest.pages.size();

	VirtualGeometryRayHierarchy rays;
	REQUIRE_EQ(rays.set_manifest(package.manifest), OK);
	REQUIRE(!package.manifest.ray_groups.is_empty());
	const RayGroupDescriptor &ray_group = package.manifest.ray_groups[0];
	REQUIRE_EQ(rays.queue_build(ray_group.stable_id, 1), OK);
	REQUIRE_EQ(rays.begin_build(ray_group.stable_id, 1, 8192, false), OK);
	REQUIRE_EQ(rays.complete_build(ray_group.stable_id, 1, 0), OK);
	rays.activate_frame_boundary(1, 2);
	CHECK_EQ(rays.get_active_version(ray_group.stable_id), uint64_t(1));
	VirtualGeometrySpatialRuntime spatial;

	VirtualGeometryRasterSelectionState left_history;
	VirtualGeometryRasterSelectionState right_history;
	const uint64_t history_ownership_sentinel = UINT64_C(0xfffffffffffffff1);
	left_history.refined_group_ids.insert(history_ownership_sentinel);
	CHECK_FALSE(right_history.refined_group_ids.has(history_ownership_sentinel));
	left_history.refined_group_ids.erase(history_ownership_sentinel);
	bool saw_independent_eye_visibility = false;
	bool saw_history_transition = false;
	for (uint32_t frame = 0; frame < 240; frame++) {
		const uint64_t serial = uint64_t(frame) + 3;
		SpatialTraversalInput traversal;
		traversal.package_identity = package.manifest.source_asset_identity;
		traversal.position = Vector3(real_t(frame % 31) * 0.4, 1.0, real_t(frame % 23) * 0.3);
		traversal.velocity = Vector3(18.0, (frame % 9 == 0) ? 25.0 : 0.0, -24.0);
		traversal.view_direction = Vector3((frame % 2) ? 1.0 : -1.0, 0.0, -1.0).normalized();
		traversal.angular_velocity = Vector3(0, (frame % 37 == 0) ? Math::PI : 0.0, 0);
		traversal.horizon_seconds = 0.5;
		traversal.cell_size = 16.0;
		traversal.maximum_cells = 64;
		const SpatialTraversalPrediction prediction = spatial.predict_traversal(traversal);
		CHECK_LE(prediction.cells.size(), int(traversal.maximum_cells));
		if ((frame % 47) == 0) {
			spatial.rebase(traversal.position);
		}
		const HashSet<uint64_t> &active_clusters = storage.get_active_cluster_ids();
		VirtualGeometryRasterSelectionInput selection_input;
		selection_input.command_capacity = 128;
		selection_input.refine_threshold_pixels = (frame % 17 < 5) ? 0.25 : 2.0;
		selection_input.coarsen_threshold_pixels = 0.2;
		VirtualGeometryRasterView left;
		left.camera_position = Vector3(real_t(frame % 31) * 0.4, 1.0, real_t(frame % 23) * 0.3);
		left.history_valid = frame > 0;
		left.occlusion_valid = true;
		left.projected_error_threshold_pixels = selection_input.refine_threshold_pixels;
		VirtualGeometryRasterView right = left;
		right.camera_position.x += 0.064;
		right.history_valid = frame > 2;
		right.stereo_boundary_uncertain = (frame % 29) == 0;
		if ((frame % 7) == 0 && !active_clusters.is_empty()) {
			for (uint64_t split_cluster : active_clusters) {
				left.occluded_cluster_ids.insert(split_cluster);
				break;
			}
		}
		if ((frame % 11) == 0) {
			left.camera_cut = true;
		}
		selection_input.views.push_back(left);
		selection_input.views.push_back(right);

		VirtualGeometryRasterSelectionInput left_input = selection_input;
		left_input.views.clear();
		left_input.views.push_back(left);
		VirtualGeometryRasterSelectionInput right_input = selection_input;
		right_input.views.clear();
		right_input.views.push_back(right);
		// Each eye owns a separate selector state. The combined stereo selection
		// below is only used to validate the shared-residency/per-eye mask union.
		left_input.refine_threshold_pixels = (frame % 17 < 5) ? 0.25 : 2.0;
		right_input.refine_threshold_pixels = (frame % 17 < 5) ? 2.0 : 0.25;
		const VirtualGeometryRasterSelection left_selection = VirtualGeometryRasterSelector::select(package, active_clusters, left_input, &left_history);
		const VirtualGeometryRasterSelection right_selection = VirtualGeometryRasterSelector::select(package, active_clusters, right_input, &right_history);
		const VirtualGeometryRasterSelection stereo_selection = VirtualGeometryRasterSelector::select(package, active_clusters, selection_input);
		CHECK_EQ(left_selection.commands.size(), 128);
		CHECK_EQ(right_selection.commands.size(), 128);
		CHECK_EQ(stereo_selection.commands.size(), 128);
		CHECK(left_selection.cluster_ids.size() <= 128);
		CHECK(right_selection.cluster_ids.size() <= 128);
		CHECK_LE(storage.get_diagnostics().active_pages, active_page_limit);
		if (stereo_selection.diagnostics.per_eye_disagreement > 0 || left_selection.cluster_ids != right_selection.cluster_ids) {
			saw_independent_eye_visibility = true;
		}
		if (stereo_selection.diagnostics.fail_open_occlusion || left_selection.diagnostics.fail_open_occlusion || right_selection.diagnostics.fail_open_occlusion) {
			saw_history_transition = true;
		}

		if ((frame % 19) == 0) {
			for (const PageDescriptor &page : package.manifest.pages) {
				if (page.persistent) {
					continue;
				}
				if (storage.get_page_state(page.stable_id) == VirtualGeometryPageState::ACTIVE) {
					storage.mark_page_used(page.stable_id, serial, serial, serial);
					if (storage.retire_page(page.stable_id) == OK) {
						storage.process_retirements(serial);
						CHECK_EQ(storage.get_page_state(page.stable_id), VirtualGeometryPageState::UNLOADED);
						const VirtualGeometryPageDiagnostics retired = storage.get_page_diagnostics(page.stable_id);
						CHECK(retired.generation > 1);
					}
					break;
				}
			}
		}
	}
	CHECK(saw_independent_eye_visibility);
	CHECK(saw_history_transition);
	for (const PageDescriptor &page : package.manifest.pages) {
		if (page.persistent) {
			CHECK_EQ(storage.get_page_state(page.stable_id), VirtualGeometryPageState::ACTIVE);
		}
	}

	// A new ray generation cannot retire until its previous submission is
	// complete, and a failed replacement retains the active coarse generation.
	const uint64_t old_version = rays.get_active_version(ray_group.stable_id);
	REQUIRE_EQ(rays.queue_build(ray_group.stable_id, 2), OK);
	REQUIRE_EQ(rays.begin_build(ray_group.stable_id, 241, 4096, false), OK);
	REQUIRE_EQ(rays.fail_build(ray_group.stable_id), OK);
	CHECK_EQ(rays.get_active_version(ray_group.stable_id), old_version);
	rays.mark_used(ray_group.stable_id, 242);
	rays.process_retirements(241);
	CHECK_EQ(rays.get_active_version(ray_group.stable_id), old_version);
	REQUIRE_EQ(rays.queue_build(ray_group.stable_id, 2), OK);
	REQUIRE_EQ(rays.begin_build(ray_group.stable_id, 243, 4096, false), OK);
	REQUIRE_EQ(rays.complete_build(ray_group.stable_id, 243, 0), OK);
	rays.activate_frame_boundary(243, 244);
	CHECK_EQ(rays.get_active_version(ray_group.stable_id), uint64_t(2));
	rays.process_retirements(243);
	CHECK_EQ(rays.get_diagnostics().retirements, uint64_t(0));
	rays.process_retirements(244);
	CHECK_EQ(rays.get_diagnostics().retirements, uint64_t(1));
}

} // namespace TestVirtualGeometryStereoLongRun
