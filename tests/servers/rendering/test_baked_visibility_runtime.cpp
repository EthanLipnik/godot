/**************************************************************************/
/*  test_baked_visibility_runtime.cpp                                    */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_baked_visibility_runtime)

#include "scene/3d/baked_visibility_volume_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "core/io/marshalls.h"
#include "servers/rendering/baked_visibility/baked_visibility_runtime.h"

namespace TestBakedVisibilityRuntime {

static Ref<BakedVisibilityData3D> make_data(const Vector3i &p_grid, const Vector<Vector<uint32_t>> &p_sets, const Vector<BakedVisibilityData3DData::Cell> &p_cells, uint32_t p_flags = 0, float p_distance = 64.0f, bool p_include_light = false) {
	BakedVisibilityData3DData data;
	data.source_uid = 1;
	data.source_path = "res://test.tscn";
	data.source_sha256.resize(32);
	data.bake_mask = 1;
	data.local_bounds = AABB(Vector3(), Vector3(p_grid.x * 10.0f, 10.0f, 10.0f));
	data.cell_size = Vector3(10, 10, 10);
	data.transport_distance = p_distance;
	data.grid_size = p_grid;
	data.instances.resize(p_include_light ? 2 : 1);
	data.instances.write[0].path = NodePath("Mesh");
	data.instances.write[0].flags = p_flags;
	data.instances.write[0].local_bounds = AABB(Vector3(), Vector3(1, 1, 1));
	data.instances.write[0].signature_sha256.resize(32);
	if (p_include_light) {
		data.instances.write[1].path = NodePath("Light");
		data.instances.write[1].kind = BakedVisibilityData3DData::INSTANCE_KIND_POSITIONAL_LIGHT;
		data.instances.write[1].local_bounds = AABB(Vector3(), Vector3(1, 1, 1));
		data.instances.write[1].signature_sha256.resize(32);
	}
	data.sets = p_sets;
	data.cells = p_cells;
	Ref<BakedVisibilityData3D> resource;
	resource.instantiate();
	CHECK_EQ(resource->set_baked_data(data), OK);
	return resource;
}

static BakedVisibilityRuntimeCandidate candidate(const MeshInstance3D &p_mesh) {
	BakedVisibilityRuntimeCandidate value;
	value.rid = p_mesh.get_instance().get_id();
	value.kind = BakedVisibilityData3DData::INSTANCE_KIND_GEOMETRY;
	value.layer_mask = 1;
	value.visible = true;
	value.world_aabb = AABB(Vector3(), Vector3(1, 1, 1));
	value.base_aabb = AABB(Vector3(), Vector3(1, 1, 1));
	value.world_transform = Transform3D();
	return value;
}

TEST_CASE("[Rendering][BakedVisibility] runtime selects primary and transport cell sets") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets;
	sets.push_back(Vector<uint32_t>());
	sets.push_back(Vector<uint32_t> { 0 });
	BakedVisibilityData3DData::Cell cell;
	cell.primary_set = 1;
	cell.transport_set = 1;
	Vector<BakedVisibilityData3DData::Cell> cells { cell };
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(make_data(Vector3i(1, 1, 1), sets, cells));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	Vector<BakedVisibilityRuntimeCandidate> candidates { candidate(mesh) };
	Vector<Vector3> eyes { Vector3(5, 5, 5) };
	const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, eyes, 1, candidates);
	CHECK(result.active);
	CHECK(result.registered_static.has(mesh.get_instance().get_id()));
	CHECK(result.primary.has(mesh.get_instance().get_id()));
	CHECK(result.transport.has(mesh.get_instance().get_id()));
	CHECK_EQ(result.primary_count, 1);
	CHECK_EQ(result.transport_count, 1);
	CHECK(result.admits_primary(mesh.get_instance().get_id()));
	CHECK(result.admits_transport(mesh.get_instance().get_id()));
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

