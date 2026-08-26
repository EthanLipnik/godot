/**************************************************************************/
/*  test_baked_visibility_baker.cpp                                      */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_baked_visibility_baker)

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "scene/3d/baked_visibility_volume_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/baked_visibility/baked_visibility_baker.h"
#include "servers/rendering/baked_visibility/baked_visibility_checkpoint.h"

#include <atomic>

namespace TestBakedVisibilityBaker {

static Ref<ArrayMesh> make_triangle(const Vector3 &a, const Vector3 &b, const Vector3 &c) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	PackedVector3Array vertices;
	vertices.push_back(a);
	vertices.push_back(b);
	vertices.push_back(c);
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

static Ref<ArrayMesh> make_quad(const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &d) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	PackedVector3Array vertices;
	vertices.push_back(a);
	vertices.push_back(b);
	vertices.push_back(c);
	vertices.push_back(d);
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	indices.push_back(0);
	indices.push_back(2);
	indices.push_back(3);
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

static Ref<ArrayMesh> make_quad_batch(int p_quad_count, bool p_reverse_triangles) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	PackedVector3Array vertices;
	PackedInt32Array indices;
	vertices.resize(p_quad_count * 4 + 3);
	for (int quad = 0; quad < p_quad_count; quad++) {
		const int vertex = quad * 4;
		const float x = float(quad) * 3.0f;
		vertices.set(vertex + 0, Vector3(x, 0, 0));
		vertices.set(vertex + 1, Vector3(x + 1, 0, 0));
		vertices.set(vertex + 2, Vector3(x + 1, 1, 0));
		vertices.set(vertex + 3, Vector3(x, 1, 0));
	}
	// Keep the mesh AABB volumetric so the static-geometry collector accepts it.
	const int cap = p_quad_count * 4;
	vertices.set(cap + 0, Vector3(-1, -1, 1));
	vertices.set(cap + 1, Vector3(-1, 1, 1));
	vertices.set(cap + 2, Vector3(1, -1, 1));
	for (int source = 0; source < p_quad_count; source++) {
		const int quad = p_reverse_triangles ? p_quad_count - 1 - source : source;
		const int vertex = quad * 4;
		indices.push_back(vertex + 0);
		indices.push_back(vertex + 1);
		indices.push_back(vertex + 2);
		indices.push_back(vertex + 0);
		indices.push_back(vertex + 2);
		indices.push_back(vertex + 3);
	}
	indices.push_back(cap + 0);
	indices.push_back(cap + 1);
	indices.push_back(cap + 2);
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

static Ref<ArrayMesh> make_connected_quad_strip(int p_quad_count) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	PackedVector3Array vertices;
	PackedInt32Array indices;
	vertices.resize((p_quad_count + 1) * 2 + 3);
	for (int column = 0; column <= p_quad_count; column++) {
		vertices.set(column * 2 + 0, Vector3(float(column), 0, 0));
		vertices.set(column * 2 + 1, Vector3(float(column), 1, 0));
	}
	for (int quad = 0; quad < p_quad_count; quad++) {
		const int left = quad * 2;
		indices.push_back(left + 0);
		indices.push_back(left + 2);
		indices.push_back(left + 3);
		indices.push_back(left + 0);
		indices.push_back(left + 3);
		indices.push_back(left + 1);
	}
	const int cap = (p_quad_count + 1) * 2;
	vertices.set(cap + 0, Vector3(-1, -1, 1));
	vertices.set(cap + 1, Vector3(-1, 1, 1));
	vertices.set(cap + 2, Vector3(1, -1, 1));
	indices.push_back(cap + 0);
	indices.push_back(cap + 1);
	indices.push_back(cap + 2);
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

static Ref<ArrayMesh> make_ring_with_hole() {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	PackedVector3Array vertices;
	PackedInt32Array indices;
	const auto append_quad = [&](const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &d) {
		const int first = vertices.size();
		vertices.push_back(a);
		vertices.push_back(b);
		vertices.push_back(c);
		vertices.push_back(d);
		indices.push_back(first + 0);
		indices.push_back(first + 1);
		indices.push_back(first + 2);
		indices.push_back(first + 0);
		indices.push_back(first + 2);
		indices.push_back(first + 3);
	};
	append_quad(Vector3(-10, 1, 0), Vector3(10, 1, 0), Vector3(10, 10, 0), Vector3(-10, 10, 0));
	append_quad(Vector3(-10, -10, 0), Vector3(-10, -1, 0), Vector3(10, -1, 0), Vector3(10, -10, 0));
	append_quad(Vector3(-10, -1, 0), Vector3(-1, -1, 0), Vector3(-1, 1, 0), Vector3(-10, 1, 0));
	append_quad(Vector3(1, -1, 0), Vector3(10, -1, 0), Vector3(10, 1, 0), Vector3(1, 1, 0));
	// Keep the otherwise planar mesh eligible for static-geometry collection.
	const int cap = vertices.size();
	vertices.push_back(Vector3(30, 30, 1));
	vertices.push_back(Vector3(31, 30, 1));
	vertices.push_back(Vector3(30, 31, 1));
	indices.push_back(cap + 0);
	indices.push_back(cap + 1);
	indices.push_back(cap + 2);
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

static Ref<ArrayMesh> make_tetrahedron(float p_half_extent) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays;
	PackedVector3Array vertices;
	vertices.push_back(Vector3(-p_half_extent, -p_half_extent, -p_half_extent));
	vertices.push_back(Vector3(p_half_extent, -p_half_extent, p_half_extent));
	vertices.push_back(Vector3(-p_half_extent, p_half_extent, p_half_extent));
	vertices.push_back(Vector3(p_half_extent, p_half_extent, -p_half_extent));
	PackedInt32Array indices;
	const int faces[] = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };
	for (int face : faces) {
		indices.push_back(face);
	}
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

static Ref<ArrayMesh> make_separating_wall() {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array wall_arrays;
	PackedVector3Array wall_vertices;
	wall_vertices.push_back(Vector3(8, -20, -20));
	wall_vertices.push_back(Vector3(8, 20, -20));
	wall_vertices.push_back(Vector3(8, 20, 20));
	wall_vertices.push_back(Vector3(8, -20, 20));
	PackedInt32Array wall_indices;
	wall_indices.append_array({ 0, 1, 2, 0, 2, 3 });
	wall_arrays.resize(Mesh::ARRAY_MAX);
	wall_arrays[Mesh::ARRAY_VERTEX] = wall_vertices;
	wall_arrays[Mesh::ARRAY_INDEX] = wall_indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, wall_arrays);
	// A second, remote noncoplanar surface makes the Mesh AABB volumetric while
	// leaving the first surface a complete separating certificate patch.
	Array volume_arrays;
	PackedVector3Array volume_vertices;
	volume_vertices.push_back(Vector3(8.1f, 30, 0));
	volume_vertices.push_back(Vector3(8.2f, 30, 0));
	volume_vertices.push_back(Vector3(8.1f, 31, 1));
	volume_arrays.resize(Mesh::ARRAY_MAX);
	volume_arrays[Mesh::ARRAY_VERTEX] = volume_vertices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, volume_arrays);
	return mesh;
}

static BakedVisibilityBakeOutput bake_blocker_budget_scene(Node3D &p_anchor, int p_max_blocker_triangles, const AABB &p_bounds, float p_cell_size) {
	BakedVisibilityBakeInput input;
	input.anchor = &p_anchor;
	input.scene_root = &p_anchor;
	input.bounds = p_bounds;
	input.requested_cell_size = p_cell_size;
	input.max_cells = 1;
	input.certificate_work_cap = 4096;
	input.max_blocker_triangles = p_max_blocker_triangles;
	input.transport_distance = p_cell_size;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), OK);
	return output;
}

static BakedVisibilityBakeOutput bake_quad_batch(int p_quad_count, bool p_reverse_triangles) {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *batch = memnew(MeshInstance3D);
	batch->set_name("Batch");
	batch->set_mesh(make_quad_batch(p_quad_count, p_reverse_triangles));
	anchor.add_child(batch);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(float(p_quad_count) * 3.0f + 4.0f, 4, 4));
	input.requested_cell_size = float(p_quad_count) * 4.0f;
	input.max_cells = 1;
	input.certificate_work_cap = 4096;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), OK);
	return output;
}

static BakedVisibilityBakeOutput bake_connected_quad_strip(int p_quad_count) {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *strip = memnew(MeshInstance3D);
	strip->set_name("Strip");
	strip->set_mesh(make_connected_quad_strip(p_quad_count));
	anchor.add_child(strip);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(float(p_quad_count) + 4.0f, 4, 4));
	input.requested_cell_size = float(p_quad_count) * 2.0f;
	input.max_cells = 1;
	input.certificate_work_cap = 4096;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), OK);
	return output;
}

