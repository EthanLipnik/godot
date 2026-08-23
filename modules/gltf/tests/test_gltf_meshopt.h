/**************************************************************************/
/*  test_gltf_meshopt.h                                                   */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

#include "modules/gltf/gltf_document.h"
#include "modules/gltf/gltf_state.h"
#include "modules/gltf/structures/gltf_buffer_view.h"
#include "modules/meshoptimizer/streamed_cluster_mesh.h"
#include "modules/meshoptimizer/streamed_cluster_mesh_instance_3d.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "scene/resources/material.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

namespace TestGLTFMeshopt {

static StreamedClusterMeshInstance3D *_find_streamed_cluster_instance(Node *p_node) {
	if (StreamedClusterMeshInstance3D *instance = Object::cast_to<StreamedClusterMeshInstance3D>(p_node)) {
		return instance;
	}
	for (int child = 0; child < p_node->get_child_count(); child++) {
		if (StreamedClusterMeshInstance3D *instance = _find_streamed_cluster_instance(p_node->get_child(child))) {
			return instance;
		}
	}
	return nullptr;
}

static PackedByteArray _encode_vertex_bytes(const PackedByteArray &p_decoded, size_t p_count, size_t p_stride) {
	PackedByteArray encoded;
	encoded.resize(meshopt_encodeVertexBufferBound(p_count, p_stride));
	const size_t encoded_size = meshopt_encodeVertexBuffer(encoded.ptrw(), encoded.size(), p_decoded.ptr(), p_count, p_stride);
	encoded.resize(encoded_size);
	return encoded;
}

static Ref<GLTFBufferView> _make_compressed_view(const PackedByteArray &p_encoded, int64_t p_decoded_length, int64_t p_stride, int64_t p_count, const String &p_mode, const String &p_filter, Ref<GLTFState> &r_state, Error &r_error) {
	r_state.instantiate();
	Vector<PackedByteArray> buffers;
	buffers.push_back(p_encoded);
	buffers.push_back(PackedByteArray());
	r_state->set_buffers(buffers);
	r_state->set_buffer_source(0, String(), p_encoded.size(), false);
	r_state->set_buffer_source(1, String(), p_decoded_length, true);
	Ref<GLTFBufferView> view;
	view.instantiate();
	view->set_buffer(1);
	view->set_byte_length(p_decoded_length);
	Dictionary extension;
	extension["buffer"] = 0;
	extension["byteLength"] = p_encoded.size();
	extension["byteStride"] = p_stride;
	extension["count"] = p_count;
	extension["mode"] = p_mode;
	if (p_filter != "NONE") {
		extension["filter"] = p_filter;
	}
	r_error = view->configure_meshopt_compression(extension, r_state);
	return view;
}

static Error _write_direct_streamed_fixture(const String &p_directory, String &r_gltf_path, String &r_bin_path, int64_t &r_maximum_compressed_range) {
	const float position_values[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
	PackedByteArray positions;
	positions.resize(sizeof(position_values));
	memcpy(positions.ptrw(), position_values, sizeof(position_values));
	const PackedByteArray encoded_positions = _encode_vertex_bytes(positions, 3, 12);
	const uint32_t index_values[] = { 0, 1, 2 };
	PackedByteArray encoded_indices;
	encoded_indices.resize(meshopt_encodeIndexBufferBound(3, 3));
	encoded_indices.resize(meshopt_encodeIndexBuffer(encoded_indices.ptrw(), encoded_indices.size(), index_values, 3));
	PackedByteArray compressed = encoded_positions;
	compressed.append_array(encoded_indices);
	r_maximum_compressed_range = MAX(encoded_positions.size(), encoded_indices.size());

	Dictionary position_extension;
	position_extension["buffer"] = 0;
	position_extension["byteOffset"] = 0;
	position_extension["byteLength"] = encoded_positions.size();
	position_extension["byteStride"] = 12;
	position_extension["count"] = 3;
	position_extension["mode"] = "ATTRIBUTES";
	Dictionary position_extensions;
	position_extensions["EXT_meshopt_compression"] = position_extension;
	Dictionary position_view;
	position_view["buffer"] = 1;
	position_view["byteLength"] = 36;
	position_view["byteStride"] = 12;
	position_view["target"] = 34962;
	position_view["extensions"] = position_extensions;

	Dictionary index_extension;
	index_extension["buffer"] = 0;
	index_extension["byteOffset"] = encoded_positions.size();
	index_extension["byteLength"] = encoded_indices.size();
	index_extension["byteStride"] = 2;
	index_extension["count"] = 3;
	index_extension["mode"] = "TRIANGLES";
	Dictionary index_extensions;
	index_extensions["EXT_meshopt_compression"] = index_extension;
	Dictionary index_view;
	index_view["buffer"] = 1;
	index_view["byteOffset"] = 36;
	index_view["byteLength"] = 6;
	index_view["target"] = 34963;
	index_view["extensions"] = index_extensions;

	Dictionary position_accessor;
	position_accessor["bufferView"] = 0;
	position_accessor["componentType"] = 5126;
	position_accessor["count"] = 3;
	position_accessor["type"] = "VEC3";
	Dictionary index_accessor;
	index_accessor["bufferView"] = 1;
	index_accessor["componentType"] = 5123;
	index_accessor["count"] = 3;
	index_accessor["type"] = "SCALAR";
	Dictionary attributes;
	attributes["POSITION"] = 0;
	Dictionary primitive;
	primitive["attributes"] = attributes;
	primitive["indices"] = 1;
	primitive["material"] = 0;
	Dictionary unlit_primitive = primitive.duplicate(true);
	unlit_primitive["material"] = 1;
	Array primitives;
	primitives.push_back(primitive);
	primitives.push_back(unlit_primitive);
	Dictionary mesh;
	mesh["name"] = "DirectTriangle";
	mesh["primitives"] = primitives;

	Dictionary pbr;
	Array base_color;
	base_color.push_back(0.25);
	base_color.push_back(0.5);
	base_color.push_back(0.75);
	base_color.push_back(1.0);
	pbr["baseColorFactor"] = base_color;
	pbr["metallicFactor"] = 0.2;
	pbr["roughnessFactor"] = 0.7;
	Dictionary emissive_strength;
	emissive_strength["emissiveStrength"] = 12.0;
	Dictionary material_extensions;
	material_extensions["KHR_materials_emissive_strength"] = emissive_strength;
	Dictionary material;
	material["name"] = "DirectEmission";
	material["pbrMetallicRoughness"] = pbr;
	Array emissive_factor;
	emissive_factor.push_back(1.0);
	emissive_factor.push_back(0.5);
	emissive_factor.push_back(0.25);
	material["emissiveFactor"] = emissive_factor;
	material["doubleSided"] = true;
	material["extensions"] = material_extensions;
	Dictionary unlit_extensions;
	unlit_extensions["KHR_materials_unlit"] = Dictionary();
	Dictionary unlit_material;
	unlit_material["name"] = "DirectUnlit";
	unlit_material["pbrMetallicRoughness"] = pbr;
	unlit_material["extensions"] = unlit_extensions;

	Dictionary visibility;
	visibility["visible"] = false;
	Dictionary node_extensions;
	node_extensions["KHR_node_visibility"] = visibility;
	Dictionary node;
	node["name"] = "StreamedNode";
	node["mesh"] = 0;
	Array translation;
	translation.push_back(2.0);
	translation.push_back(3.0);
	translation.push_back(4.0);
	node["translation"] = translation;
	node["extensions"] = node_extensions;
	Dictionary scene;
	Array scene_nodes;
	scene_nodes.push_back(0);
	scene["name"] = "DirectStreamedFixture";
	scene["nodes"] = scene_nodes;

	Dictionary compressed_buffer;
	compressed_buffer["uri"] = "direct_meshopt_fixture.bin";
	compressed_buffer["byteLength"] = compressed.size();
	Dictionary fallback_buffer;
	fallback_buffer["byteLength"] = 42;
	Dictionary asset;
	asset["version"] = "2.0";
	Dictionary json;
	json["asset"] = asset;
	Array extensions_used;
	extensions_used.push_back("EXT_meshopt_compression");
	extensions_used.push_back("KHR_materials_emissive_strength");
	extensions_used.push_back("KHR_materials_unlit");
	extensions_used.push_back("KHR_node_visibility");
	json["extensionsUsed"] = extensions_used;
	Array extensions_required;
	extensions_required.push_back("EXT_meshopt_compression");
	json["extensionsRequired"] = extensions_required;
	Array buffers;
	buffers.push_back(compressed_buffer);
	buffers.push_back(fallback_buffer);
	json["buffers"] = buffers;
	Array buffer_views;
	buffer_views.push_back(position_view);
	buffer_views.push_back(index_view);
	json["bufferViews"] = buffer_views;
	Array accessors;
	accessors.push_back(position_accessor);
	accessors.push_back(index_accessor);
	json["accessors"] = accessors;
	Array meshes;
	meshes.push_back(mesh);
	json["meshes"] = meshes;
	Array materials;
	materials.push_back(material);
	materials.push_back(unlit_material);
	json["materials"] = materials;
	Array nodes;
	nodes.push_back(node);
	json["nodes"] = nodes;
	Array scenes;
	scenes.push_back(scene);
	json["scenes"] = scenes;
	json["scene"] = 0;

	const Error directory_error = DirAccess::make_dir_recursive_absolute(p_directory);
	if (directory_error != OK && directory_error != ERR_ALREADY_EXISTS) {
		return directory_error;
	}
	r_gltf_path = p_directory.path_join("direct_meshopt_fixture.gltf");
	r_bin_path = p_directory.path_join("direct_meshopt_fixture.bin");
	Ref<FileAccess> bin_file = FileAccess::open(r_bin_path, FileAccess::WRITE);
	ERR_FAIL_COND_V(bin_file.is_null(), ERR_FILE_CANT_OPEN);
	bin_file->store_buffer(compressed.ptr(), compressed.size());
	ERR_FAIL_COND_V(bin_file->get_error() != OK, bin_file->get_error());
	bin_file.unref();
	Ref<FileAccess> json_file = FileAccess::open(r_gltf_path, FileAccess::WRITE);
	ERR_FAIL_COND_V(json_file.is_null(), ERR_FILE_CANT_OPEN);
	json_file->store_string(JSON::stringify(json));
	ERR_FAIL_COND_V(json_file->get_error() != OK, json_file->get_error());
	return OK;
}

TEST_CASE("[SceneTree][GLTFDocument] EXT_meshopt_compression decodes all modes") {
	PackedByteArray attributes;
	attributes.resize(4 * 8);
	for (int i = 0; i < attributes.size(); i++) {
		attributes.set(i, uint8_t(i * 7 + 3));
	}
	PackedByteArray encoded_attributes = _encode_vertex_bytes(attributes, 8, 4);
	Ref<GLTFState> state;
	Error error = OK;
	Ref<GLTFBufferView> view = _make_compressed_view(encoded_attributes, attributes.size(), 4, 8, "ATTRIBUTES", "NONE", state, error);
	CHECK(error == OK);
	CHECK(view->load_buffer_view_data(state) == attributes);

	const uint32_t triangle_indices[] = { 0, 1, 2, 2, 1, 3 };
	PackedByteArray encoded_triangles;
	encoded_triangles.resize(meshopt_encodeIndexBufferBound(6, 4));
	encoded_triangles.resize(meshopt_encodeIndexBuffer(encoded_triangles.ptrw(), encoded_triangles.size(), triangle_indices, 6));
	view = _make_compressed_view(encoded_triangles, 6 * 4, 4, 6, "TRIANGLES", "NONE", state, error);
	CHECK(error == OK);
	PackedByteArray decoded_triangles = view->load_buffer_view_data(state);
	REQUIRE(decoded_triangles.size() == int64_t(sizeof(triangle_indices)));
	CHECK(memcmp(decoded_triangles.ptr(), triangle_indices, sizeof(triangle_indices)) == 0);

	const uint32_t sequence_indices[] = { 9, 3, 17, 4, 4, 100 };
	PackedByteArray encoded_sequence;
	encoded_sequence.resize(meshopt_encodeIndexSequenceBound(6, 101));
	encoded_sequence.resize(meshopt_encodeIndexSequence(encoded_sequence.ptrw(), encoded_sequence.size(), sequence_indices, 6));
	view = _make_compressed_view(encoded_sequence, 6 * 4, 4, 6, "INDICES", "NONE", state, error);
	CHECK(error == OK);
	PackedByteArray decoded_sequence = view->load_buffer_view_data(state);
	REQUIRE(decoded_sequence.size() == int64_t(sizeof(sequence_indices)));
	CHECK(memcmp(decoded_sequence.ptr(), sequence_indices, sizeof(sequence_indices)) == 0);
	CHECK(GLTFDocument::get_supported_gltf_extensions_hashset().has("EXT_meshopt_compression"));
}

TEST_CASE("[SceneTree][GLTFDocument] EXT_meshopt_compression applies all filters") {
	struct FilterCase {
		String name;
		int stride;
	};
	const FilterCase cases[] = { { "OCTAHEDRAL", 4 }, { "QUATERNION", 8 }, { "EXPONENTIAL", 12 } };
	for (const FilterCase &filter_case : cases) {
		constexpr int count = 4;
		PackedByteArray filtered;
		filtered.resize(count * filter_case.stride);
		if (filter_case.name == "OCTAHEDRAL") {
			const float vectors[count * 4] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, -1, 0, 0, -1 };
			meshopt_encodeFilterOct(filtered.ptrw(), count, filter_case.stride, 8, vectors);
		} else if (filter_case.name == "QUATERNION") {
			const float quaternions[count * 4] = { 0, 0, 0, 1, 0.7071067f, 0, 0, 0.7071067f, 0, 0.7071067f, 0, 0.7071067f, 0, 0, 0.7071067f, 0.7071067f };
			meshopt_encodeFilterQuat(filtered.ptrw(), count, filter_case.stride, 16, quaternions);
		} else {
			const float values[count * 3] = { 1, 2, 3, -4, 5, 6, 0.25f, 0.5f, 0.75f, 100, -20, 0.125f };
			meshopt_encodeFilterExp(filtered.ptrw(), count, filter_case.stride, 20, values, meshopt_EncodeExpSharedVector);
		}
		PackedByteArray expected = filtered;
		if (filter_case.name == "OCTAHEDRAL") {
			meshopt_decodeFilterOct(expected.ptrw(), count, filter_case.stride);
		} else if (filter_case.name == "QUATERNION") {
			meshopt_decodeFilterQuat(expected.ptrw(), count, filter_case.stride);
		} else {
			meshopt_decodeFilterExp(expected.ptrw(), count, filter_case.stride);
		}
		const PackedByteArray encoded = _encode_vertex_bytes(filtered, count, filter_case.stride);
		Ref<GLTFState> state;
		Error error = OK;
		Ref<GLTFBufferView> view = _make_compressed_view(encoded, filtered.size(), filter_case.stride, count, "ATTRIBUTES", filter_case.name, state, error);
		CHECK(error == OK);
		CHECK(view->load_buffer_view_data(state) == expected);
	}
}

TEST_CASE("[SceneTree][GLTFDocument] EXT_meshopt_compression rejects malformed ranges and layout") {
	PackedByteArray compressed;
	compressed.resize(16);
	Ref<GLTFState> state;
	Error error = OK;
	ERR_PRINT_OFF;
	Ref<GLTFBufferView> view = _make_compressed_view(compressed, 32, 3, 8, "ATTRIBUTES", "NONE", state, error);
	CHECK(error == ERR_PARSE_ERROR);
	view = _make_compressed_view(compressed, 20, 4, 8, "ATTRIBUTES", "NONE", state, error);
	CHECK(error == ERR_PARSE_ERROR);
	view = _make_compressed_view(compressed, 24, 4, 6, "TRIANGLES", "OCTAHEDRAL", state, error);
	CHECK(error == ERR_PARSE_ERROR);

	PackedByteArray corrupt_bitstream;
	corrupt_bitstream.resize(1);
	corrupt_bitstream.set(0, 0xff);
	view = _make_compressed_view(corrupt_bitstream, 32, 4, 8, "ATTRIBUTES", "NONE", state, error);
	CHECK(error == OK);
	CHECK(view->load_buffer_view_data(state).is_empty());
	ERR_PRINT_ON;
}

TEST_CASE("[SceneTree][GLTFDocument] External buffer ranges use 64-bit sparse-file offsets") {
	const String path = OS::get_singleton()->get_cache_path().path_join("godot_gltf_meshopt_sparse_test.bin");
	const uint64_t offset = 0x100000000ull + 37;
	Error open_error = OK;
	Ref<FileAccess> output = FileAccess::open(path, FileAccess::WRITE, &open_error);
	REQUIRE(open_error == OK);
	output->seek(offset);
	const uint8_t expected[] = { 11, 22, 33, 44 };
	output->store_buffer(expected, sizeof(expected));
	output.unref();

	Ref<GLTFState> state;
	state.instantiate();
	Vector<PackedByteArray> buffers;
	buffers.push_back(PackedByteArray());
	state->set_buffers(buffers);
	state->set_buffer_source(0, path, offset + sizeof(expected), false);
	PackedByteArray actual;
	CHECK(state->read_buffer_range(0, offset, sizeof(expected), actual) == OK);
	REQUIRE(actual.size() == int64_t(sizeof(expected)));
	CHECK(memcmp(actual.ptr(), expected, sizeof(expected)) == 0);
	CHECK(state->get_maximum_source_read() == sizeof(expected));
	CHECK(state->get_source_bytes_read() == sizeof(expected));
	DirAccess::remove_absolute(path);
}

TEST_CASE("[SceneTree][GLTFDocument] Meshopt fixture imports through the normal scene path") {
	const float position_values[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
	PackedByteArray positions;
	positions.resize(sizeof(position_values));
	memcpy(positions.ptrw(), position_values, sizeof(position_values));
	const PackedByteArray encoded_positions = _encode_vertex_bytes(positions, 3, 12);
	const uint32_t index_values[] = { 0, 1, 2 };
	PackedByteArray encoded_indices;
	encoded_indices.resize(meshopt_encodeIndexBufferBound(3, 3));
	encoded_indices.resize(meshopt_encodeIndexBuffer(encoded_indices.ptrw(), encoded_indices.size(), index_values, 3));
	PackedByteArray compressed = encoded_positions;
	compressed.append_array(encoded_indices);

	Dictionary position_extension;
	position_extension["buffer"] = 0;
	position_extension["byteOffset"] = 0;
	position_extension["byteLength"] = encoded_positions.size();
	position_extension["byteStride"] = 12;
	position_extension["count"] = 3;
	position_extension["mode"] = "ATTRIBUTES";
	Dictionary position_extensions;
	position_extensions["EXT_meshopt_compression"] = position_extension;
	Dictionary position_view;
	position_view["buffer"] = 1;
	position_view["byteOffset"] = 0;
	position_view["byteLength"] = 36;
	position_view["byteStride"] = 12;
	position_view["target"] = 34962;
	position_view["extensions"] = position_extensions;

	Dictionary index_extension;
	index_extension["buffer"] = 0;
	index_extension["byteOffset"] = encoded_positions.size();
	index_extension["byteLength"] = encoded_indices.size();
	index_extension["byteStride"] = 2;
	index_extension["count"] = 3;
	index_extension["mode"] = "TRIANGLES";
	Dictionary index_extensions;
	index_extensions["EXT_meshopt_compression"] = index_extension;
	Dictionary index_view;
	index_view["buffer"] = 1;
	index_view["byteOffset"] = 36;
	index_view["byteLength"] = 6;
	index_view["target"] = 34963;
	index_view["extensions"] = index_extensions;

	Dictionary position_accessor;
	position_accessor["bufferView"] = 0;
	position_accessor["componentType"] = 5126;
	position_accessor["count"] = 3;
	position_accessor["type"] = "VEC3";
	Array position_min;
	position_min.push_back(0.0);
	position_min.push_back(0.0);
	position_min.push_back(0.0);
	position_accessor["min"] = position_min;
	Array position_max;
	position_max.push_back(1.0);
	position_max.push_back(1.0);
	position_max.push_back(0.0);
	position_accessor["max"] = position_max;
	Dictionary index_accessor;
	index_accessor["bufferView"] = 1;
	index_accessor["componentType"] = 5123;
	index_accessor["count"] = 3;
	index_accessor["type"] = "SCALAR";
	Dictionary attributes;
	attributes["POSITION"] = 0;
	Dictionary primitive;
	primitive["attributes"] = attributes;
	primitive["indices"] = 1;
	Dictionary mesh;
	Array primitives;
	primitives.push_back(primitive);
	mesh["primitives"] = primitives;
	Dictionary node;
	node["mesh"] = 0;
	Dictionary scene;
	Array scene_nodes;
	scene_nodes.push_back(0);
	scene["nodes"] = scene_nodes;
	Dictionary asset;
	asset["version"] = "2.0";
	Dictionary compressed_buffer;
	compressed_buffer["uri"] = "meshopt_fixture.bin";
	compressed_buffer["byteLength"] = compressed.size();
	Dictionary fallback_buffer;
	fallback_buffer["byteLength"] = 42;
	Dictionary json;
	json["asset"] = asset;
	Array extensions;
	extensions.push_back("EXT_meshopt_compression");
	json["extensionsUsed"] = extensions;
	json["extensionsRequired"] = extensions;
	Array buffers;
	buffers.push_back(compressed_buffer);
	buffers.push_back(fallback_buffer);
	json["buffers"] = buffers;
	Array buffer_views;
	buffer_views.push_back(position_view);
	buffer_views.push_back(index_view);
	json["bufferViews"] = buffer_views;
	Array accessors;
	accessors.push_back(position_accessor);
	accessors.push_back(index_accessor);
	json["accessors"] = accessors;
	Array meshes;
	meshes.push_back(mesh);
	json["meshes"] = meshes;
	Array nodes;
	nodes.push_back(node);
	json["nodes"] = nodes;
	Array scenes;
	scenes.push_back(scene);
	json["scenes"] = scenes;
	json["scene"] = 0;

	const String directory = OS::get_singleton()->get_cache_path().path_join("godot_meshopt_fixture");
	DirAccess::make_dir_recursive_absolute(directory);
	const String gltf_path = directory.path_join("meshopt_fixture.gltf");
	const String bin_path = directory.path_join("meshopt_fixture.bin");
	Ref<FileAccess> bin_file = FileAccess::open(bin_path, FileAccess::WRITE);
	REQUIRE(bin_file.is_valid());
	bin_file->store_buffer(compressed.ptr(), compressed.size());
	bin_file.unref();
	Ref<FileAccess> json_file = FileAccess::open(gltf_path, FileAccess::WRITE);
	REQUIRE(json_file.is_valid());
	json_file->store_string(JSON::stringify(json));
	json_file.unref();

	Ref<GLTFDocument> document;
	document.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	CHECK(document->append_from_file(gltf_path, state) == OK);
	Node *root = document->generate_scene(state);
	REQUIRE(root != nullptr);
	CHECK((root->get_child_count() == 1 || root->get_class() == StringName("ImporterMeshInstance3D")));
	memdelete(root);
	CHECK(state->get_maximum_source_read() <= uint64_t(MAX(encoded_positions.size(), encoded_indices.size())));

	DirAccess::remove_absolute(gltf_path);
	DirAccess::remove_absolute(bin_path);
	DirAccess::remove_absolute(directory);
}

TEST_CASE("[SceneTree][GLTFDocument] direct streamed meshopt import emits a normal hierarchy and page cache") {
	const String directory = OS::get_singleton()->get_cache_path().path_join(vformat("godot_meshopt_direct_fixture_%d", OS::get_singleton()->get_process_id()));
	const String cache_directory = directory.path_join("streamed_cache");
	String gltf_path;
	String bin_path;
	int64_t maximum_compressed_range = 0;
	REQUIRE(_write_direct_streamed_fixture(directory, gltf_path, bin_path, maximum_compressed_range) == OK);

	Ref<GLTFDocument> document;
	document.instantiate();
	Node *root = nullptr;
	Dictionary diagnostics;
	CHECK(document->import_streamed_cluster_scene(gltf_path, cache_directory, 4, 1, 0, root, diagnostics) == OK);
	REQUIRE(root != nullptr);
	CHECK(root->get_name() == StringName("DirectStreamedFixture"));
	CHECK(root->has_meta("streamed_cluster_diagnostics"));
	CHECK(int64_t(diagnostics["compiled_primitives"]) == 2);
	CHECK(int64_t(diagnostics["compiled_triangles"]) == 2);
	CHECK(int64_t(diagnostics["compiled_pages"]) > 0);
	CHECK(int64_t(diagnostics["cache_bytes"]) > 0);
	CHECK(int64_t(diagnostics["source_range_read_count"]) == 4);
	CHECK(int64_t(diagnostics["maximum_source_range_read"]) <= maximum_compressed_range);
	CHECK(int64_t(diagnostics["maximum_primitive_input_bytes"]) == 48);
	CHECK(int64_t(diagnostics["unsupported_total"]) == 0);

	StreamedClusterMeshInstance3D *instance = _find_streamed_cluster_instance(root);
	REQUIRE(instance != nullptr);
	REQUIRE(instance->get_parent() != nullptr);
	Node3D *mesh_node = Object::cast_to<Node3D>(instance->get_parent());
	REQUIRE(mesh_node != nullptr);
	CHECK(mesh_node->get_position().is_equal_approx(Vector3(2, 3, 4)));
	CHECK_FALSE(mesh_node->is_visible());
	REQUIRE(mesh_node->get_child_count() == 2);
	StreamedClusterMeshInstance3D *emissive_instance = Object::cast_to<StreamedClusterMeshInstance3D>(mesh_node->get_child(0));
	StreamedClusterMeshInstance3D *unlit_instance = Object::cast_to<StreamedClusterMeshInstance3D>(mesh_node->get_child(1));
	REQUIRE(emissive_instance != nullptr);
	REQUIRE(unlit_instance != nullptr);
	Ref<StreamedClusterMesh> emissive_mesh = emissive_instance->get_streamed_mesh();
	Ref<StreamedClusterMesh> unlit_mesh = unlit_instance->get_streamed_mesh();
	REQUIRE(emissive_mesh.is_valid());
	REQUIRE(unlit_mesh.is_valid());
	CHECK(emissive_mesh->get_page_count() + unlit_mesh->get_page_count() == int64_t(diagnostics["compiled_pages"]));
	const String emissive_manifest_path = cache_directory.path_join("mesh_000000_primitive_0000.tres");
	const String unlit_manifest_path = cache_directory.path_join("mesh_000000_primitive_0001.tres");
	CHECK(FileAccess::exists(emissive_manifest_path));
	CHECK(FileAccess::exists(unlit_manifest_path));
	const TypedArray<Material> materials = emissive_mesh->get_materials();
	REQUIRE(materials.size() == 1);
	Ref<StandardMaterial3D> material = materials[0];
	REQUIRE(material.is_valid());
	CHECK(material->get_name() == StringName("DirectEmission"));
	CHECK(Math::is_equal_approx(material->get_metallic(), 0.2f));
	CHECK(Math::is_equal_approx(material->get_roughness(), 0.7f));
	CHECK(material->get_albedo().is_equal_approx(Color(0.25, 0.5, 0.75, 1.0).linear_to_srgb()));
	CHECK(Math::is_equal_approx(material->get_emission_energy_multiplier(), 12.0f));
	CHECK(material->get_feature(BaseMaterial3D::FEATURE_EMISSION));
	CHECK(material->get_cull_mode() == BaseMaterial3D::CULL_DISABLED);
	const TypedArray<Material> unlit_materials = unlit_mesh->get_materials();
	REQUIRE(unlit_materials.size() == 1);
	Ref<StandardMaterial3D> unlit_material = unlit_materials[0];
	REQUIRE(unlit_material.is_valid());
	CHECK(unlit_material->get_name() == StringName("DirectUnlit"));
	CHECK(unlit_material->get_shading_mode() == BaseMaterial3D::SHADING_MODE_UNSHADED);

	PackedStringArray page_paths;
	const Ref<StreamedClusterMesh> compiled_meshes[] = { emissive_mesh, unlit_mesh };
	for (const Ref<StreamedClusterMesh> &compiled_mesh : compiled_meshes) {
		for (int page_index = 0; page_index < compiled_mesh->get_page_count(); page_index++) {
			const StreamedClusterMesh::Page *page = compiled_mesh->get_page(page_index);
			REQUIRE(page != nullptr);
			CHECK(FileAccess::exists(page->blob_path));
			page_paths.push_back(page->blob_path);
		}
	}
	memdelete(root);
	material.unref();
	unlit_material.unref();
	emissive_mesh.unref();
	unlit_mesh.unref();
	for (const String &page_path : page_paths) {
		DirAccess::remove_absolute(page_path);
	}
	DirAccess::remove_absolute(emissive_manifest_path.get_basename() + ".pages");
	DirAccess::remove_absolute(unlit_manifest_path.get_basename() + ".pages");
	DirAccess::remove_absolute(emissive_manifest_path);
	DirAccess::remove_absolute(unlit_manifest_path);
	DirAccess::remove_absolute(cache_directory);
	DirAccess::remove_absolute(gltf_path);
	DirAccess::remove_absolute(bin_path);
	DirAccess::remove_absolute(directory);
}

} // namespace TestGLTFMeshopt