TEST_CASE("[Rendering][BakedVisibility] runtime reports geometry counts independently of lights") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	OmniLight3D light;
	mesh.set_name("Mesh");
	light.set_name("Light");
	volume.add_child(&mesh);
	volume.add_child(&light);
	Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0, 1 } };
	BakedVisibilityData3DData::Cell cell;
	cell.primary_set = 1;
	cell.transport_set = 1;
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(make_data(Vector3i(1, 1, 1), sets, Vector<BakedVisibilityData3DData::Cell> { cell }, 0, 64.0f, true));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	BakedVisibilityRuntimeCandidate light_candidate = candidate(mesh);
	light_candidate.rid = light.get_instance().get_id();
	light_candidate.kind = BakedVisibilityData3DData::INSTANCE_KIND_POSITIONAL_LIGHT;
	const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 1, Vector<BakedVisibilityRuntimeCandidate> { candidate(mesh), light_candidate });
	CHECK(result.active);
	CHECK_EQ(result.primary_count, 2);
	CHECK_EQ(result.registered_static_geometry_count, 1);
	CHECK_EQ(result.registered_static_light_count, 1);
	CHECK_EQ(result.primary_geometry_count, 1);
	CHECK_EQ(result.primary_light_count, 1);
	CHECK_EQ(result.transport_geometry_count, 1);
	CHECK_EQ(result.transport_light_count, 1);
	const BakedVisibilityRuntimeStats stats = BakedVisibilityRuntime::get_singleton().get_last_query_stats(0);
	CHECK(stats.available);
	CHECK_EQ(stats.primary_geometry_count, 1);
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
	volume.remove_child(&light);
}

TEST_CASE("[Rendering][BakedVisibility] runtime fails open at boundaries, outside, and stereo gaps") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets;
	sets.push_back(Vector<uint32_t>());
	BakedVisibilityData3DData::Cell cell;
	Vector<BakedVisibilityData3DData::Cell> cells { cell };
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(make_data(Vector3i(1, 1, 1), sets, cells));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	Vector<BakedVisibilityRuntimeCandidate> candidates { candidate(mesh) };
	CHECK(BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(0.05f, 5, 5) }, 1, candidates).fail_open);
	CHECK(BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5), Vector3(11, 5, 5) }, 1, candidates).fail_open);
	CHECK(BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 2, candidates).fail_open);
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

TEST_CASE("[Rendering][BakedVisibility] moved blocker fails open while nonblocker detaches") {
	for (uint32_t flags : { uint32_t(0), uint32_t(BakedVisibilityData3DData::INSTANCE_FLAG_CERTIFIED_BLOCKER) }) {
		BakedVisibilityVolume3D volume;
		MeshInstance3D mesh;
		mesh.set_name("Mesh");
		volume.add_child(&mesh);
		Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
		BakedVisibilityData3DData::Cell cell;
		cell.primary_set = 1;
		cell.transport_set = 1;
		volume.set_bake_root(NodePath("."));
		volume.set_bake_mask(1);
		volume.set_data(make_data(Vector3i(1, 1, 1), sets, Vector<BakedVisibilityData3DData::Cell> { cell }, flags));
		BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
		BakedVisibilityRuntimeCandidate changed = candidate(mesh);
		changed.world_transform.origin.x = 2.0f;
		const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 1, Vector<BakedVisibilityRuntimeCandidate> { changed });
		CHECK_EQ(result.fail_open, flags != 0);
		if (!flags) CHECK(result.admits_primary(mesh.get_instance().get_id()));
		BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
		volume.remove_child(&mesh);
	}
}

TEST_CASE("[Rendering][BakedVisibility] degraded cell and D contract are explicit") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
	BakedVisibilityData3DData::Cell cell;
	cell.flags = BakedVisibilityData3DData::CELL_FLAG_DEGRADED;
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(make_data(Vector3i(1, 1, 1), sets, Vector<BakedVisibilityData3DData::Cell> { cell }, 0, 64.0f));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	CHECK(BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 1, Vector<BakedVisibilityRuntimeCandidate> { candidate(mesh) }).fail_open);
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