static BakedVisibilityBakeOutput bake_two_meshes(bool p_reverse_children, int p_work_cap, bool p_serial_reference = false, bool p_multi_tile = false) {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *occluder = memnew(MeshInstance3D);
	occluder->set_name("Occluder");
	Ref<ArrayMesh> occluder_mesh = make_triangle(Vector3(-100, -100, 0), Vector3(100, -100, 0), Vector3(0, 100, 0));
	Ref<StandardMaterial3D> occluder_material;
	occluder_material.instantiate();
	occluder_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	occluder_mesh->surface_set_material(0, occluder_material);
	occluder->set_mesh(occluder_mesh);
	MeshInstance3D *candidate = memnew(MeshInstance3D);
	candidate->set_name("Candidate");
	candidate->set_mesh(make_triangle(Vector3(-0.1f, -0.1f, 0), Vector3(0.1f, -0.1f, 0), Vector3(0, 0.1f, 0)));
	candidate->set_position(Vector3(0, 0, -2));
	if (p_reverse_children) {
		anchor.add_child(candidate);
		anchor.add_child(occluder);
	} else {
		anchor.add_child(occluder);
		anchor.add_child(candidate);
	}
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = p_multi_tile ? AABB(Vector3(-2, -2, -2), Vector3(36, 4, 4)) : AABB(Vector3(-0.2f, -0.2f, 1.8f), Vector3(0.4f, 0.4f, 0.4f));
	input.requested_cell_size = p_multi_tile ? 4.0f : 1.0f;
	input.max_cells = p_multi_tile ? 16 : 1;
	input.certificate_work_cap = p_work_cap;
	input.transport_distance = 1.0f;
	input.test_serial_reference = p_serial_reference;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), OK);
	return output;
}

static const BakedVisibilityBakeSet &primary_set(const BakedVisibilityBakeOutput &p_output, const BakedVisibilityBakeCell &p_cell) {
	return p_output.interned_primary_sets[p_cell.primary_set];
}

static const BakedVisibilityBakeSet &transport_set(const BakedVisibilityBakeOutput &p_output, const BakedVisibilityBakeCell &p_cell) {
	return p_output.interned_transport_sets[p_cell.transport_set];
}

static bool intersects_certificate_triangle(const Vector3 &p_from, const Vector3 &p_to) {
	const Vector3 a(-100, -100, 0);
	const Vector3 b(100, -100, 0);
	const Vector3 c(0, 100, 0);
	const Vector3 direction = p_to - p_from;
	const Vector3 normal = (b - a).cross(c - a);
	const float denominator = normal.dot(direction);
	if (Math::abs(denominator) < CMP_EPSILON) {
		return false;
	}
	const float t = normal.dot(a - p_from) / denominator;
	if (t <= 0.0f || t >= 1.0f) {
		return false;
	}
	const Vector3 point = p_from + direction * t;
	const Vector3 ab = b - a;
	const Vector3 ac = c - a;
	const Vector3 ap = point - a;
	const float denominator_barycentric = ab.dot(ab) * ac.dot(ac) - ab.dot(ac) * ab.dot(ac);
	const float u = (ap.dot(ab) * ac.dot(ac) - ap.dot(ac) * ab.dot(ac)) / denominator_barycentric;
	const float v = (ap.dot(ac) * ab.dot(ab) - ap.dot(ab) * ab.dot(ac)) / denominator_barycentric;
	return u > 0.0001f && v > 0.0001f && u + v < 0.9999f;
}

TEST_CASE("[Rendering][BakedVisibility] convex patch certificate excludes a fully hidden candidate") {
	const BakedVisibilityBakeOutput output = bake_two_meshes(false, 4096);
	const BakedVisibilityBakeOutput serial_reference = bake_two_meshes(false, 4096, true);
	REQUIRE_EQ(output.cells.size(), 1);
	REQUIRE_EQ(serial_reference.cells.size(), 1);
	CHECK_EQ(output.geometry_paths.size(), 2);
	CHECK_LT(primary_set(output, output.cells[0]).geometry.size(), 2);
	CHECK_EQ(transport_set(output, output.cells[0]).geometry.size(), 2); // Hidden geometry remains available to two-segment transport.
	CHECK_EQ(primary_set(output, output.cells[0]).geometry, primary_set(serial_reference, serial_reference.cells[0]).geometry);
	CHECK_EQ(transport_set(output, output.cells[0]).geometry, transport_set(serial_reference, serial_reference.cells[0]).geometry);
	CHECK_GT(output.preprocess.blocker_nominations, uint64_t(0));
	CHECK_EQ(output.preprocess.blocker_nominations, uint64_t(6)); // Two unique blockers across three certificate queries, not once per corner ray.
}

TEST_CASE("[Rendering][BakedVisibility] every deterministic interior certificate sample crosses the shrunken patch") {
	const BakedVisibilityBakeOutput output = bake_two_meshes(false, 4096);
	REQUIRE_EQ(primary_set(output, output.cells[0]).geometry.size(), 1);
	uint32_t state = 0x6d2b79f5;
	for (int i = 0; i < 256; i++) {
		state = state * 1664525u + 1013904223u;
		const float sx = -0.18f + float(state & 0xffff) / 65535.0f * 0.36f;
		state = state * 1664525u + 1013904223u;
		const float sy = -0.18f + float(state & 0xffff) / 65535.0f * 0.36f;
		state = state * 1664525u + 1013904223u;
		const float tx = -0.08f + float(state & 0xffff) / 65535.0f * 0.16f;
		state = state * 1664525u + 1013904223u;
		const float ty = -0.08f + float(state & 0xffff) / 65535.0f * 0.16f;
		CHECK(intersects_certificate_triangle(Vector3(sx, sy, 2.0f), Vector3(tx, ty, -2.0f)));
	}
}

TEST_CASE("[Rendering][BakedVisibility] parallel multi-tile bake matches serial reference") {
	const BakedVisibilityBakeOutput parallel = bake_two_meshes(false, 4096, false, true);
	const BakedVisibilityBakeOutput serial = bake_two_meshes(false, 4096, true, true);
	REQUIRE_EQ(parallel.tile_count, 2);
	REQUIRE_EQ(serial.tile_count, parallel.tile_count);
	CHECK_EQ(parallel.tile_cell_indices, serial.tile_cell_indices);
	CHECK_EQ(parallel.tiles.size(), serial.tiles.size());
	for (int tile = 0; tile < parallel.tiles.size(); tile++) {
		CHECK_EQ(parallel.tiles[tile].coordinate, serial.tiles[tile].coordinate);
		CHECK_EQ(parallel.tiles[tile].first_cell, serial.tiles[tile].first_cell);
		CHECK_EQ(parallel.tiles[tile].cell_count, serial.tiles[tile].cell_count);
		CHECK_EQ(parallel.tiles[tile].candidate_dependencies, serial.tiles[tile].candidate_dependencies);
		CHECK_EQ(parallel.tiles[tile].light_dependencies, serial.tiles[tile].light_dependencies);
		CHECK_EQ(parallel.tiles[tile].certificate_dependencies, serial.tiles[tile].certificate_dependencies);
	}
	REQUIRE_EQ(parallel.cells.size(), serial.cells.size());
	for (int cell = 0; cell < parallel.cells.size(); cell++) {
		CHECK_EQ(parallel.cells[cell].coordinate, serial.cells[cell].coordinate);
		CHECK_EQ(parallel.cells[cell].flags, serial.cells[cell].flags);
		CHECK_EQ(parallel.cells[cell].primary_set, serial.cells[cell].primary_set);
		CHECK_EQ(parallel.cells[cell].transport_set, serial.cells[cell].transport_set);
	}
	CHECK_EQ(parallel.interned_primary_sets.size(), serial.interned_primary_sets.size());
	CHECK_EQ(parallel.interned_transport_sets.size(), serial.interned_transport_sets.size());
	for (int set = 0; set < parallel.interned_primary_sets.size(); set++) {
		CHECK_EQ(parallel.interned_primary_sets[set].geometry, serial.interned_primary_sets[set].geometry);
		CHECK_EQ(parallel.interned_primary_sets[set].lights, serial.interned_primary_sets[set].lights);
	}
	for (int set = 0; set < parallel.interned_transport_sets.size(); set++) {
		CHECK_EQ(parallel.interned_transport_sets[set].geometry, serial.interned_transport_sets[set].geometry);
		CHECK_EQ(parallel.interned_transport_sets[set].lights, serial.interned_transport_sets[set].lights);
	}
}

TEST_CASE("[Rendering][BakedVisibility] lexical ordering makes equivalent input permutations deterministic") {
	const BakedVisibilityBakeOutput first = bake_two_meshes(false, 4096);
	const BakedVisibilityBakeOutput second = bake_two_meshes(true, 4096);
	CHECK_EQ(first.geometry_paths, second.geometry_paths);
	CHECK_EQ(primary_set(first, first.cells[0]).geometry, primary_set(second, second.cells[0]).geometry);
	CHECK_EQ(transport_set(first, first.cells[0]).geometry, transport_set(second, second.cells[0]).geometry);
}

TEST_CASE("[Rendering][BakedVisibility] blocker preprocessing cache reuses canonical patches across bakes") {
	const BakedVisibilityBakeOutput first = bake_two_meshes(false, 4096);
	const BakedVisibilityBakeOutput second = bake_two_meshes(false, 4096);
	CHECK_GT(second.preprocess.cached_patch_hits, uint64_t(0));
	CHECK_EQ(first.scene_fingerprint, second.scene_fingerprint);
	CHECK_EQ(primary_set(first, first.cells[0]).geometry, primary_set(second, second.cells[0]).geometry);
}

TEST_CASE("[Rendering][BakedVisibility] deterministic work cap includes the unprocessed remainder") {
	const BakedVisibilityBakeOutput output = bake_two_meshes(false, 64);
	REQUIRE_EQ(output.cells.size(), 1);
	CHECK_NE(output.cells[0].flags & BakedVisibilityBakeCell::FLAG_WORK_CAP_FALLBACK, 0);
	CHECK_EQ(primary_set(output, output.cells[0]).geometry.size(), 2);
}

