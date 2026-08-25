/**************************************************************************/
/*  test_virtual_geometry.h                                               */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

#include "modules/meshoptimizer/virtual_geometry_compiler.h"
#include "modules/meshoptimizer/virtual_geometry.h"
#include "modules/meshoptimizer/virtual_geometry_instance_3d.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "scene/resources/material.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_raster.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_storage.h"

namespace TestVirtualGeometry {

static VirtualGeometryCompiler::Input _make_grid(int p_side) {
	VirtualGeometryCompiler::Input input;
	input.source_asset_identity = 0x1001;
	input.source_primitive_identity = 0x2002;
	for (int z = 0; z < p_side; z++) {
		for (int x = 0; x < p_side; x++) {
			input.positions.push_back(Vector3(x, real_t(x * x + z * z) * 0.002, z));
			input.normals.push_back(Vector3(0, 1, 0));
			input.uv0.push_back(Vector2(x, z));
		}
	}
	for (int z = 0; z < p_side - 1; z++) for (int x = 0; x < p_side - 1; x++) {
		const int a = z * p_side + x;
		const int b = a + 1;
		const int c = a + p_side;
		const int d = c + 1;
		input.indices.push_back(a);
		input.indices.push_back(c);
		input.indices.push_back(b);
		input.indices.push_back(b);
		input.indices.push_back(c);
		input.indices.push_back(d);
		input.triangle_materials.push_back((x / 4) & 1);
		input.triangle_materials.push_back((x / 4) & 1);
	}
	return input;
}

TEST_CASE("[VirtualGeometry] VG1 compiler emits deterministic deep portable packages") {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_triangles_per_cluster = 16;
	settings.max_vertices_per_cluster = 16;
	settings.clusters_per_group = 4;
	settings.max_decoded_page_bytes = 4096;
	RendererVirtualGeometry::Package first;
	RendererVirtualGeometry::Package second;
	// This size forces several bounded group-reduction rounds per semantic
	// partition; a shallow two-level hierarchy is not an acceptable VG1 result.
	const VirtualGeometryCompiler::Input input = _make_grid(17);
	CHECK(compiler.compile(input, settings, first) == OK);
	CHECK(compiler.compile(input, settings, second) == OK);
	CHECK(RendererVirtualGeometry::validate_package(first) == OK);
	CHECK(first.manifest.groups.size() > 2);
	CHECK(first.manifest.pages.size() > 1);
	REQUIRE(first.manifest.clusters.size() == second.manifest.clusters.size());
	for (int i = 0; i < first.manifest.clusters.size(); i++) {
		CHECK(first.manifest.clusters[i].stable_id == second.manifest.clusters[i].stable_id);
		CHECK(first.manifest.clusters[i].bounds.encloses(first.manifest.resource_bounds.intersection(first.manifest.clusters[i].bounds)));
	}
	REQUIRE(first.compressed_pages.size() == second.compressed_pages.size());
	for (int i = 0; i < first.compressed_pages.size(); i++) CHECK(first.compressed_pages[i] == second.compressed_pages[i]);
	VirtualGeometryCompiler::Settings repacked_settings = settings;
	repacked_settings.max_decoded_page_bytes = 64 * 1024;
	RendererVirtualGeometry::Package repacked;
	CHECK(compiler.compile(input, repacked_settings, repacked) == OK);
	REQUIRE(repacked.manifest.clusters.size() == first.manifest.clusters.size());
	for (int i = 0; i < first.manifest.clusters.size(); i++) {
		CHECK(repacked.manifest.clusters[i].stable_id == first.manifest.clusters[i].stable_id);
		CHECK(repacked.manifest.clusters[i].position_decode_scale == Vector3(1, 1, 1));
	}
	for (const RendererVirtualGeometry::RefinementGroupDescriptor &group : first.manifest.groups) {
		CHECK_FALSE(group.coarse_cluster_ids.is_empty());
		CHECK_FALSE(group.fine_cluster_ids.is_empty());
		CHECK(group.geometric_error >= 0.0);
	}
	bool reduced_group = false;
	bool nonzero_error = false;
	for (const RendererVirtualGeometry::RefinementGroupDescriptor &group : first.manifest.groups) {
		uint32_t fine_triangles = 0;
		uint32_t coarse_triangles = 0;
		for (uint64_t id : group.fine_cluster_ids) for (const RendererVirtualGeometry::ClusterDescriptor &cluster : first.manifest.clusters) if (cluster.stable_id == id) fine_triangles += cluster.triangle_count;
		for (uint64_t id : group.coarse_cluster_ids) for (const RendererVirtualGeometry::ClusterDescriptor &cluster : first.manifest.clusters) if (cluster.stable_id == id) {
			coarse_triangles += cluster.triangle_count;
			CHECK(cluster.bounds.encloses(group.bounds));
		}
		reduced_group |= coarse_triangles < fine_triangles;
		nonzero_error |= group.geometric_error > 0.0;
	}
	CHECK(reduced_group);
	CHECK(nonzero_error);
}

TEST_CASE("[VirtualGeometry] VG1 validates malformed streams and declares conventional content") {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	RendererVirtualGeometry::Package package;
	VirtualGeometryCompiler::Input malformed = _make_grid(3);
	malformed.indices.write[0] = 999;
	ERR_PRINT_OFF;
	CHECK(compiler.compile(malformed, settings, package) == ERR_INVALID_DATA);
	ERR_PRINT_ON;
	VirtualGeometryCompiler::Input conventional = _make_grid(3);
	conventional.opaque = false;
	conventional.conventional_path_reason = "Blended material remains on the conventional path.";
	CHECK(compiler.compile(conventional, settings, package) == OK);
	CHECK(package.manifest.clusters.is_empty());
	CHECK(package.manifest.conventional_path_diagnostics.size() == 1);
	CHECK(compiler.get_diagnostics().size() == 1);
}

TEST_CASE("[VirtualGeometry] VG1 page payload checksum and range validation fail closed") {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_triangles_per_cluster = 4;
	RendererVirtualGeometry::Package package;
	CHECK(compiler.compile(_make_grid(5), settings, package) == OK);
	REQUIRE(!package.compressed_pages.is_empty());
	package.compressed_pages.write[0].set(0, package.compressed_pages[0][0] ^ 1);
	ERR_PRINT_OFF;
	CHECK(RendererVirtualGeometry::validate_package(package) != OK);
	ERR_PRINT_ON;
	CHECK(compiler.compile(_make_grid(5), settings, package) == OK);
	package.manifest.clusters.write[0].payload_offset = uint64_t(INT64_MAX);
	ERR_PRINT_OFF;
	CHECK(RendererVirtualGeometry::validate_package(package) != OK);
	ERR_PRINT_ON;
}

TEST_CASE("[VirtualGeometry] VG1 declares an exact protected-border fallback") {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Input input;
	input.source_asset_identity = 7;
	input.source_primitive_identity = 9;
	input.positions.push_back(Vector3(0, 0, 0)); input.positions.push_back(Vector3(1, 0, 0)); input.positions.push_back(Vector3(0, 1, 0));
	input.positions.push_back(Vector3(3, 0, 0)); input.positions.push_back(Vector3(4, 0, 0)); input.positions.push_back(Vector3(3, 1, 0));
	for (int index : { 0, 1, 2, 3, 4, 5 }) input.indices.push_back(index);
	VirtualGeometryCompiler::Settings settings;
	settings.max_triangles_per_cluster = 1;
	settings.clusters_per_group = 2;
	RendererVirtualGeometry::Package package;
	CHECK(compiler.compile(input, settings, package) == OK);
	CHECK_FALSE(compiler.get_diagnostics().is_empty());
	CHECK(RendererVirtualGeometry::validate_package(package) == OK);
}

TEST_CASE("[VirtualGeometry] VG1 resource package round-trips without runtime residency") {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_triangles_per_cluster = 4;
	RendererVirtualGeometry::Package package;
	REQUIRE(compiler.compile(_make_grid(5), settings, package) == OK);
	REQUIRE(!package.manifest.materials.is_empty());
	Ref<VirtualGeometry> source = memnew(VirtualGeometry);
	REQUIRE(source->set_compiled_package(package) == OK);
	const uint64_t package_revision_before_material_edit = source->get_package_revision();
	TypedArray<Material> bindings;
	for (int i = 0; i < package.manifest.materials.size(); i++) {
		bindings.push_back(Ref<Material>(memnew(StandardMaterial3D)));
	}
	source->set_material_bindings(bindings);
	CHECK(source->get_package_revision() == package_revision_before_material_edit);
	const PackedByteArray original_blob = source->get_serialized_package();
	REQUIRE(!original_blob.is_empty());
	CHECK(source->get_package_revision() != 0);
	const String path = OS::get_singleton()->get_cache_path().path_join("virtual_geometry_roundtrip.tres");
	const String second_path = OS::get_singleton()->get_cache_path().path_join("virtual_geometry_roundtrip_second.tres");
	CHECK(ResourceSaver::save(source, path) == OK);
	Error load_error = OK;
	Ref<VirtualGeometry> loaded = ResourceLoader::load(path, "VirtualGeometry", ResourceFormatLoader::CACHE_MODE_IGNORE, &load_error);
	REQUIRE(load_error == OK);
	REQUIRE(loaded.is_valid());
	CHECK(loaded.ptr() != source.ptr());
	CHECK(loaded->is_valid_virtual_geometry());
	CHECK(loaded->get_source_asset_identity() == source->get_source_asset_identity());
	CHECK(loaded->get_source_primitive_identity() == source->get_source_primitive_identity());
	CHECK(loaded->get_compiled_package().manifest.pages.size() == package.manifest.pages.size());
	CHECK(loaded->get_compiled_package().manifest.clusters.size() == package.manifest.clusters.size());
	CHECK(loaded->get_compiled_package().manifest.materials.size() == package.manifest.materials.size());
	CHECK(loaded->get_material_bindings().size() == bindings.size());
	CHECK(loaded->has_complete_material_bindings());
	CHECK(loaded->get_package_revision() == source->get_package_revision());
	CHECK(loaded->get_serialized_package() == original_blob);
	CHECK(ResourceSaver::save(loaded, second_path) == OK);
	CHECK(FileAccess::get_file_as_bytes(path) == FileAccess::get_file_as_bytes(second_path));
	Ref<Material> replacement = memnew(StandardMaterial3D);
	bindings[0] = replacement;
	loaded->set_material_bindings(bindings);
	CHECK(loaded->get_package_revision() == package_revision_before_material_edit);
	CHECK(loaded->has_complete_material_bindings());
	CHECK(loaded->get_compiled_package().manifest.pages.size() == package.manifest.pages.size());
	PackedByteArray corrupt = original_blob;
	corrupt.write[corrupt.size() - 1] ^= 1;
	ERR_PRINT_OFF;
	CHECK(source->set_serialized_package(corrupt) != OK);
	PackedByteArray truncated = original_blob;
	REQUIRE(truncated.resize(truncated.size() - 1) == OK);
	CHECK(source->set_serialized_package(truncated) != OK);
	ERR_PRINT_ON;
	CHECK(source->get_serialized_package() == original_blob);
	CHECK(source->set_serialized_package(PackedByteArray()) == OK);
	CHECK_FALSE(source->is_valid_virtual_geometry());
	CHECK(source->get_source_asset_identity() == 0);
	DirAccess::remove_absolute(path);
	DirAccess::remove_absolute(second_path);
}

TEST_CASE("[VirtualGeometry] F6 serializes five resources for fifteen pure-VG submissions") {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_triangles_per_cluster = 4;
	settings.max_vertices_per_cluster = 8;
	settings.clusters_per_group = 2;
	settings.max_decoded_page_bytes = 512;
	const String cache_path = OS::get_singleton()->get_cache_path();
	Vector<Ref<VirtualGeometry>> resources;
	resources.reserve(5);

	for (uint32_t resource_index = 0; resource_index < 5; resource_index++) {
		VirtualGeometryCompiler::Input input = _make_grid(9);
		input.source_asset_identity += resource_index + 1;
		input.source_primitive_identity += resource_index + 1;
		RendererVirtualGeometry::Package package;
		REQUIRE_EQ(compiler.compile(input, settings, package), OK);
		REQUIRE_EQ(package.manifest.materials.size(), 2);
		Ref<VirtualGeometry> source = memnew(VirtualGeometry);
		REQUIRE_EQ(source->set_compiled_package(package), OK);
		TypedArray<Material> bindings;
		bindings.push_back(Ref<Material>(memnew(StandardMaterial3D)));
		bindings.push_back(Ref<Material>(memnew(StandardMaterial3D)));
		source->set_material_bindings(bindings);
		const String path = cache_path.path_join(vformat("virtual_geometry_f6_multi_%d.tres", resource_index));
		REQUIRE_EQ(ResourceSaver::save(source, path), OK);
		Error load_error = OK;
		Ref<VirtualGeometry> loaded = ResourceLoader::load(path, "VirtualGeometry", ResourceFormatLoader::CACHE_MODE_IGNORE, &load_error);
		REQUIRE_EQ(load_error, OK);
		REQUIRE(loaded.is_valid());
		CHECK(loaded.ptr() != source.ptr());
		CHECK(loaded->has_complete_material_bindings());
		CHECK_EQ(loaded->get_compiled_package().manifest.materials.size(), 2);
		resources.push_back(loaded);
	}

	uint32_t submitted_instances = 0;
	uint32_t submitted_nonzero_commands = 0;
	uint64_t bounded_command_records = 0;
	for (uint32_t resource_index = 0; resource_index < uint32_t(resources.size()); resource_index++) {
		RendererVirtualGeometry::VirtualGeometryStorage storage;
		REQUIRE_EQ(storage.set_package(resources[resource_index]->get_compiled_package(), resources[resource_index]->get_package_revision()), OK);
		for (;;) {
			const Vector<uint64_t> requests = storage.take_io_requests(64);
			if (requests.is_empty()) break;
			for (uint64_t page_id : requests) {
				const RendererVirtualGeometry::VirtualGeometryPageDiagnostics diagnostics = storage.get_page_diagnostics(page_id);
				PackedByteArray decoded;
				String error;
				REQUIRE_EQ(storage.decode_page_on_worker(page_id, diagnostics.generation, decoded, error), OK);
				storage.enqueue_worker_completion(page_id, diagnostics.generation, decoded, OK);
			}
			storage.render_process(resource_index, resource_index + 1);
			storage.notify_submission_completed(resource_index + 1);
		}
		REQUIRE(storage.get_diagnostics().active_pages > 0);
		RendererVirtualGeometry::VirtualGeometryRasterSelectionInput input;
		input.command_capacity = 4096;
		RendererVirtualGeometry::VirtualGeometryRasterView view;
		view.camera_position = Vector3(0, 0, 8);
		input.views.push_back(view);
		for (uint32_t shared_instance = 0; shared_instance < 3; shared_instance++) {
			VirtualGeometryInstance3D instance;
			instance.set_virtual_geometry(resources[resource_index]);
			instance.set_semantic_instance_id(0xF6000000ull + resource_index * 3 + shared_instance + 1);
			const RendererVirtualGeometry::VirtualGeometryRasterSelection selection = RendererVirtualGeometry::VirtualGeometryRasterSelector::select(resources[resource_index]->get_compiled_package(), storage.get_active_cluster_ids(), input);
			CHECK_EQ(selection.commands.size(), 4096);
			uint32_t nonzero_commands = 0;
			for (const RendererVirtualGeometry::VirtualGeometryIndexedIndirectCommand &command : selection.commands) {
				nonzero_commands += command.index_count != 0 && command.instance_count != 0;
			}
			CHECK(nonzero_commands > 0);
			submitted_instances++;
			submitted_nonzero_commands += nonzero_commands;
			bounded_command_records += selection.commands.size();
		}
	}
	CHECK_EQ(resources.size(), 5);
	CHECK_EQ(submitted_instances, 15);
	CHECK_EQ(submitted_nonzero_commands > 0, true);
	CHECK_LE(bounded_command_records, uint64_t(15) * 4096);
	CHECK_LE(bounded_command_records, uint64_t(65536));
	for (uint32_t resource_index = 0; resource_index < 5; resource_index++) {
		DirAccess::remove_absolute(cache_path.path_join(vformat("virtual_geometry_f6_multi_%d.tres", resource_index)));
	}
}

} // namespace TestVirtualGeometry