TEST_CASE("[Rendering][BakedVisibility] runtime unions overlapping volume sets") {
	BakedVisibilityVolume3D first_volume;
	BakedVisibilityVolume3D second_volume;
	MeshInstance3D first_mesh;
	MeshInstance3D second_mesh;
	first_mesh.set_name("Mesh");
	second_mesh.set_name("Mesh");
	first_volume.add_child(&first_mesh);
	second_volume.add_child(&second_mesh);
	Vector<Vector<uint32_t>> selected_sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
	Vector<Vector<uint32_t>> empty_sets { Vector<uint32_t>() };
	BakedVisibilityData3DData::Cell selected_cell;
	selected_cell.primary_set = 1;
	selected_cell.transport_set = 1;
	BakedVisibilityData3DData::Cell empty_cell;
	first_volume.set_bake_root(NodePath("."));
	first_volume.set_bake_mask(1);
	first_volume.set_data(make_data(Vector3i(1, 1, 1), selected_sets, Vector<BakedVisibilityData3DData::Cell> { selected_cell }));
	second_volume.set_bake_root(NodePath("."));
	second_volume.set_bake_mask(1);
	second_volume.set_data(make_data(Vector3i(1, 1, 1), empty_sets, Vector<BakedVisibilityData3DData::Cell> { empty_cell }));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&first_volume);
	BakedVisibilityRuntime::get_singleton().register_test_volume(&second_volume);
	const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 1, Vector<BakedVisibilityRuntimeCandidate> { candidate(first_mesh), candidate(second_mesh) });
	CHECK(result.active);
	CHECK_EQ(result.volume_count, 2);
	CHECK_EQ(result.cell_count, 2);
	CHECK_EQ(result.registered_static.size(), 2);
	CHECK(result.primary.has(first_mesh.get_instance().get_id()));
	CHECK(result.transport.has(first_mesh.get_instance().get_id()));
	CHECK(!result.primary.has(second_mesh.get_instance().get_id()));
	CHECK(!result.transport.has(second_mesh.get_instance().get_id()));
	BakedVisibilityRuntime::get_singleton().unregister_volume(first_volume.get_instance_id());
	BakedVisibilityRuntime::get_singleton().unregister_volume(second_volume.get_instance_id());
	first_volume.remove_child(&first_mesh);
	second_volume.remove_child(&second_mesh);
}

TEST_CASE("[Rendering][BakedVisibility] runtime unions cells crossed by lookup margin") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
	BakedVisibilityData3DData::Cell first_cell;
	BakedVisibilityData3DData::Cell second_cell;
	second_cell.primary_set = 1;
	second_cell.transport_set = 1;
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(make_data(Vector3i(2, 1, 1), sets, Vector<BakedVisibilityData3DData::Cell> { first_cell, second_cell }));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(9.95f, 5, 5) }, 1, Vector<BakedVisibilityRuntimeCandidate> { candidate(mesh) });
	CHECK(result.active);
	CHECK_EQ(result.cell_count, 2);
	CHECK(result.primary.has(mesh.get_instance().get_id()));
	CHECK(result.transport.has(mesh.get_instance().get_id()));
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

TEST_CASE("[Rendering][BakedVisibility] runtime isolates corrupt leaf chunks and fails open only for that lookup") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
	Vector<BakedVisibilityData3DData::Cell> cells;
	cells.resize(16);
	for (int index = 0; index < cells.size(); index++) {
		cells.write[index].primary_set = 1;
		cells.write[index].transport_set = 1;
	}
	Ref<BakedVisibilityData3D> resource = make_data(Vector3i(16, 1, 1), sets, cells);
	PackedByteArray payload = resource->get_payload();
	const uint32_t metadata_size = decode_uint32(payload.ptr() + 12);
	payload.write[20 + metadata_size + 4] ^= 1;
	resource->set_payload(payload);
	REQUIRE(resource->is_valid());
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(resource);
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	const Vector<BakedVisibilityRuntimeCandidate> candidates { candidate(mesh) };
	const BakedVisibilityRuntimeResult healthy = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(95, 5, 5) }, 1, candidates);
	CHECK(healthy.active);
	CHECK_FALSE(healthy.fail_open);
	const BakedVisibilityRuntimeResult corrupted = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 1, candidates);
	CHECK(corrupted.fail_open);
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