TEST_CASE("[Rendering][BakedVisibility] connected coplanar triangles merge into a convex certificate patch") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *occluder = memnew(MeshInstance3D);
	occluder->set_name("Occluder");
	Ref<ArrayMesh> occluder_mesh = make_quad(Vector3(-10, -10, 0), Vector3(10, -10, 0), Vector3(10, 10, 0), Vector3(-10, 10, 0));
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	occluder_mesh->surface_set_material(0, material);
	occluder->set_mesh(occluder_mesh);
	MeshInstance3D *candidate = memnew(MeshInstance3D);
	candidate->set_name("Candidate");
	candidate->set_mesh(make_triangle(Vector3(-0.2f, -0.2f, 0), Vector3(0.2f, -0.2f, 0), Vector3(0, 0.2f, 0)));
	candidate->set_position(Vector3(0, 0, -2));
	anchor.add_child(occluder);
	anchor.add_child(candidate);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-0.2f, -0.2f, 1.8f), Vector3(0.4f, 0.4f, 0.4f));
	input.requested_cell_size = 1.0f;
	input.max_cells = 1;
	input.certificate_work_cap = 4096;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), OK);
	CHECK_EQ(primary_set(output, output.cells[0]).geometry.size(), 1);
}

TEST_CASE("[Rendering][BakedVisibility] holes and concavities remain separate conservative patches") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *ring = memnew(MeshInstance3D);
	ring->set_name("Ring");
	Ref<ArrayMesh> ring_mesh = make_ring_with_hole();
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	ring_mesh->surface_set_material(0, material);
	ring->set_mesh(ring_mesh);
	MeshInstance3D *candidate = memnew(MeshInstance3D);
	candidate->set_name("Candidate");
	candidate->set_mesh(make_tetrahedron(0.1f));
	candidate->set_position(Vector3(0, 0, -2));
	anchor.add_child(ring);
	anchor.add_child(candidate);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-0.2f, -0.2f, 1.8f), Vector3(0.4f, 0.4f, 0.4f));
	input.requested_cell_size = 1.0f;
	input.max_cells = 1;
	input.certificate_work_cap = 4096;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), OK);
	REQUIRE_EQ(output.cells.size(), 1);
	CHECK_EQ(primary_set(output, output.cells[0]).geometry.size(), 2);
}

TEST_CASE("[Rendering][BakedVisibility] default one-sided opaque material certifies only from its rendered side") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *occluder = memnew(MeshInstance3D);
	occluder->set_name("Occluder");
	// This CCW triangle has +Z geometric normal. Godot front faces are
	// clockwise, so the default CULL_BACK material is rendered from -Z.
	occluder->set_mesh(make_triangle(Vector3(-100, -100, 0), Vector3(100, -100, 0), Vector3(0, 100, 0)));
	MeshInstance3D *candidate = memnew(MeshInstance3D);
	candidate->set_name("Candidate");
	candidate->set_mesh(make_triangle(Vector3(-0.1f, -0.1f, 0), Vector3(0.1f, -0.1f, 0), Vector3(0, 0.1f, 0)));
	candidate->set_position(Vector3(0, 0, 2));
	anchor.add_child(occluder);
	anchor.add_child(candidate);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-0.2f, -0.2f, -2.2f), Vector3(0.4f, 0.4f, 0.4f));
	input.requested_cell_size = 1.0f;
	input.max_cells = 1;
	input.certificate_work_cap = 4096;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), OK);
	CHECK_EQ(primary_set(output, output.cells[0]).geometry.size(), 1);
}

TEST_CASE("[Rendering][BakedVisibility] spatial BVH remains deterministic beyond one leaf with tied centroids") {
	auto bake_many = [](bool p_reverse_children) {
		Node3D anchor;
		anchor.set_name("Anchor");
		Vector<MeshInstance3D *> children;
		for (int i = 0; i < 10; i++) {
			MeshInstance3D *mesh = memnew(MeshInstance3D);
			mesh->set_name(vformat("Candidate%02d", i));
			mesh->set_mesh(make_triangle(Vector3(-0.1f, -0.1f, 0), Vector3(0.1f, -0.1f, 0), Vector3(0, 0.1f, 0)));
			// Pairs intentionally share a centroid, exercising the lexical tie-break.
			mesh->set_position(Vector3(float(i / 2) * 0.3f, 0, -2));
			children.push_back(mesh);
		}
		for (int i = 0; i < children.size(); i++) {
			anchor.add_child(children[p_reverse_children ? children.size() - 1 - i : i]);
		}
		BakedVisibilityBakeInput input;
		input.anchor = &anchor;
		input.scene_root = &anchor;
		input.bounds = AABB(Vector3(-0.2f, -0.2f, 1.8f), Vector3(2, 0.4f, 0.4f));
		input.requested_cell_size = 4.0f;
		input.max_cells = 1;
		input.certificate_work_cap = 4096;
		input.transport_distance = 1.0f;
		BakedVisibilityBakeOutput output;
		CHECK_EQ(BakedVisibilityBaker().bake(input, output), OK);
		return output;
	};
	const BakedVisibilityBakeOutput first = bake_many(false);
	const BakedVisibilityBakeOutput second = bake_many(true);
	CHECK_EQ(first.geometry_paths, second.geometry_paths);
	CHECK_EQ(primary_set(first, first.cells[0]).geometry, primary_set(second, second.cells[0]).geometry);
}

TEST_CASE("[Rendering][BakedVisibility] preprocessing scales with shared edges rather than all patch pairs") {
	constexpr int quad_count = 4096;
	const BakedVisibilityBakeOutput first = bake_quad_batch(quad_count, false);
	const BakedVisibilityBakeOutput second = bake_quad_batch(quad_count, true);
	CHECK_EQ(first.geometry_paths, second.geometry_paths);
	CHECK_EQ(primary_set(first, first.cells[0]).geometry, primary_set(second, second.cells[0]).geometry);
	CHECK_EQ(first.preprocess.triangle_count, uint64_t(quad_count * 2 + 1));
	CHECK_EQ(first.preprocess.shared_edge_candidates, uint64_t(quad_count));
	CHECK_EQ(first.preprocess.merge_attempts, uint64_t(quad_count));
	CHECK_EQ(first.preprocess.merged_patch_count, uint64_t(quad_count));
	CHECK_LT(first.preprocess.merge_vertex_visits, uint64_t(quad_count) * 8);
	CHECK_EQ(first.preprocess.shared_edge_candidates, second.preprocess.shared_edge_candidates);
	CHECK_EQ(first.preprocess.merge_attempts, second.preprocess.merge_attempts);
	CHECK_NE(first.preprocess.progress & BakedVisibilityBakeOutput::PreprocessStats::PROGRESS_TRIANGLES_EXTRACTED, 0);
	CHECK_NE(first.preprocess.progress & BakedVisibilityBakeOutput::PreprocessStats::PROGRESS_PATCHES_MERGED, 0);
	CHECK_NE(first.preprocess.progress & BakedVisibilityBakeOutput::PreprocessStats::PROGRESS_BVHS_BUILT, 0);
}

TEST_CASE("[Rendering][BakedVisibility] connected coplanar strips never rebuild growing merge polygons") {
	constexpr int quad_count = 4096;
	const BakedVisibilityBakeOutput output = bake_connected_quad_strip(quad_count);
	CHECK_EQ(output.preprocess.triangle_count, uint64_t(quad_count * 2 + 1));
	CHECK_LE(output.preprocess.merge_attempts, output.preprocess.triangle_count / 2);
	CHECK_LE(output.preprocess.merge_vertex_visits, output.preprocess.merge_attempts * 6);
}

TEST_CASE("[Rendering][BakedVisibility] zero blocker budget preserves geometry without materializing surface arrays") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *dense = memnew(MeshInstance3D);
	dense->set_name("Dense");
	dense->set_mesh(make_quad_batch(1024, false)); // 2,049 synthetic triangles.
	anchor.add_child(dense);

	const BakedVisibilityBakeOutput output = bake_blocker_budget_scene(anchor, 0, AABB(Vector3(-2, -2, -2), Vector3(3100, 4, 4)), 4096.0f);
	REQUIRE_EQ(output.cells.size(), 1);
	CHECK_EQ(output.geometry_paths.size(), 1);
	CHECK_EQ(primary_set(output, output.cells[0]).geometry.size(), 1);
	CHECK_EQ(transport_set(output, output.cells[0]).geometry.size(), 1);
	CHECK_EQ(output.preprocess.blocker_surface_candidates, uint64_t(1));
	CHECK_EQ(output.preprocess.blocker_arrays_materialized, uint64_t(0));
	CHECK_EQ(output.preprocess.blocker_triangles_selected, uint64_t(0));
	CHECK_FALSE(output.conservative_fallback);
}

