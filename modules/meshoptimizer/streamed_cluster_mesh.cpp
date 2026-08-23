/**************************************************************************/
/*  streamed_cluster_mesh.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "streamed_cluster_mesh.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

#include <cmath>
#include <limits>

static bool _checked_size_product(size_t p_left, size_t p_right, size_t &r_result) {
	if (p_left != 0 && p_right > SIZE_MAX / p_left) {
		return false;
	}
	r_result = p_left * p_right;
	return r_result <= size_t(INT64_MAX);
}

static bool _checked_size_sum(size_t p_left, size_t p_right, size_t &r_result) {
	if (p_right > SIZE_MAX - p_left) {
		return false;
	}
	r_result = p_left + p_right;
	return r_result <= size_t(INT64_MAX);
}

template <typename T>
static bool _can_allocate_vector(size_t p_count) {
	size_t byte_count = 0;
	return p_count <= size_t(INT64_MAX) && _checked_size_product(p_count, sizeof(T), byte_count);
}

static bool _make_conservative_axis(real_t p_minimum, real_t p_maximum, real_t &r_position, real_t &r_size) {
	const real_t positive_infinity = std::numeric_limits<real_t>::infinity();
	r_position = std::nextafter(p_minimum, -positive_infinity);
	const real_t outward_end = std::nextafter(p_maximum, positive_infinity);
	r_size = std::nextafter(outward_end - r_position, positive_infinity);
	while (Math::is_finite(r_size) && r_position + r_size < p_maximum) {
		const real_t next_size = std::nextafter(r_size, positive_infinity);
		if (next_size == r_size) {
			return false;
		}
		r_size = next_size;
	}
	return Math::is_finite(r_position) && Math::is_finite(r_size) && r_size >= 0.0f;
}

static bool _make_conservative_bounds(const AABB &p_bounds, AABB &r_bounds) {
	if (!p_bounds.is_finite()) {
		return false;
	}
	const Vector3 minimum = p_bounds.position;
	const Vector3 maximum = p_bounds.get_end();
	Vector3 position;
	Vector3 size;
	return _make_conservative_axis(minimum.x, maximum.x, position.x, size.x) &&
			_make_conservative_axis(minimum.y, maximum.y, position.y, size.y) &&
			_make_conservative_axis(minimum.z, maximum.z, position.z, size.z) &&
			(r_bounds = AABB(position, size)).is_finite();
}

uint64_t StreamedClusterMesh::_hash_bytes(const uint8_t *p_data, uint64_t p_size, uint64_t p_seed) {
	uint64_t hash = p_seed;
	for (uint64_t i = 0; i < p_size; i++) {
		hash ^= p_data[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

uint64_t StreamedClusterMesh::_make_stable_id(uint64_t p_content_hash, uint32_t p_lod_level, uint32_t p_group) {
	uint64_t hash = _hash_bytes(reinterpret_cast<const uint8_t *>(&p_lod_level), sizeof(p_lod_level), p_content_hash);
	hash = _hash_bytes(reinterpret_cast<const uint8_t *>(&p_group), sizeof(p_group), hash);
	hash &= uint64_t(INT64_MAX);
	return hash == 0 ? 1 : hash;
}

PackedByteArray StreamedClusterMesh::_pack_page(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) {
	const uint64_t vertex_bytes = uint64_t(p_vertices.size()) * 3ull * sizeof(float);
	const uint64_t index_bytes = uint64_t(p_indices.size()) * sizeof(uint32_t);
	ERR_FAIL_COND_V(vertex_bytes > uint64_t(UINT32_MAX) || index_bytes > uint64_t(UINT32_MAX) || vertex_bytes + index_bytes > uint64_t(INT64_MAX), PackedByteArray());
	PackedByteArray payload;
	ERR_FAIL_COND_V(payload.resize(int64_t(vertex_bytes + index_bytes)) != OK, PackedByteArray());
	uint8_t *write = payload.ptrw();
	for (int64_t i = 0; i < p_vertices.size(); i++) {
		const Vector3 vertex = p_vertices[i];
		const float packed[3] = { float(vertex.x), float(vertex.y), float(vertex.z) };
		memcpy(write + i * 3 * sizeof(float), packed, sizeof(packed));
	}
	uint32_t *index_write = reinterpret_cast<uint32_t *>(write + vertex_bytes);
	for (int64_t i = 0; i < p_indices.size(); i++) {
		index_write[i] = uint32_t(p_indices[i]);
	}
	return payload;
}

Error StreamedClusterMesh::_build_local_page(const PackedVector3Array &p_source_vertices, const Vector<unsigned int> &p_global_indices, uint32_t p_material_index, uint32_t p_lod_level, uint32_t p_group, float p_error, bool p_persistent, Page &r_page, PackedByteArray &r_payload) {
	ERR_FAIL_COND_V(p_global_indices.is_empty() || p_global_indices.size() % 3 != 0, ERR_INVALID_DATA);
	HashMap<unsigned int, uint32_t> local_by_global;
	PackedVector3Array local_vertices;
	PackedInt32Array local_indices;
	ERR_FAIL_COND_V(p_global_indices.size() > size_t(INT64_MAX), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(!_can_allocate_vector<Vector3>(p_global_indices.size()) || !_can_allocate_vector<int32_t>(p_global_indices.size()), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(local_vertices.reserve(int64_t(p_global_indices.size())) != OK || local_indices.resize(int64_t(p_global_indices.size())) != OK, ERR_OUT_OF_MEMORY);
	AABB bounds;
	bool have_bounds = false;
	for (int64_t i = 0; i < p_global_indices.size(); i++) {
		const unsigned int global_index = p_global_indices[i];
		ERR_FAIL_COND_V(global_index >= uint64_t(p_source_vertices.size()), ERR_INVALID_DATA);
		uint32_t *existing = local_by_global.getptr(global_index);
		uint32_t local_index;
		if (existing) {
			local_index = *existing;
		} else {
			local_index = local_vertices.size();
			local_by_global.insert(global_index, local_index);
			const Vector3 vertex = p_source_vertices[global_index];
			local_vertices.push_back(vertex);
			if (!have_bounds) {
				bounds = AABB(vertex, Vector3());
				have_bounds = true;
			} else {
				bounds.expand_to(vertex);
			}
		}
		local_indices.set(i, int32_t(local_index));
	}

	r_payload = _pack_page(local_vertices, local_indices);
	ERR_FAIL_COND_V(r_payload.is_empty(), ERR_OUT_OF_MEMORY);
	r_page.content_hash = _hash_bytes(r_payload.ptr(), r_payload.size()) & uint64_t(INT64_MAX);
	r_page.stable_id = _make_stable_id(r_page.content_hash, p_lod_level, p_group);
	r_page.revision = r_page.content_hash == 0 ? 1 : r_page.content_hash;
	r_page.bounds = bounds;
	r_page.geometric_error = p_error;
	r_page.material_index = p_material_index;
	r_page.lod_level = p_lod_level;
	r_page.vertex_count = local_vertices.size();
	r_page.index_count = local_indices.size();
	r_page.persistent = p_persistent;
	return OK;
}

Dictionary StreamedClusterMesh::_page_to_dictionary(const Page &p_page) const {
	Dictionary dictionary;
	dictionary["stable_id"] = int64_t(p_page.stable_id);
	dictionary["revision"] = int64_t(p_page.revision & uint64_t(INT64_MAX));
	dictionary["content_hash"] = int64_t(p_page.content_hash & uint64_t(INT64_MAX));
	dictionary["bounds"] = p_page.bounds;
	dictionary["geometric_error"] = p_page.geometric_error;
	dictionary["material_index"] = p_page.material_index;
	dictionary["lod_level"] = p_page.lod_level;
	dictionary["vertex_count"] = p_page.vertex_count;
	dictionary["index_count"] = p_page.index_count;
	dictionary["parent_id"] = int64_t(p_page.parent_id);
	dictionary["child_ids"] = p_page.child_ids;
	dictionary["blob_path"] = p_page.blob_path;
	dictionary["blob_offset"] = int64_t(p_page.blob_offset);
	dictionary["blob_length"] = int64_t(p_page.blob_length);
	dictionary["persistent"] = p_page.persistent;
	return dictionary;
}

Error StreamedClusterMesh::_page_from_dictionary(const Dictionary &p_dictionary, Page &r_page) const {
	static const char *required[] = { "stable_id", "revision", "content_hash", "bounds", "geometric_error", "material_index", "lod_level", "vertex_count", "index_count", "parent_id", "child_ids", "blob_path", "blob_offset", "blob_length", "persistent" };
	for (const char *key : required) {
		ERR_FAIL_COND_V_MSG(!p_dictionary.has(key), ERR_INVALID_DATA, vformat("StreamedClusterMesh page is missing '%s'.", key));
	}
	const int64_t stable_id = p_dictionary["stable_id"];
	const int64_t revision = p_dictionary["revision"];
	const int64_t content_hash = p_dictionary["content_hash"];
	const int64_t material_index = p_dictionary["material_index"];
	const int64_t lod_level = p_dictionary["lod_level"];
	const int64_t vertex_count = p_dictionary["vertex_count"];
	const int64_t index_count = p_dictionary["index_count"];
	const int64_t parent_id = p_dictionary["parent_id"];
	const int64_t blob_offset = p_dictionary["blob_offset"];
	const int64_t blob_length = p_dictionary["blob_length"];
	ERR_FAIL_COND_V_MSG(stable_id <= 0 || revision <= 0 || content_hash < 0 || parent_id < 0, ERR_INVALID_DATA, "StreamedClusterMesh page has an invalid signed identity or revision.");
	ERR_FAIL_COND_V_MSG(material_index < 0 || material_index > UINT32_MAX || lod_level < 0 || lod_level > UINT32_MAX, ERR_INVALID_DATA, "StreamedClusterMesh page has an invalid material or LOD index.");
	ERR_FAIL_COND_V_MSG(vertex_count <= 0 || vertex_count > UINT32_MAX || index_count <= 0 || index_count > UINT32_MAX || index_count % 3 != 0, ERR_INVALID_DATA, "StreamedClusterMesh page has invalid triangle counts.");
	ERR_FAIL_COND_V_MSG(blob_offset != 0 || blob_length <= PAGE_HEADER_SIZE || uint64_t(blob_length) > uint64_t(UINT32_MAX), ERR_INVALID_DATA, "StreamedClusterMesh page blob has an invalid range or exceeds the uint32 GPU-buffer limit.");
	r_page.stable_id = uint64_t(stable_id);
	r_page.revision = uint64_t(revision);
	r_page.content_hash = uint64_t(content_hash);
	r_page.bounds = p_dictionary["bounds"];
	r_page.geometric_error = p_dictionary["geometric_error"];
	ERR_FAIL_COND_V_MSG(!r_page.bounds.is_finite() || !Math::is_finite(r_page.geometric_error) || r_page.geometric_error < 0.0f, ERR_INVALID_DATA, "StreamedClusterMesh page has invalid bounds or geometric error.");
	r_page.material_index = uint32_t(material_index);
	r_page.lod_level = uint32_t(lod_level);
	r_page.vertex_count = uint32_t(vertex_count);
	r_page.index_count = uint32_t(index_count);
	r_page.parent_id = uint64_t(parent_id);
	r_page.child_ids = p_dictionary["child_ids"];
	for (int64_t child_id : r_page.child_ids) {
		ERR_FAIL_COND_V_MSG(child_id <= 0, ERR_INVALID_DATA, "StreamedClusterMesh page has an invalid child ID.");
	}
	r_page.blob_path = p_dictionary["blob_path"];
	ERR_FAIL_COND_V_MSG(r_page.blob_path.is_empty(), ERR_INVALID_DATA, "StreamedClusterMesh page has no blob path.");
	r_page.blob_offset = uint64_t(blob_offset);
	r_page.blob_length = uint64_t(blob_length);
	r_page.persistent = p_dictionary["persistent"];
	return OK;
}

void StreamedClusterMesh::set_pages(const Array &p_pages) {
	pages.clear();
	total_blob_bytes = 0;
	maximum_blob_bytes = 0;
	for (int i = 0; i < p_pages.size(); i++) {
		ERR_CONTINUE_MSG(p_pages[i].get_type() != Variant::DICTIONARY, "StreamedClusterMesh page metadata must be a Dictionary.");
		Page page;
		ERR_CONTINUE(_page_from_dictionary(p_pages[i], page) != OK);
		pages.push_back(page);
		total_blob_bytes += page.blob_length;
		maximum_blob_bytes = MAX(maximum_blob_bytes, page.blob_length);
	}
}

Array StreamedClusterMesh::get_pages() const {
	Array result;
	for (const Page &page : pages) {
		result.push_back(_page_to_dictionary(page));
	}
	return result;
}

void StreamedClusterMesh::set_materials(const TypedArray<Material> &p_materials) {
	materials.clear();
	for (int i = 0; i < p_materials.size(); i++) {
		materials.push_back(p_materials[i]);
	}
}

TypedArray<Material> StreamedClusterMesh::get_materials() const {
	TypedArray<Material> result;
	for (const Ref<Material> &material : materials) {
		result.push_back(material);
	}
	return result;
}

int StreamedClusterMesh::get_persistent_page_count() const {
	int count = 0;
	for (const Page &page : pages) {
		count += page.persistent ? 1 : 0;
	}
	return count;
}

const StreamedClusterMesh::Page *StreamedClusterMesh::get_page(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, pages.size(), nullptr);
	return &pages[p_index];
}

int StreamedClusterMesh::find_page(uint64_t p_stable_id) const {
	for (int i = 0; i < pages.size(); i++) {
		if (pages[i].stable_id == p_stable_id) {
			return i;
		}
	}
	return -1;
}

PackedByteArray StreamedClusterMesh::load_page_payload(int p_page, Error *r_error) const {
	if (r_error) {
		*r_error = OK;
	}
	ERR_FAIL_INDEX_V(p_page, pages.size(), PackedByteArray());
	const Page &page = pages[p_page];
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(page.blob_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		if (r_error) {
			*r_error = open_error;
		}
		return PackedByteArray();
	}
	const uint32_t magic = file->get_32();
	const uint32_t version = file->get_32();
	const uint32_t header_size = file->get_32();
	const uint32_t flags = file->get_32();
	const uint64_t stable_id = file->get_64();
	const uint64_t revision = file->get_64();
	const uint32_t vertex_count = file->get_32();
	const uint32_t index_count = file->get_32();
	const uint32_t vertex_stride = file->get_32();
	const uint32_t index_stride = file->get_32();
	const uint32_t material_index = file->get_32();
	const uint32_t lod_level = file->get_32();
	const uint64_t payload_size = file->get_64();
	const uint64_t content_hash = file->get_64();
	if (magic != PAGE_MAGIC || version != FORMAT_VERSION || header_size != PAGE_HEADER_SIZE || flags != (page.persistent ? 1u : 0u) || stable_id != page.stable_id || revision != page.revision || content_hash != page.content_hash || vertex_count != page.vertex_count || index_count != page.index_count || vertex_stride != 12 || index_stride != 4 || material_index != page.material_index || lod_level != page.lod_level || page.blob_offset != 0 || payload_size > uint64_t(INT64_MAX) || payload_size > uint64_t(UINT32_MAX) || payload_size + PAGE_HEADER_SIZE != page.blob_length || payload_size + PAGE_HEADER_SIZE != file->get_length()) {
		ERR_PRINT(vformat("StreamedClusterMesh page header mismatch for '%s' (magic=%d version=%d header=%d stable=%d/%d revision=%d/%d hash=%d/%d vertices=%d/%d indices=%d/%d strides=%d/%d payload=%d blob=%d file=%d).", page.blob_path, magic, version, header_size, stable_id, page.stable_id, revision, page.revision, content_hash, page.content_hash, vertex_count, page.vertex_count, index_count, page.index_count, vertex_stride, index_stride, payload_size, page.blob_length, file->get_length()));
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return PackedByteArray();
	}
	PackedByteArray payload;
	const Error resize_error = payload.resize(int64_t(payload_size));
	const uint64_t bytes_read = resize_error == OK ? file->get_buffer(payload.ptrw(), payload_size) : 0;
	const uint64_t decoded_hash = resize_error == OK && bytes_read == payload_size ? (_hash_bytes(payload.ptr(), payload.size()) & uint64_t(INT64_MAX)) : 0;
	if (resize_error != OK || bytes_read != payload_size || decoded_hash != content_hash) {
		ERR_PRINT(vformat("StreamedClusterMesh page payload mismatch for '%s' (resize=%d read=%d/%d hash=%d/%d).", page.blob_path, resize_error, bytes_read, payload_size, decoded_hash, content_hash));
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return PackedByteArray();
	}
	return payload;
}

Error StreamedClusterMesh::build_from_arrays(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices, uint32_t p_material_index, uint32_t p_max_triangles_per_cluster, uint32_t p_clusters_per_group) {
	ERR_FAIL_COND_V_MSG(p_vertices.is_empty() || p_indices.is_empty() || p_indices.size() % 3 != 0, ERR_INVALID_PARAMETER, "StreamedClusterMesh requires indexed triangle geometry.");
	ERR_FAIL_COND_V_MSG(p_vertices.size() > int64_t(UINT32_MAX), ERR_INVALID_PARAMETER, "StreamedClusterMesh vertex count exceeds uint32 indices.");
	ERR_FAIL_COND_V_MSG(p_max_triangles_per_cluster < 4 || p_max_triangles_per_cluster > 256 || p_max_triangles_per_cluster % 4 != 0, ERR_INVALID_PARAMETER, "Cluster triangle count must be a multiple of 4 in [4, 256].");
	ERR_FAIL_COND_V_MSG(p_clusters_per_group == 0 || p_clusters_per_group > 64, ERR_INVALID_PARAMETER, "Cluster group size must be in [1, 64].");
	ERR_FAIL_COND_V_MSG(uint64_t(p_vertices.size()) > uint64_t(SIZE_MAX) || uint64_t(p_indices.size()) > uint64_t(SIZE_MAX), ERR_INVALID_DATA, "StreamedClusterMesh input exceeds addressable allocation sizes.");
	const size_t vertex_count = size_t(p_vertices.size());
	const size_t index_count = size_t(p_indices.size());
	size_t position_count = 0;
	ERR_FAIL_COND_V_MSG(!_checked_size_product(vertex_count, 3, position_count), ERR_INVALID_DATA, "StreamedClusterMesh position count overflows the allocation limit.");
	ERR_FAIL_COND_V_MSG(!_can_allocate_vector<float>(position_count) || !_can_allocate_vector<unsigned int>(index_count), ERR_INVALID_DATA, "StreamedClusterMesh input allocations exceed the supported addressable range.");
	for (int64_t i = 0; i < p_vertices.size(); i++) {
		ERR_FAIL_COND_V_MSG(!p_vertices[i].is_finite(), ERR_INVALID_DATA, "StreamedClusterMesh source positions must be finite.");
	}

	Vector<float> positions;
	ERR_FAIL_COND_V(positions.resize(int64_t(position_count)) != OK, ERR_OUT_OF_MEMORY);
	for (int64_t i = 0; i < p_vertices.size(); i++) {
		positions.write[i * 3 + 0] = float(p_vertices[i].x);
		positions.write[i * 3 + 1] = float(p_vertices[i].y);
		positions.write[i * 3 + 2] = float(p_vertices[i].z);
	}
	Vector<unsigned int> indices;
	ERR_FAIL_COND_V(indices.resize(int64_t(index_count)) != OK, ERR_OUT_OF_MEMORY);
	for (int64_t i = 0; i < p_indices.size(); i++) {
		ERR_FAIL_COND_V_MSG(p_indices[i] < 0 || p_indices[i] >= p_vertices.size(), ERR_INVALID_DATA, "StreamedClusterMesh index is out of range.");
		indices.write[i] = uint32_t(p_indices[i]);
	}

	constexpr size_t max_cluster_vertices = 64;
	const size_t meshlet_bound = meshopt_buildMeshletsBound(index_count, max_cluster_vertices, p_max_triangles_per_cluster);
	ERR_FAIL_COND_V_MSG(meshlet_bound == 0 || meshlet_bound > size_t(INT64_MAX) || meshlet_bound > size_t(UINT32_MAX), ERR_INVALID_DATA, "StreamedClusterMesh meshlet bound is not representable by the cluster page format.");
	Vector<meshopt_Meshlet> meshlets;
	Vector<unsigned int> meshlet_vertices;
	Vector<unsigned char> meshlet_triangles;
	// meshoptimizer documents index_count as the worst-case capacity for both
	// output index streams; meshlet_bound * cluster limits can be undersized.
	ERR_FAIL_COND_V(!_can_allocate_vector<meshopt_Meshlet>(meshlet_bound) || !_can_allocate_vector<unsigned int>(index_count) || !_can_allocate_vector<unsigned char>(index_count), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(meshlets.resize(int64_t(meshlet_bound)) != OK || meshlet_vertices.resize(int64_t(index_count)) != OK || meshlet_triangles.resize(int64_t(index_count)) != OK, ERR_OUT_OF_MEMORY);
	const size_t meshlet_count = meshopt_buildMeshlets(meshlets.ptrw(), meshlet_vertices.ptrw(), meshlet_triangles.ptrw(), indices.ptr(), index_count, positions.ptr(), vertex_count, sizeof(float) * 3, max_cluster_vertices, p_max_triangles_per_cluster, 0.0f);
	ERR_FAIL_COND_V(meshlet_count == 0 || meshlet_count > meshlet_bound || meshlet_count > size_t(UINT32_MAX), ERR_CANT_CREATE);
	ERR_FAIL_COND_V(meshlets.resize(int64_t(meshlet_count)) != OK, ERR_OUT_OF_MEMORY);

	Vector<Vector<unsigned int>> fine_indices;
	ERR_FAIL_COND_V(!_can_allocate_vector<Vector<unsigned int>>(meshlet_count) || !_can_allocate_vector<unsigned int>(meshlet_count), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(fine_indices.resize(int64_t(meshlet_count)) != OK, ERR_OUT_OF_MEMORY);
	Vector<unsigned int> concatenated_indices;
	ERR_FAIL_COND_V(concatenated_indices.reserve(int64_t(index_count)) != OK, ERR_OUT_OF_MEMORY);
	Vector<unsigned int> cluster_index_counts;
	ERR_FAIL_COND_V(cluster_index_counts.resize(int64_t(meshlet_count)) != OK, ERR_OUT_OF_MEMORY);
	size_t total_cluster_index_count = 0;
	for (size_t i = 0; i < meshlet_count; i++) {
		const meshopt_Meshlet &meshlet = meshlets[i];
		size_t triangle_index_count = 0;
		size_t vertex_end = 0;
		size_t triangle_end = 0;
		ERR_FAIL_COND_V(!_checked_size_product(meshlet.triangle_count, 3, triangle_index_count) || !_checked_size_sum(meshlet.vertex_offset, meshlet.vertex_count, vertex_end) || !_checked_size_sum(meshlet.triangle_offset, triangle_index_count, triangle_end) || vertex_end > meshlet_vertices.size() || triangle_end > meshlet_triangles.size(), ERR_CANT_CREATE);
		Vector<unsigned int> &cluster = fine_indices.write[i];
		ERR_FAIL_COND_V(!_can_allocate_vector<unsigned int>(triangle_index_count), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(cluster.resize(int64_t(triangle_index_count)) != OK, ERR_OUT_OF_MEMORY);
		for (uint32_t triangle = 0; triangle < meshlet.triangle_count; triangle++) {
			for (uint32_t corner = 0; corner < 3; corner++) {
				const uint32_t local = meshlet_triangles[meshlet.triangle_offset + triangle * 3 + corner];
				ERR_FAIL_COND_V(local >= meshlet.vertex_count, ERR_CANT_CREATE);
				const unsigned int global = meshlet_vertices[meshlet.vertex_offset + local];
				ERR_FAIL_COND_V(global >= vertex_count, ERR_CANT_CREATE);
				cluster.write[triangle * 3 + corner] = global;
			}
		}
		ERR_FAIL_COND_V(!_checked_size_sum(total_cluster_index_count, triangle_index_count, total_cluster_index_count) || triangle_index_count > size_t(UINT32_MAX), ERR_CANT_CREATE);
		cluster_index_counts.write[i] = uint32_t(triangle_index_count);
		concatenated_indices.append_array(cluster);
	}
	ERR_FAIL_COND_V(total_cluster_index_count != index_count || concatenated_indices.size() != int64_t(index_count), ERR_CANT_CREATE);

	Vector<unsigned int> partitions;
	ERR_FAIL_COND_V(partitions.resize(int64_t(meshlet_count)) != OK, ERR_OUT_OF_MEMORY);
	const size_t group_count = meshopt_partitionClusters(partitions.ptrw(), concatenated_indices.ptr(), index_count, cluster_index_counts.ptr(), meshlet_count, positions.ptr(), vertex_count, sizeof(float) * 3, p_clusters_per_group);
	ERR_FAIL_COND_V(group_count == 0 || group_count > meshlet_count || group_count > size_t(UINT32_MAX), ERR_CANT_CREATE);
	Vector<size_t> group_index_counts;
	Vector<size_t> group_meshlet_counts;
	ERR_FAIL_COND_V(!_can_allocate_vector<size_t>(group_count), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(group_index_counts.resize(int64_t(group_count)) != OK || group_meshlet_counts.resize(int64_t(group_count)) != OK, ERR_OUT_OF_MEMORY);
	for (size_t group = 0; group < group_count; group++) {
		group_index_counts.write[group] = 0;
		group_meshlet_counts.write[group] = 0;
	}
	for (size_t cluster = 0; cluster < meshlet_count; cluster++) {
		const size_t group = partitions[cluster];
		ERR_FAIL_COND_V(group >= group_count, ERR_CANT_CREATE);
		ERR_FAIL_COND_V(!_checked_size_sum(group_index_counts[group], cluster_index_counts[cluster], group_index_counts.write[group]) || !_checked_size_sum(group_meshlet_counts[group], 1, group_meshlet_counts.write[group]), ERR_CANT_CREATE);
	}
	size_t page_capacity = 0;
	ERR_FAIL_COND_V(!_checked_size_sum(group_count, meshlet_count, page_capacity), ERR_INVALID_DATA);

	pages.clear();
	pending_payloads.clear();
	ERR_FAIL_COND_V(!_can_allocate_vector<Page>(page_capacity) || !_can_allocate_vector<PackedByteArray>(page_capacity), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(pages.reserve(int64_t(page_capacity)) != OK || pending_payloads.reserve(int64_t(page_capacity)) != OK, ERR_OUT_OF_MEMORY);
	for (size_t group = 0; group < group_count; group++) {
		Vector<unsigned int> group_indices;
		Vector<uint32_t> group_meshlets;
		ERR_FAIL_COND_V(!_can_allocate_vector<unsigned int>(group_index_counts[group]) || !_can_allocate_vector<uint32_t>(group_meshlet_counts[group]), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(group_indices.reserve(int64_t(group_index_counts[group])) != OK || group_meshlets.reserve(int64_t(group_meshlet_counts[group])) != OK, ERR_OUT_OF_MEMORY);
		for (size_t cluster = 0; cluster < meshlet_count; cluster++) {
			if (partitions[cluster] == group) {
				group_indices.append_array(fine_indices[cluster]);
				group_meshlets.push_back(uint32_t(cluster));
			}
		}
		ERR_CONTINUE(group_indices.is_empty());

		Vector<unsigned int> simplified;
		ERR_FAIL_COND_V(!_can_allocate_vector<unsigned int>(group_indices.size()), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(simplified.resize(group_indices.size()) != OK, ERR_OUT_OF_MEMORY);
		const size_t target_count = MAX(size_t(3), (size_t(group_indices.size()) / 2) / 3 * 3);
		float relative_error = 0.0f;
		const size_t simplified_count = meshopt_simplify(simplified.ptrw(), group_indices.ptr(), group_indices.size(), positions.ptr(), vertex_count, sizeof(float) * 3, target_count, 1.0f, meshopt_SimplifyLockBorder, &relative_error);
		ERR_FAIL_COND_V(simplified_count == 0 || simplified_count % 3 != 0 || !Math::is_finite(relative_error) || relative_error < 0.0f, ERR_CANT_CREATE);
		ERR_FAIL_COND_V(simplified_count > size_t(group_indices.size()) || simplified.resize(int64_t(simplified_count)) != OK, ERR_OUT_OF_MEMORY);

		Page coarse_page;
		PackedByteArray coarse_payload;
		const float simplification_scale = meshopt_simplifyScale(positions.ptr(), vertex_count, sizeof(float) * 3);
		const float absolute_error = relative_error * simplification_scale;
		ERR_FAIL_COND_V(!Math::is_finite(simplification_scale) || simplification_scale < 0.0f || !Math::is_finite(absolute_error) || absolute_error < 0.0f, ERR_CANT_CREATE);
		ERR_FAIL_COND_V(_build_local_page(p_vertices, simplified, p_material_index, 1, uint32_t(group), absolute_error, true, coarse_page, coarse_payload) != OK, ERR_CANT_CREATE);
		const uint64_t coarse_id = coarse_page.stable_id;

		Vector<Page> group_fine_pages;
		Vector<PackedByteArray> group_fine_payloads;
		ERR_FAIL_COND_V(!_can_allocate_vector<Page>(group_meshlets.size()) || !_can_allocate_vector<PackedByteArray>(group_meshlets.size()) || !_can_allocate_vector<int64_t>(group_meshlets.size()), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(group_fine_pages.reserve(group_meshlets.size()) != OK || group_fine_payloads.reserve(group_meshlets.size()) != OK || coarse_page.child_ids.reserve(group_meshlets.size()) != OK, ERR_OUT_OF_MEMORY);
		AABB group_bounds;
		bool have_group_bounds = false;
		for (uint32_t cluster : group_meshlets) {
			Page fine_page;
			PackedByteArray fine_payload;
			ERR_FAIL_COND_V(_build_local_page(p_vertices, fine_indices[cluster], p_material_index, 0, cluster, 0.0f, false, fine_page, fine_payload) != OK, ERR_CANT_CREATE);
			group_bounds = have_group_bounds ? group_bounds.merge(fine_page.bounds) : fine_page.bounds;
			have_group_bounds = true;
			fine_page.parent_id = coarse_id;
			coarse_page.child_ids.push_back(int64_t(fine_page.stable_id));
			group_fine_pages.push_back(fine_page);
			group_fine_payloads.push_back(fine_payload);
		}
		ERR_FAIL_COND_V(!have_group_bounds, ERR_CANT_CREATE);
		// Selection bounds must remain conservative even when simplification removes
		// an interior extremum that is not part of the locked group border.
		ERR_FAIL_COND_V(!_make_conservative_bounds(group_bounds, coarse_page.bounds), ERR_CANT_CREATE);
		pages.push_back(coarse_page);
		pending_payloads.push_back(coarse_payload);
		for (int i = 0; i < group_fine_pages.size(); i++) {
			pages.push_back(group_fine_pages[i]);
			pending_payloads.push_back(group_fine_payloads[i]);
		}
	}

	ERR_FAIL_COND_V(pages.is_empty() || get_persistent_page_count() == 0, ERR_CANT_CREATE);
	HashSet<uint64_t> stable_ids;
	for (const Page &page : pages) {
		ERR_FAIL_COND_V_MSG(stable_ids.has(page.stable_id), ERR_CANT_CREATE, "StreamedClusterMesh generated a stable page ID collision.");
		stable_ids.insert(page.stable_id);
	}
	const PackedByteArray source_payload = _pack_page(p_vertices, p_indices);
	ERR_FAIL_COND_V(source_payload.is_empty(), ERR_OUT_OF_MEMORY);
	source_hash = _hash_bytes(source_payload.ptr(), source_payload.size()) & uint64_t(INT64_MAX);
	format_version = FORMAT_VERSION;
	total_blob_bytes = 0;
	maximum_blob_bytes = 0;
	for (const PackedByteArray &payload : pending_payloads) {
		const uint64_t blob_size = PAGE_HEADER_SIZE + uint64_t(payload.size());
		ERR_FAIL_COND_V_MSG(blob_size > uint64_t(UINT32_MAX), ERR_OUT_OF_MEMORY, "StreamedClusterMesh generated a page larger than the uint32 GPU-buffer limit.");
		ERR_FAIL_COND_V_MSG(total_blob_bytes > UINT64_MAX - blob_size, ERR_OUT_OF_MEMORY, "StreamedClusterMesh total page bytes overflow the resource limit.");
		total_blob_bytes += blob_size;
		maximum_blob_bytes = MAX(maximum_blob_bytes, blob_size);
	}
	emit_changed();
	return OK;
}

Error StreamedClusterMesh::save_cache(const String &p_manifest_path) {
	ERR_FAIL_COND_V_MSG(format_version != FORMAT_VERSION || pages.is_empty() || pending_payloads.size() != pages.size(), ERR_INVALID_DATA, "StreamedClusterMesh has no unsaved generated page data.");
	const String page_directory = p_manifest_path.get_basename() + ".pages";
	const Error directory_error = DirAccess::make_dir_recursive_absolute(page_directory);
	ERR_FAIL_COND_V_MSG(directory_error != OK && directory_error != ERR_ALREADY_EXISTS, directory_error, "Could not create StreamedClusterMesh page directory.");

	for (int i = 0; i < pages.size(); i++) {
		Page &page = pages.write[i];
		const PackedByteArray &payload = pending_payloads[i];
		const String page_path = page_directory.path_join(String::num_uint64(page.stable_id, 16).pad_zeros(16) + ".scpage");
		Error open_error = OK;
		Ref<FileAccess> file = FileAccess::open(page_path, FileAccess::WRITE, &open_error);
		ERR_FAIL_COND_V_MSG(file.is_null(), open_error, "Could not create StreamedClusterMesh page blob.");
		file->store_32(PAGE_MAGIC);
		file->store_32(FORMAT_VERSION);
		file->store_32(PAGE_HEADER_SIZE);
		file->store_32(page.persistent ? 1 : 0);
		file->store_64(page.stable_id);
		file->store_64(page.revision);
		file->store_32(page.vertex_count);
		file->store_32(page.index_count);
		file->store_32(12);
		file->store_32(4);
		file->store_32(page.material_index);
		file->store_32(page.lod_level);
		file->store_64(payload.size());
		file->store_64(page.content_hash);
		file->store_buffer(payload.ptr(), payload.size());
		const Error write_error = file->get_error();
		ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Could not write a complete StreamedClusterMesh page blob.");
		page.blob_path = page_path;
		page.blob_offset = 0;
		page.blob_length = PAGE_HEADER_SIZE + uint64_t(payload.size());
	}

	const Error save_error = ResourceSaver::save(this, p_manifest_path);
	ERR_FAIL_COND_V(save_error != OK, save_error);
	pending_payloads.clear();
	return OK;
}

void StreamedClusterMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_format_version", "version"), &StreamedClusterMesh::set_format_version);
	ClassDB::bind_method(D_METHOD("get_format_version"), &StreamedClusterMesh::get_format_version);
	ClassDB::bind_method(D_METHOD("set_source_hash", "source_hash"), &StreamedClusterMesh::set_source_hash);
	ClassDB::bind_method(D_METHOD("get_source_hash"), &StreamedClusterMesh::get_source_hash);
	ClassDB::bind_method(D_METHOD("set_pages", "pages"), &StreamedClusterMesh::set_pages);
	ClassDB::bind_method(D_METHOD("get_pages"), &StreamedClusterMesh::get_pages);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &StreamedClusterMesh::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &StreamedClusterMesh::get_materials);
	ClassDB::bind_method(D_METHOD("get_page_count"), &StreamedClusterMesh::get_page_count);
	ClassDB::bind_method(D_METHOD("get_persistent_page_count"), &StreamedClusterMesh::get_persistent_page_count);
	ClassDB::bind_method(D_METHOD("get_total_blob_bytes"), &StreamedClusterMesh::get_total_blob_bytes);
	ClassDB::bind_method(D_METHOD("get_maximum_blob_bytes"), &StreamedClusterMesh::get_maximum_blob_bytes);
	ClassDB::bind_method(D_METHOD("load_page_payload", "page"), &StreamedClusterMesh::load_page_payload_bind);
	ClassDB::bind_method(D_METHOD("build_from_arrays", "vertices", "indices", "material_index", "max_triangles_per_cluster", "clusters_per_group"), &StreamedClusterMesh::build_from_arrays, DEFVAL(0), DEFVAL(124), DEFVAL(8));
	ClassDB::bind_method(D_METHOD("save_cache", "manifest_path"), &StreamedClusterMesh::save_cache);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "format_version", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_READ_ONLY), "set_format_version", "get_format_version");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_READ_ONLY), "set_source_hash", "get_source_hash");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "pages", PROPERTY_HINT_ARRAY_TYPE, "Dictionary", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_READ_ONLY), "set_pages", "get_pages");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, "Material", PROPERTY_USAGE_STORAGE), "set_materials", "get_materials");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "page_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_page_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "persistent_page_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_persistent_page_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "total_blob_bytes", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_total_blob_bytes");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "maximum_blob_bytes", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_maximum_blob_bytes");
}