TEST_CASE("[Rendering][BakedVisibility] runtime bounds the leaf cache and redecodes evicted leaves") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
	Vector<BakedVisibilityData3DData::Cell> cells;
	cells.resize(72);
	for (int index = 0; index < cells.size(); index++) {
		cells.write[index].primary_set = 1;
		cells.write[index].transport_set = 1;
	}
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	Ref<BakedVisibilityData3D> resource = make_data(Vector3i(72, 1, 1), sets, cells);
	REQUIRE_MESSAGE(resource->is_valid(), resource->get_validation_error());
	Vector<uint32_t> decoded_indices;
	Vector<BakedVisibilityData3DData::Cell> decoded_cells;
	Vector<Vector<uint32_t>> decoded_sets;
	String leaf_error;
	REQUIRE_MESSAGE(BakedVisibilityCodec::decode_leaf_payload(*resource->get_baked_data(), 0, decoded_indices, decoded_cells, decoded_sets, &leaf_error) == OK, leaf_error);
	volume.set_data(resource);
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	const Vector<BakedVisibilityRuntimeCandidate> candidates { candidate(mesh) };
	for (int leaf = 0; leaf < 9; leaf++) {
		const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5.0f + leaf * 80.0f, 5, 5) }, 1, candidates);
		INFO(result.reason);
		CHECK_FALSE(result.fail_open);
	}
	BakedVisibilityRuntimeStats stats = BakedVisibilityRuntime::get_singleton().get_last_query_stats(0);
	CHECK_EQ(stats.cached_leaf_count, 8);
	CHECK_EQ(stats.leaf_decode_count, 9);
	CHECK_EQ(stats.leaf_eviction_count, 1);
	CHECK_FALSE(BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(5, 5, 5) }, 1, candidates).fail_open);
	stats = BakedVisibilityRuntime::get_singleton().get_last_query_stats(0);
	CHECK_EQ(stats.cached_leaf_count, 8);
	CHECK_EQ(stats.leaf_decode_count, 10);
	CHECK_EQ(stats.leaf_eviction_count, 2);
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

TEST_CASE("[Rendering][BakedVisibility] runtime resolves a cell through its tile parent chain") {
	BakedVisibilityVolume3D volume;
	MeshInstance3D mesh;
	mesh.set_name("Mesh");
	volume.add_child(&mesh);
	Vector<Vector<uint32_t>> sets { Vector<uint32_t>(), Vector<uint32_t> { 0 } };
	Vector<BakedVisibilityData3DData::Cell> cells;
	cells.resize(9);
	cells.write[8].primary_set = 1;
	cells.write[8].transport_set = 1;
	volume.set_bake_root(NodePath("."));
	volume.set_bake_mask(1);
	volume.set_data(make_data(Vector3i(9, 1, 1), sets, cells));
	BakedVisibilityRuntime::get_singleton().register_test_volume(&volume);
	const BakedVisibilityRuntimeResult result = BakedVisibilityRuntime::get_singleton().query(0, Vector<Vector3> { Vector3(85, 5, 5) }, 1, Vector<BakedVisibilityRuntimeCandidate> { candidate(mesh) });
	CHECK(result.active);
	CHECK(result.primary.has(mesh.get_instance().get_id()));
	CHECK_GT(result.tile_count, 2);
	BakedVisibilityRuntime::get_singleton().unregister_volume(volume.get_instance_id());
	volume.remove_child(&mesh);
}

} // namespace TestBakedVisibilityRuntime