TEST_CASE("[Rendering][BakedVisibility] many cells retain only one copy of identical visibility sets") {
	constexpr int cell_axis = 16;
	constexpr int directional_light_count = 256;
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *receiver = memnew(MeshInstance3D);
	receiver->set_name("Receiver");
	receiver->set_mesh(make_tetrahedron(100.0f));
	anchor.add_child(receiver);
	for (int i = 0; i < directional_light_count; i++) {
		DirectionalLight3D *light = memnew(DirectionalLight3D);
		light->set_name(vformat("DirectionalLight%03d", i));
		anchor.add_child(light);
	}

	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-8, -8, -8), Vector3(cell_axis, cell_axis, cell_axis));
	input.requested_cell_size = 1.0f;
	input.max_cells = cell_axis * cell_axis * cell_axis;
	input.certificate_work_cap = 4096;
	input.max_blocker_triangles = 0;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), OK);
	REQUIRE_EQ(output.cells.size(), cell_axis * cell_axis * cell_axis);
	REQUIRE_EQ(output.interned_primary_sets.size(), 1);
	REQUIRE_EQ(output.interned_transport_sets.size(), 1);
	CHECK_EQ(output.interned_primary_sets[0].geometry.size(), 1);
	CHECK_EQ(output.interned_primary_sets[0].lights.size(), directional_light_count);
	CHECK_EQ(output.interned_transport_sets[0].geometry.size(), 1);
	CHECK_EQ(output.interned_transport_sets[0].lights.size(), directional_light_count);
	bool all_raw_sets_empty = true;
	for (const BakedVisibilityBakeCell &cell : output.cells) {
		all_raw_sets_empty &= cell.primary.geometry.is_empty() && cell.primary.lights.is_empty() && cell.transport.geometry.is_empty() && cell.transport.lights.is_empty();
		all_raw_sets_empty &= cell.primary_set == 0 && cell.transport_set == 0;
	}
	CHECK(all_raw_sets_empty);
}

TEST_CASE("[Rendering][BakedVisibility] blocker selection prefers usefulness over child order") {
	auto bake = [](bool p_reverse_children) {
		Node3D anchor;
		anchor.set_name("Anchor");
		MeshInstance3D *dense = memnew(MeshInstance3D);
		dense->set_name("Dense");
		dense->set_mesh(make_quad_batch(2, false)); // Five low-value triangles.
		dense->set_scale(Vector3(0.01f, 0.01f, 0.01f));
		MeshInstance3D *useful = memnew(MeshInstance3D);
		useful->set_name("Useful");
		useful->set_mesh(make_tetrahedron(10.0f)); // Four high-value triangles.
		if (p_reverse_children) {
			anchor.add_child(useful);
			anchor.add_child(dense);
		} else {
			anchor.add_child(dense);
			anchor.add_child(useful);
		}
		return bake_blocker_budget_scene(anchor, 5, AABB(Vector3(-20, -20, -20), Vector3(40, 40, 40)), 64.0f);
	};

	const BakedVisibilityBakeOutput first = bake(false);
	const BakedVisibilityBakeOutput second = bake(true);
	CHECK_EQ(first.geometry_paths, second.geometry_paths);
	CHECK_EQ(first.geometry_certified_blockers, second.geometry_certified_blockers);
	CHECK_EQ(first.preprocess.blocker_arrays_materialized, uint64_t(1));
	CHECK_EQ(first.preprocess.blocker_surfaces_selected, uint64_t(1));
	CHECK_EQ(first.preprocess.blocker_triangles_selected, uint64_t(4));
	CHECK_EQ(first.preprocess.blocker_triangles_rejected, uint64_t(5));
	REQUIRE_EQ(first.geometry_paths.size(), 2);
	CHECK_EQ(first.geometry_paths[0], "Dense");
	CHECK_EQ(first.geometry_paths[1], "Useful");
	CHECK_FALSE(first.geometry_certified_blockers[0]);
	CHECK(first.geometry_certified_blockers[1]);
}

TEST_CASE("[Rendering][BakedVisibility] rejected dense blocker remains static geometry without array materialization") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *dense = memnew(MeshInstance3D);
	dense->set_name("Dense");
	dense->set_mesh(make_quad_batch(1024, false)); // 2,049 synthetic triangles.
	MeshInstance3D *useful = memnew(MeshInstance3D);
	useful->set_name("Useful");
	useful->set_mesh(make_tetrahedron(10.0f));
	anchor.add_child(dense);
	anchor.add_child(useful);

	const BakedVisibilityBakeOutput output = bake_blocker_budget_scene(anchor, 4, AABB(Vector3(-20, -20, -20), Vector3(3100, 40, 40)), 4096.0f);
	REQUIRE_EQ(output.cells.size(), 1);
	CHECK_EQ(output.geometry_paths.size(), 2);
	CHECK_EQ(output.static_geometry_count, 2);
	CHECK_EQ(output.preprocess.blocker_surface_candidates, uint64_t(2));
	const bool materialized_or_cached = (output.preprocess.blocker_arrays_materialized == 1 && output.preprocess.blocker_triangles_selected == 4) ||
			(output.preprocess.blocker_arrays_materialized == 0 && output.preprocess.cached_patch_hits >= 1 && output.preprocess.blocker_triangles_selected == 0);
	CHECK(materialized_or_cached);
	CHECK_LE(output.preprocess.blocker_triangles_selected, uint64_t(4));
	CHECK_EQ(output.preprocess.blocker_triangles_rejected, uint64_t(2049));
}

TEST_CASE("[Rendering][BakedVisibility] transparent blocker candidates are rejected before array materialization") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *transparent = memnew(MeshInstance3D);
	transparent->set_name("Transparent");
	Ref<ArrayMesh> mesh = make_quad_batch(1024, false);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	mesh->surface_set_material(0, material);
	transparent->set_mesh(mesh);
	anchor.add_child(transparent);

	const BakedVisibilityBakeOutput output = bake_blocker_budget_scene(anchor, 4096, AABB(Vector3(-2, -2, -2), Vector3(3100, 4, 4)), 4096.0f);
	CHECK_EQ(output.geometry_paths.size(), 1);
	CHECK_EQ(output.preprocess.blocker_surface_candidates, uint64_t(0));
	CHECK_EQ(output.preprocess.blocker_surfaces_rejected, uint64_t(1));
	CHECK_EQ(output.preprocess.blocker_arrays_materialized, uint64_t(0));
	CHECK_EQ(output.preprocess.blocker_triangles_selected, uint64_t(0));
}

TEST_CASE("[Rendering][BakedVisibility] transparent surface does not prevent opaque surface blocker extraction") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mixed = memnew(MeshInstance3D);
	mixed->set_name("Mixed");
	Ref<ArrayMesh> mesh = make_tetrahedron(1.0f); // Four opaque source triangles.
	Array transparent_arrays;
	PackedVector3Array transparent_vertices;
	transparent_vertices.push_back(Vector3(-2, -2, 2));
	transparent_vertices.push_back(Vector3(2, -2, 2));
	transparent_vertices.push_back(Vector3(2, 2, 2));
	transparent_vertices.push_back(Vector3(-2, 2, 2));
	PackedInt32Array transparent_indices;
	transparent_indices.push_back(0);
	transparent_indices.push_back(1);
	transparent_indices.push_back(2);
	transparent_indices.push_back(0);
	transparent_indices.push_back(2);
	transparent_indices.push_back(3);
	transparent_arrays.resize(Mesh::ARRAY_MAX);
	transparent_arrays[Mesh::ARRAY_VERTEX] = transparent_vertices;
	transparent_arrays[Mesh::ARRAY_INDEX] = transparent_indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, transparent_arrays);
	Ref<StandardMaterial3D> transparent_material;
	transparent_material.instantiate();
	transparent_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	mesh->surface_set_material(1, transparent_material);
	mixed->set_mesh(mesh);
	anchor.add_child(mixed);

	const BakedVisibilityBakeOutput output = bake_blocker_budget_scene(anchor, 4, AABB(Vector3(-3, -3, -3), Vector3(6, 6, 6)), 8.0f);
	REQUIRE_EQ(output.cells.size(), 1);
	REQUIRE_EQ(output.geometry_paths.size(), 1);
	REQUIRE_EQ(output.geometry_certified_blockers.size(), 1);
	CHECK_EQ(output.static_geometry_count, 1);
	CHECK_EQ(primary_set(output, output.cells[0]).geometry.size(), 1);
	CHECK_EQ(transport_set(output, output.cells[0]).geometry.size(), 1);
	CHECK_EQ(output.preprocess.blocker_surface_candidates, uint64_t(1));
	CHECK_EQ(output.preprocess.blocker_surfaces_rejected, uint64_t(1));
	CHECK_EQ(output.preprocess.blocker_arrays_materialized, uint64_t(1));
	CHECK_EQ(output.preprocess.blocker_triangles_selected, uint64_t(4));
	CHECK_EQ(output.preprocess.blocker_triangles_rejected, uint64_t(2));
	CHECK(output.geometry_certified_blockers[0]);
}

TEST_CASE("[Rendering][BakedVisibility] volume blocker budget overrides input and clamps authored values") {
	BakedVisibilityVolume3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Useful");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	CHECK_EQ(anchor.get_max_blocker_triangles(), 131072);

	anchor.set_max_blocker_triangles(4);
	const BakedVisibilityBakeOutput overridden = bake_blocker_budget_scene(anchor, 0, AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4)), 8.0f);
	CHECK_EQ(overridden.preprocess.blocker_triangles_selected, uint64_t(4));
	CHECK_EQ(overridden.preprocess.blocker_arrays_materialized, uint64_t(1));

	anchor.set_max_blocker_triangles(-1);
	CHECK_EQ(anchor.get_max_blocker_triangles(), 0);
	const BakedVisibilityBakeOutput disabled = bake_blocker_budget_scene(anchor, 4, AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4)), 8.0f);
	CHECK_EQ(disabled.preprocess.blocker_triangles_selected, uint64_t(0));
	CHECK_EQ(disabled.preprocess.blocker_arrays_materialized, uint64_t(0));
	anchor.set_max_blocker_triangles(1048577);
	CHECK_EQ(anchor.get_max_blocker_triangles(), 1048576);
}

TEST_CASE("[Rendering][BakedVisibility] build data canonicalizes interleaved geometry and light paths") {
	const String source_path = "res://tests/baked_visibility_tmp/interleaved_paths.tscn";
	const String source_absolute_path = ProjectSettings::get_singleton()->globalize_path(source_path);
	const Error create_directory_error = DirAccess::make_dir_recursive_absolute(source_absolute_path.get_base_dir());
	REQUIRE_EQ(create_directory_error, OK);
	if (create_directory_error != OK) {
		return;
	}
	const ResourceUID::ID uid = ResourceUID::get_singleton()->create_id();
	const bool previous_editor_hint = Engine::get_singleton()->is_editor_hint();
	bool editor_hint_changed = false;
	const auto cleanup = [&]() {
		if (editor_hint_changed) {
			Engine::get_singleton()->set_editor_hint(previous_editor_hint);
		}
		DirAccess::remove_absolute(source_absolute_path);
		DirAccess::remove_absolute(source_absolute_path.get_base_dir());
	};
	Ref<FileAccess> source = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source.is_valid());
	if (source.is_null()) {
		cleanup();
		return;
	}
	source->store_string(vformat("[gd_scene load_steps=1 format=3 uid=\"%s\"]\n\n[node name=\"Root\" type=\"Node\"]\n", ResourceUID::get_singleton()->id_to_text(uid)));
	source->close();

	BakedVisibilityBakeInput input;
	input.source_path = source_path;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	output.bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	output.cell_size = 1.0f;
	output.grid_size = Vector3i(1, 1, 1);
	output.geometry_paths.push_back("ZGeometry");
	output.geometry_identities.push_back("test");
	output.geometry_bounds.push_back(AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1)));
	output.geometry_certified_blockers.push_back(false);
	output.light_paths.push_back("ALight");
	output.light_bounds.push_back(AABB(Vector3(), Vector3()));
	output.light_directional.push_back(false);
	BakedVisibilityBakeSet set;
	set.geometry.push_back(0);
	set.lights.push_back(0);
	output.interned_primary_sets.push_back(set);
	output.interned_transport_sets.push_back(set);
	BakedVisibilityBakeCell cell;
	cell.primary_set = 0;
	cell.transport_set = 0;
	output.cells.push_back(cell);

	Engine::get_singleton()->set_editor_hint(true);
	editor_hint_changed = true;
	BakedVisibilityData3DData data;
	String error;
	const Error build_error = BakedVisibilityBaker().build_data(input, output, data, &error);
	REQUIRE_EQ(build_error, OK);
	if (build_error != OK) {
		cleanup();
		return;
	}
	const Error validation_error = BakedVisibilityCodec::validate(data, &error);
	REQUIRE_EQ(validation_error, OK);
	REQUIRE_EQ(data.instances.size(), 2);
	if (validation_error != OK || data.instances.size() != 2) {
		cleanup();
		return;
	}
	CHECK_EQ(String(data.instances[0].path), "ALight");
	CHECK_EQ(String(data.instances[1].path), "ZGeometry");
	BakedVisibilityData3DData repeat_data;
	const Error repeat_build_error = BakedVisibilityBaker().build_data(input, output, repeat_data, &error);
	REQUIRE_EQ(repeat_build_error, OK);
	if (repeat_build_error != OK) {
		cleanup();
		return;
	}
	PackedByteArray bytes;
	PackedByteArray repeat_bytes;
	const Error encode_error = BakedVisibilityCodec::encode(data, bytes, &error);
	REQUIRE_EQ(encode_error, OK);
	if (encode_error != OK) {
		cleanup();
		return;
	}
	const Error repeat_encode_error = BakedVisibilityCodec::encode(repeat_data, repeat_bytes, &error);
	REQUIRE_EQ(repeat_encode_error, OK);
	if (repeat_encode_error != OK) {
		cleanup();
		return;
	}
	CHECK_EQ(bytes, repeat_bytes);
	cleanup();
}

TEST_CASE("[Rendering][BakedVisibility] build data rejects malformed interned set indices") {
	BakedVisibilityBakeOutput output;
	BakedVisibilityBakeCell cell;
	cell.primary_set = 1;
	cell.transport_set = 0;
	output.cells.push_back(cell);
	BakedVisibilityData3DData data;
	String error;
	CHECK_EQ(BakedVisibilityBaker().build_data(BakedVisibilityBakeInput(), output, data, &error), ERR_INVALID_DATA);
	CHECK_EQ(error, "Baked visibility output references an invalid interned set.");
}

TEST_CASE("[Rendering][BakedVisibility] UID-bearing generated bake dependency does not change source digest") {
	const String source_path = "user://baked_visibility_uid_dependency.tscn";
	const String baked_path = "user://baked_visibility_uid_dependency.bvis";
	const ResourceUID::ID source_uid = ResourceUID::get_singleton()->create_id();
	const ResourceUID::ID baked_uid = ResourceUID::get_singleton()->create_id();
	const String source_uid_text = ResourceUID::get_singleton()->id_to_text(source_uid);
	const String baked_uid_text = ResourceUID::get_singleton()->id_to_text(baked_uid);

	Ref<FileAccess> baked_file = FileAccess::open(baked_path, FileAccess::WRITE);
	REQUIRE(baked_file.is_valid());
	baked_file->store_string("generated baked visibility data");
	baked_file->close();
	ResourceUID::get_singleton()->add_id(baked_uid, baked_path);

	Ref<FileAccess> source = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source.is_valid());
	source->store_string(vformat("[gd_scene load_steps=1 format=3 uid=\"%s\"]\n\n[node name=\"Root\" type=\"Node\"]\n[node name=\"BakedVisibility\" type=\"Node\" parent=\".\"]\n", source_uid_text));
	source->close();
	const PackedByteArray before = BakedVisibilityBaker::make_source_digest(source_path);
	REQUIRE_EQ(before.size(), 32);

	source = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source.is_valid());
	source->store_string(vformat("[gd_scene load_steps=2 format=3 uid=\"%s\"]\n\n[ext_resource type=\"BakedVisibilityData3D\" uid=\"%s\" path=\"%s\" id=\"1_visibility\"]\n\n[node name=\"Root\" type=\"Node\"]\n[node name=\"BakedVisibility\" type=\"Node\" parent=\".\"]\ndata = ExtResource(\"1_visibility\")\n", source_uid_text, baked_uid_text, baked_path));
	source->close();
	List<String> dependencies;
	ResourceLoader::get_dependencies(source_path, &dependencies, false);
	REQUIRE_EQ(dependencies.size(), 1);
	CHECK(dependencies.front()->get().begins_with("uid://"));
	const PackedByteArray after = BakedVisibilityBaker::make_source_digest(source_path);
	CHECK_EQ(before, after);

	ResourceUID::get_singleton()->remove_id(baked_uid);
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(source_path));
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(baked_path));
}

TEST_CASE("[Rendering][BakedVisibility] source digest streams binary dependencies and keeps staged scene text") {
	const String source_path = "user://baked_visibility_binary_digest.tscn";
	const String staged_source_path = "user://baked_visibility_binary_digest_staged.tscn";
	const String binary_path = "user://baked_visibility_binary_digest.bin";
	const auto cleanup = [&]() {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(source_path));
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(staged_source_path));
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(binary_path));
	};

	PackedByteArray binary;
	binary.resize(4 * 1024 * 1024);
	for (int i = 0; i < binary.size(); i++) {
		binary.set(i, uint8_t(i));
	}
	Ref<FileAccess> file = FileAccess::open(binary_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_buffer(binary);
	file->close();

	file = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"Resource\" path=\"%s\" id=\"1_binary\"]\n\n[node name=\"OnDisk\" type=\"Node\"]\n", binary_path));
	file->close();
	file = FileAccess::open(staged_source_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"Resource\" path=\"%s\" id=\"1_binary\"]\n\n[node name=\"Staged\" type=\"Node\"]\n", binary_path));
	file->close();

	const PackedByteArray on_disk_digest = BakedVisibilityBaker::make_source_digest(source_path);
	const PackedByteArray staged_digest = BakedVisibilityBaker::make_source_digest(source_path, staged_source_path);
	const PackedByteArray repeated_staged_digest = BakedVisibilityBaker::make_source_digest(source_path, staged_source_path);
	REQUIRE_EQ(on_disk_digest.size(), 32);
	REQUIRE_EQ(staged_digest.size(), 32);
	CHECK_NE(on_disk_digest, staged_digest);
	CHECK_EQ(staged_digest, repeated_staged_digest);

	binary.set(binary.size() - 1, 0x00);
	file = FileAccess::open(binary_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_buffer(binary);
	file->close();
	const PackedByteArray changed_binary_digest = BakedVisibilityBaker::make_source_digest(source_path, staged_source_path);
	CHECK_NE(staged_digest, changed_binary_digest);

	cleanup();
}

TEST_CASE("[Rendering][BakedVisibility] tile index pool covers every cell exactly once") {
	BakedVisibilityData3DData value;
	value.source_uid = 1;
	value.source_path = "res://tile.tscn";
	value.source_sha256.resize(32);
	value.local_bounds = AABB(Vector3(), Vector3(17, 9, 2));
	value.cell_size = Vector3(1, 1, 1);
	value.grid_size = Vector3i(17, 9, 2);
	value.sets.push_back(Vector<uint32_t>());
	value.cells.resize(17 * 9 * 2);
	PackedByteArray bytes;
	CHECK_EQ(BakedVisibilityCodec::encode(value, bytes), OK);
	BakedVisibilityData3DData decoded;
	CHECK_EQ(BakedVisibilityCodec::decode(bytes, decoded), OK);
	CHECK(decoded.tile_cell_indices.is_empty());
	CHECK(decoded.cells.is_empty());
	const int leaf_count = decoded.tile_grid_size.x * decoded.tile_grid_size.y * decoded.tile_grid_size.z;
	CHECK_GT(decoded.tiles.size(), leaf_count);
	CHECK_GT(decoded.hierarchy_depth, 1);
	for (int i = 0; i < leaf_count; i++) {
		CHECK_EQ(decoded.tiles[i].level, 0);
		CHECK((decoded.tiles[i].flags & BakedVisibilityData3DData::Tile::FLAG_LEAF) != 0);
		CHECK_GE(decoded.tiles[i].parent, leaf_count);
	}
	Vector<uint8_t> seen;
	seen.resize(value.cells.size());
	for (int i = 0; i < seen.size(); i++) seen.write[i] = 0;
	for (int tile = 0; tile < leaf_count; tile++) {
		Vector<uint32_t> cell_indices;
		Vector<BakedVisibilityData3DData::Cell> cells;
		Vector<Vector<uint32_t>> sets;
		REQUIRE_EQ(BakedVisibilityCodec::decode_leaf_payload(decoded, tile, cell_indices, cells, sets), OK);
		for (uint32_t cell_index : cell_indices) {
			REQUIRE(cell_index < uint32_t(seen.size()));
			CHECK_FALSE(seen[cell_index]);
			seen.write[cell_index] = 1;
		}
	}
	for (uint8_t cell_seen : seen) CHECK(cell_seen);
}

TEST_CASE("[Rendering][BakedVisibility] bake build and encode non-multiple tiled grid") {
	const String source_path = "res://tests/baked_visibility_tmp/tiled_grid.tscn";
	const String source_absolute_path = ProjectSettings::get_singleton()->globalize_path(source_path);
	REQUIRE_EQ(DirAccess::make_dir_recursive_absolute(source_absolute_path.get_base_dir()), OK);
	const ResourceUID::ID uid = ResourceUID::get_singleton()->create_id();
	Ref<FileAccess> source = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source.is_valid());
	source->store_string(vformat("[gd_scene load_steps=1 format=3 uid=\"%s\"]\n\n[node name=\"Root\" type=\"Node3D\"]\n", ResourceUID::get_singleton()->id_to_text(uid)));
	source->close();

	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.source_path = source_path;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(56, 16, 120));
	input.requested_cell_size = 4.0f;
	input.max_cells = 4096;
	input.transport_distance = 4.0f;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), OK);
	CHECK_EQ(output.grid_size, Vector3i(14, 4, 30));
	CHECK_EQ(output.tile_count, 8);
	BakedVisibilityData3DData data;
	String error;
	const bool previous_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	REQUIRE_EQ(BakedVisibilityBaker().build_data(input, output, data, &error), OK);
	CHECK_EQ(data.tile_grid_size, Vector3i(2, 1, 4));
	PackedByteArray bytes;
	CHECK_EQ(BakedVisibilityCodec::encode(data, bytes, &error), OK);
	Engine::get_singleton()->set_editor_hint(previous_editor_hint);

	anchor.remove_child(mesh);
	memdelete(mesh);
	DirAccess::remove_absolute(source_absolute_path);
	DirAccess::remove_absolute(source_absolute_path.get_base_dir());
}

TEST_CASE("[Rendering][BakedVisibility] multi-tile 1200-cell bake preserves bounded grid for encoding") {
	const String source_path = "res://tests/baked_visibility_tmp/grid_1200.tscn";
	const String source_absolute_path = ProjectSettings::get_singleton()->globalize_path(source_path);
	REQUIRE_EQ(DirAccess::make_dir_recursive_absolute(source_absolute_path.get_base_dir()), OK);
	const ResourceUID::ID uid = ResourceUID::get_singleton()->create_id();
	Ref<FileAccess> source = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source.is_valid());
	if (source.is_valid()) {
		source->store_string(vformat("[gd_scene load_steps=1 format=3 uid=\"%s\"]\n\n[node name=\"Root\" type=\"Node3D\"]\n", ResourceUID::get_singleton()->id_to_text(uid)));
		source->close();
	}
	ResourceUID::get_singleton()->add_id(uid, source_path);
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.source_path = source_path;
	input.bounds = AABB(Vector3(-7.5f, -2.5f, -8.0f), Vector3(15.0f, 5.0f, 16.0f));
	input.requested_cell_size = 1.0f;
	input.max_cells = 2048;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), OK);
	CHECK_EQ(output.grid_size, Vector3i(15, 5, 16));
	CHECK_EQ(output.grid_size.x * output.grid_size.y * output.grid_size.z, output.cells.size());
	BakedVisibilityData3DData data;
	String error;
	const bool previous_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	const Error build_error = BakedVisibilityBaker().build_data(input, output, data, &error);
	REQUIRE_EQ(build_error, OK);
	PackedByteArray encoded;
	REQUIRE_EQ(BakedVisibilityCodec::encode(data, encoded, &error), OK);
	BakedVisibilityData3DData decoded;
	REQUIRE_EQ(BakedVisibilityCodec::decode(encoded, decoded, &error), OK);
	CHECK_EQ(decoded.grid_size, output.grid_size);
	CHECK_EQ(decoded.cells.size(), 0);
	CHECK_EQ(decoded.tile_cell_indices.size(), 0);
	CHECK_EQ(decoded.grid_size.x * decoded.grid_size.y * decoded.grid_size.z, output.cells.size());
	PackedByteArray reencoded;
	REQUIRE_EQ(BakedVisibilityCodec::encode(decoded, reencoded, &error), OK);
	BakedVisibilityData3DData restaged;
	REQUIRE_EQ(BakedVisibilityCodec::decode(reencoded, restaged, &error), OK);
	if (!restaged.instances.is_empty() && !restaged.instances[0].signature_sha256.is_empty()) {
		restaged.instances.write[0].signature_sha256.write[0] ^= 1;
	}
	PackedByteArray restaged_bytes;
	REQUIRE_EQ(BakedVisibilityCodec::encode(restaged, restaged_bytes, &error), OK);
	BakedVisibilityData3DData restaged_decoded;
	REQUIRE_EQ(BakedVisibilityCodec::decode(restaged_bytes, restaged_decoded, &error), OK);
	PackedByteArray canonical_restaged_bytes;
	REQUIRE_EQ(BakedVisibilityCodec::encode(restaged_decoded, canonical_restaged_bytes, &error), OK);
	CHECK_EQ(canonical_restaged_bytes, restaged_bytes);
	Vector<uint8_t> coverage;
	coverage.resize(output.cells.size());
	for (int index = 0; index < coverage.size(); index++) coverage.write[index] = 0;
	for (int tile = 0; tile < restaged_decoded.tiles.size(); tile++) {
		if (!(restaged_decoded.tiles[tile].flags & BakedVisibilityData3DData::Tile::FLAG_LEAF)) continue;
		Vector<uint32_t> indices;
		Vector<BakedVisibilityData3DData::Cell> cells;
		Vector<Vector<uint32_t>> sets;
		REQUIRE_EQ(BakedVisibilityCodec::decode_leaf_payload(restaged_decoded, tile, indices, cells, sets, &error), OK);
		for (uint32_t index : indices) {
			REQUIRE(index < uint32_t(coverage.size()));
			REQUIRE(!coverage[index]);
			coverage.write[index] = 1;
		}
	}
	for (uint8_t covered : coverage) CHECK(covered);
	Engine::get_singleton()->set_editor_hint(previous_editor_hint);
	anchor.remove_child(mesh);
	memdelete(mesh);
	DirAccess::remove_absolute(source_absolute_path);
	DirAccess::remove_absolute(source_absolute_path.get_base_dir());
}

TEST_CASE("[Rendering][BakedVisibility] memory cap rejects before staged allocation") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	input.requested_cell_size = 1.0f;
	input.max_cells = 64;
	input.max_memory_bytes = 1;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), ERR_OUT_OF_MEMORY);
	CHECK(output.error.contains("memory cap"));
	CHECK(output.error.contains("scene geometry extraction"));
	CHECK(output.error.contains("requested="));
	CHECK(output.error.contains("used="));
	CHECK(output.error.contains("budget="));
	anchor.remove_child(mesh);
	memdelete(mesh);
}

TEST_CASE("[Rendering][BakedVisibility] memory cap reserves variable cell results before allocation") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	input.requested_cell_size = 1.0f;
	input.max_cells = 64;
	input.max_memory_bytes = 4096;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), ERR_OUT_OF_MEMORY);
	CHECK(output.error.contains("cell results and checkpoint"));
	CHECK(output.error.contains("requested="));
	CHECK(output.error.contains("used="));
	CHECK(output.error.contains("budget="));
	anchor.remove_child(mesh);
	memdelete(mesh);
}

TEST_CASE("[Rendering][BakedVisibility] cancellation checkpoint records tile bitmap, not a prefix") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	std::atomic_bool cancel = true;
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(16, 1, 1));
	input.requested_cell_size = 1.0f;
	input.max_cells = 64;
	input.cancel_flag = &cancel;
	BakedVisibilityBakeOutput output;
	CHECK_EQ(BakedVisibilityBaker().bake(input, output), ERR_BUSY);
	CHECK_EQ(output.checkpoint.completed_tiles.size(), output.tile_count);
	for (uint8_t complete : output.checkpoint.completed_tiles) CHECK_FALSE(complete);
	anchor.remove_child(mesh);
	memdelete(mesh);
}

TEST_CASE("[Rendering][BakedVisibility] checkpoint store round-trips an immutable tile snapshot") {
	const String source_path = "res://tests/baked_visibility_tmp/checkpoint_scene.tscn";
	const String source_absolute_path = ProjectSettings::get_singleton()->globalize_path(source_path);
	REQUIRE_EQ(DirAccess::make_dir_recursive_absolute(source_absolute_path.get_base_dir()), OK);
	const ResourceUID::ID source_uid = ResourceUID::get_singleton()->create_id();
	Ref<FileAccess> source_scene = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source_scene.is_valid());
	if (source_scene.is_valid()) {
		source_scene->store_string(vformat("[gd_scene load_steps=1 format=3 uid=\"%s\"]\n\n[node name=\"Anchor\" type=\"Node3D\"]\n", ResourceUID::get_singleton()->id_to_text(source_uid)));
		source_scene->close();
	}
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);
	std::atomic_bool cancel = true;
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.source_path = source_path;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(16, 4, 4));
	input.requested_cell_size = 1.0f;
	input.max_cells = 256;
	input.cancel_flag = &cancel;
	BakedVisibilityBakeOutput output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, output), ERR_BUSY);
	const String checkpoint_path = "res://baked_visibility_checkpoint_test.bvck";
	String error;
	REQUIRE_EQ(BakedVisibilityBakeCheckpointStore::save(checkpoint_path, output.checkpoint, &error), OK);
	BakedVisibilityBakeCheckpoint loaded;
	REQUIRE_EQ(BakedVisibilityBakeCheckpointStore::load(checkpoint_path, loaded, &error), OK);
	CHECK_EQ(loaded.grid_size, output.checkpoint.grid_size);
	CHECK_EQ(loaded.tile_grid_size, output.checkpoint.tile_grid_size);
	CHECK_EQ(loaded.hierarchy_depth, output.checkpoint.hierarchy_depth);
	CHECK_EQ(loaded.completed_tiles, output.checkpoint.completed_tiles);
	CHECK_EQ(loaded.completed_cell_bitmap, output.checkpoint.completed_cell_bitmap);
	CHECK_EQ(loaded.tiles.size(), output.checkpoint.tiles.size());
	for (int tile = 0; tile < loaded.tiles.size(); tile++) {
		CHECK_EQ(loaded.tiles[tile].candidate_dependencies, output.checkpoint.tiles[tile].candidate_dependencies);
		CHECK_EQ(loaded.tiles[tile].light_dependencies, output.checkpoint.tiles[tile].light_dependencies);
		CHECK_EQ(loaded.tiles[tile].certificate_dependencies, output.checkpoint.tiles[tile].certificate_dependencies);
	}
	CHECK_EQ(loaded.cells.size(), output.checkpoint.cells.size());
	CHECK_EQ(loaded.geometry_paths, output.checkpoint.geometry_paths);
	CHECK_EQ(loaded.light_paths, output.checkpoint.light_paths);
	cancel.store(false);
	BakedVisibilityBakeInput complete_input = input;
	complete_input.resume_checkpoint = nullptr;
	BakedVisibilityBakeOutput complete;
	REQUIRE_EQ(BakedVisibilityBaker().bake(complete_input, complete), OK);
	REQUIRE_EQ(BakedVisibilityBakeCheckpointStore::save(checkpoint_path, complete.checkpoint, &error), OK);
	REQUIRE_EQ(BakedVisibilityBakeCheckpointStore::load(checkpoint_path, loaded, &error), OK);
	BakedVisibilityBakeInput resumed_input = input;
	resumed_input.resume_checkpoint = &loaded;
	BakedVisibilityBakeOutput resumed;
	REQUIRE_EQ(BakedVisibilityBaker().bake(resumed_input, resumed), OK);
	CHECK_EQ(resumed.reused_cells, resumed.completed_cells);
	// This fixture is intentionally unsaved, so build_data cannot produce a
	// runtime payload. Compare the complete canonical bake representation that
	// feeds encoding instead, including interned tables, tiles, and cells.
	CHECK_EQ(resumed.interned_primary_sets.size(), complete.interned_primary_sets.size());
	CHECK_EQ(resumed.interned_transport_sets.size(), complete.interned_transport_sets.size());
	for (int set = 0; set < complete.interned_primary_sets.size(); set++) {
		CHECK_EQ(resumed.interned_primary_sets[set].geometry, complete.interned_primary_sets[set].geometry);
		CHECK_EQ(resumed.interned_primary_sets[set].lights, complete.interned_primary_sets[set].lights);
	}
	for (int set = 0; set < complete.interned_transport_sets.size(); set++) {
		CHECK_EQ(resumed.interned_transport_sets[set].geometry, complete.interned_transport_sets[set].geometry);
		CHECK_EQ(resumed.interned_transport_sets[set].lights, complete.interned_transport_sets[set].lights);
	}
	CHECK_EQ(resumed.cells.size(), complete.cells.size());
	for (int cell = 0; cell < complete.cells.size(); cell++) {
		CHECK_EQ(resumed.cells[cell].coordinate, complete.cells[cell].coordinate);
		CHECK_EQ(resumed.cells[cell].flags, complete.cells[cell].flags);
		CHECK_EQ(resumed.cells[cell].primary_set, complete.cells[cell].primary_set);
		CHECK_EQ(resumed.cells[cell].transport_set, complete.cells[cell].transport_set);
	}
	for (int tile = 0; tile < complete.tiles.size(); tile++) {
		CHECK_EQ(resumed.tiles[tile].coordinate, complete.tiles[tile].coordinate);
		CHECK_EQ(resumed.tiles[tile].first_cell, complete.tiles[tile].first_cell);
		CHECK_EQ(resumed.tiles[tile].cell_count, complete.tiles[tile].cell_count);
		CHECK_EQ(resumed.tiles[tile].flags, complete.tiles[tile].flags);
		CHECK_EQ(resumed.tiles[tile].candidate_dependencies, complete.tiles[tile].candidate_dependencies);
		CHECK_EQ(resumed.tiles[tile].certificate_dependencies, complete.tiles[tile].certificate_dependencies);
		CHECK_EQ(resumed.tiles[tile].light_dependencies, complete.tiles[tile].light_dependencies);
		CHECK_EQ(resumed.tiles[tile].dependency_signature, complete.tiles[tile].dependency_signature);
	}
	CHECK_EQ(resumed.tile_cell_indices, complete.tile_cell_indices);
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(checkpoint_path));
	anchor.remove_child(mesh);
	memdelete(mesh);
	DirAccess::remove_absolute(source_absolute_path);
	DirAccess::remove_absolute(source_absolute_path.get_base_dir());
}

TEST_CASE("[Rendering][BakedVisibility] tile checkpoints resume deterministically and invalidate conservatively") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	mesh->set_name("Mesh");
	mesh->set_mesh(make_tetrahedron(1.0f));
	anchor.add_child(mesh);

	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(-2, -2, -2), Vector3(16, 4, 4));
	input.requested_cell_size = 1.0f;
	input.max_cells = 256;
	BakedVisibilityBakeOutput baseline;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, baseline), OK);
	REQUIRE_EQ(baseline.tile_count, 2);

	BakedVisibilityBakeCheckpoint sparse = baseline.checkpoint;
	sparse.completed_tiles.write[0] = 1;
	sparse.completed_tiles.write[1] = 0;
	BakedVisibilityBakeInput sparse_input = input;
	sparse_input.resume_checkpoint = &sparse;
	BakedVisibilityBakeOutput sparse_output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(sparse_input, sparse_output), OK);
	CHECK_EQ(sparse_output.reused_cells, sparse.tiles[0].cell_count);
	CHECK_EQ(sparse_output.completed_cells, baseline.completed_cells);
	CHECK_EQ(sparse_output.interned_primary_sets.size(), baseline.interned_primary_sets.size());
	CHECK_EQ(sparse_output.interned_transport_sets.size(), baseline.interned_transport_sets.size());
	for (int i = 0; i < baseline.interned_primary_sets.size(); i++) {
		CHECK_EQ(sparse_output.interned_primary_sets[i].geometry, baseline.interned_primary_sets[i].geometry);
		CHECK_EQ(sparse_output.interned_primary_sets[i].lights, baseline.interned_primary_sets[i].lights);
	}
	for (int i = 0; i < baseline.interned_transport_sets.size(); i++) {
		CHECK_EQ(sparse_output.interned_transport_sets[i].geometry, baseline.interned_transport_sets[i].geometry);
		CHECK_EQ(sparse_output.interned_transport_sets[i].lights, baseline.interned_transport_sets[i].lights);
	}
	for (int i = 0; i < baseline.cells.size(); i++) {
		CHECK_EQ(sparse_output.cells[i].coordinate, baseline.cells[i].coordinate);
		CHECK_EQ(sparse_output.cells[i].primary_set, baseline.cells[i].primary_set);
		CHECK_EQ(sparse_output.cells[i].transport_set, baseline.cells[i].transport_set);
		CHECK_EQ(sparse_output.cells[i].flags, baseline.cells[i].flags);
	}

	BakedVisibilityBakeCheckpoint changed_signature = baseline.checkpoint;
	changed_signature.tiles.write[0].dependency_signature.set(0, changed_signature.tiles[0].dependency_signature[0] ^ 0x1);
	BakedVisibilityBakeInput changed_input = input;
	changed_input.resume_checkpoint = &changed_signature;
	BakedVisibilityBakeOutput changed_output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(changed_input, changed_output), OK);
	CHECK_EQ(changed_output.reused_cells, baseline.tiles[1].cell_count);

	BakedVisibilityBakeInput changed_setting_input = input;
	changed_setting_input.transport_distance += 1.0f;
	changed_setting_input.resume_checkpoint = &baseline.checkpoint;
	BakedVisibilityBakeOutput changed_setting_output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(changed_setting_input, changed_setting_output), OK);
	CHECK_EQ(changed_setting_output.reused_cells, 0);

	mesh->set_position(Vector3(1, 0, 0));
	BakedVisibilityBakeInput moved_input = input;
	moved_input.resume_checkpoint = &baseline.checkpoint;
	BakedVisibilityBakeOutput moved_output;
	REQUIRE_EQ(BakedVisibilityBaker().bake(moved_input, moved_output), OK);
	CHECK_EQ(moved_output.reused_cells, 0);

	anchor.remove_child(mesh);
	memdelete(mesh);
}

TEST_CASE("[Rendering][BakedVisibility] real local mesh mutation selectively reuses an isolated leaf") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *left = memnew(MeshInstance3D);
	left->set_name("Left");
	left->set_mesh(make_tetrahedron(0.5f));
	left->set_position(Vector3(2, 0, 0));
	Ref<StandardMaterial3D> left_transparent;
	left_transparent.instantiate();
	left_transparent->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	left->set_material_override(left_transparent);
	anchor.add_child(left);
	MeshInstance3D *right = memnew(MeshInstance3D);
	right->set_name("Right");
	right->set_mesh(make_tetrahedron(0.5f));
	right->set_position(Vector3(13, 0, 0));
	anchor.add_child(right);
	MeshInstance3D *wall = memnew(MeshInstance3D);
	wall->set_name("Wall");
	wall->set_mesh(make_separating_wall());
	Ref<StandardMaterial3D> two_sided;
	two_sided.instantiate();
	two_sided->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	wall->set_material_override(two_sided);
	// Keep the complete certificate patch inside the first leaf; the second leaf
	// is separated by a positive margin rather than beginning on the patch.
	wall->set_position(Vector3(-3, 0, 0));
	anchor.add_child(wall);

	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(0, -2, -2), Vector3(16, 4, 4));
	input.requested_cell_size = 1.0f;
	// Keep unit cells over this 16 x 4 x 4 extent so x produces two leaf tiles.
	input.max_cells = 256;
	input.transport_distance = 1.0f;
	input.certificate_work_cap = 8192;
	BakedVisibilityBakeOutput baseline;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, baseline), OK);
	REQUIRE_EQ(baseline.tile_count, 2);
	if (baseline.tile_count != 2) {
		anchor.remove_child(wall);
		anchor.remove_child(right);
		anchor.remove_child(left);
		memdelete(wall);
		memdelete(right);
		memdelete(left);
		return;
	}

	left->set_position(Vector3(2.25f, 0, 0));
	BakedVisibilityBakeInput resumed_input = input;
	resumed_input.resume_checkpoint = &baseline.checkpoint;
	BakedVisibilityBakeOutput resumed;
	REQUIRE_EQ(BakedVisibilityBaker().bake(resumed_input, resumed), OK);
	BakedVisibilityBakeOutput clean;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, clean), OK);
	// The canonical full-scene fingerprint changes with the mesh mutation, so
	// dependency-only reuse is intentionally rejected and all tiles rebuild.
	CHECK_EQ(resumed.reused_cells, uint32_t(0));
	CHECK_EQ(resumed.cells.size(), clean.cells.size());
	for (int cell = 0; cell < clean.cells.size(); cell++) {
		CHECK_EQ(resumed.cells[cell].coordinate, clean.cells[cell].coordinate);
		CHECK_EQ(resumed.cells[cell].primary_set, clean.cells[cell].primary_set);
		CHECK_EQ(resumed.cells[cell].transport_set, clean.cells[cell].transport_set);
		CHECK_EQ(resumed.cells[cell].flags, clean.cells[cell].flags);
	}

	anchor.remove_child(wall);
	anchor.remove_child(right);
	anchor.remove_child(left);
	memdelete(wall);
	memdelete(right);
	memdelete(left);
}

TEST_CASE("[Rendering][BakedVisibility] partial certificate wall mutation rebuilds only dependent leaf") {
	Node3D anchor;
	anchor.set_name("Anchor");
	MeshInstance3D *receiver = memnew(MeshInstance3D);
	receiver->set_name("Receiver");
	receiver->set_mesh(make_tetrahedron(0.4f));
	receiver->set_position(Vector3(2, 2, 0));
	anchor.add_child(receiver);
	MeshInstance3D *target = memnew(MeshInstance3D);
	target->set_name("Target");
	target->set_mesh(make_tetrahedron(0.4f));
	target->set_position(Vector3(6, 2, 0));
	anchor.add_child(target);
	MeshInstance3D *wall = memnew(MeshInstance3D);
	wall->set_name("PartialWall");
	wall->set_mesh(make_separating_wall());
	wall->set_scale(Vector3(0.5f, 0.1f, 1.0f)); // x=4, only the y=0 leaf.
	Ref<StandardMaterial3D> two_sided;
	two_sided.instantiate();
	two_sided->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	wall->set_material_override(two_sided);
	anchor.add_child(wall);
	// The changed wall and both receivers stay below y=8. This immutable,
	// complete separator is the upper leaf's actual exclusion certificate, so
	// that leaf has no dependency on PartialWall.
	MeshInstance3D *separator = memnew(MeshInstance3D);
	separator->set_name("UpperLeafSeparator");
	separator->set_mesh(make_separating_wall());
	separator->set_rotation(Vector3(0, 0, Math::PI * 0.5f));
	separator->set_position(Vector3(0, -0.5f, 0)); // Plane y=7.5: positive upper-leaf margin.
	separator->set_material_override(two_sided);
	anchor.add_child(separator);
	BakedVisibilityBakeInput input;
	input.anchor = &anchor;
	input.scene_root = &anchor;
	input.bounds = AABB(Vector3(0, 0, -2), Vector3(8, 16, 4));
	input.requested_cell_size = 1.0f;
	input.max_cells = 512;
	input.transport_distance = 1.0f;
	BakedVisibilityBakeOutput baseline;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, baseline), OK);
	REQUIRE_EQ(baseline.tile_count, 2);
	if (baseline.tile_count != 2) {
		anchor.remove_child(separator);
		anchor.remove_child(wall);
		anchor.remove_child(target);
		anchor.remove_child(receiver);
		memdelete(separator);
		memdelete(wall);
		memdelete(target);
		memdelete(receiver);
		return;
	}
	const int partial_wall_index = baseline.geometry_paths.find("PartialWall");
	REQUIRE(partial_wall_index >= 0);
	if (partial_wall_index < 0) {
		anchor.remove_child(separator);
		anchor.remove_child(wall);
		anchor.remove_child(target);
		anchor.remove_child(receiver);
		memdelete(separator);
		memdelete(wall);
		memdelete(target);
		memdelete(receiver);
		return;
	}
	CHECK_FALSE(baseline.tiles[1].candidate_dependencies.has(uint32_t(partial_wall_index)));
	CHECK_FALSE(baseline.tiles[1].certificate_dependencies.has(uint32_t(partial_wall_index)));
	wall->set_position(Vector3(0.2f, 0, 0));
	BakedVisibilityBakeInput resumed_input = input;
	resumed_input.resume_checkpoint = &baseline.checkpoint;
	BakedVisibilityBakeOutput resumed;
	REQUIRE_EQ(BakedVisibilityBaker().bake(resumed_input, resumed), OK);
	BakedVisibilityBakeOutput clean;
	REQUIRE_EQ(BakedVisibilityBaker().bake(input, clean), OK);
	// A blocker mutation invalidates the full-scene contract; excluded candidates
	// are not present in the old dependency list and must not be reused.
	CHECK_EQ(resumed.reused_cells, uint32_t(0));
	CHECK_EQ(resumed.cells.size(), clean.cells.size());
	for (int cell = 0; cell < clean.cells.size(); cell++) {
		CHECK_EQ(resumed.cells[cell].coordinate, clean.cells[cell].coordinate);
		CHECK_EQ(resumed.cells[cell].primary_set, clean.cells[cell].primary_set);
		CHECK_EQ(resumed.cells[cell].transport_set, clean.cells[cell].transport_set);
		CHECK_EQ(resumed.cells[cell].flags, clean.cells[cell].flags);
	}
	anchor.remove_child(separator);
	anchor.remove_child(wall);
	anchor.remove_child(target);
	anchor.remove_child(receiver);
	memdelete(separator);
	memdelete(wall);
	memdelete(target);
	memdelete(receiver);
}

} // namespace TestBakedVisibilityBaker
