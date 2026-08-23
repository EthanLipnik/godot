/**************************************************************************/
/*  streamed_cluster_mesh.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/variant/typed_array.h"
#include "scene/resources/material.h"

class StreamedClusterMesh : public Resource {
	GDCLASS(StreamedClusterMesh, Resource);

public:
	static constexpr uint32_t FORMAT_VERSION = 1;
	static constexpr uint32_t PAGE_MAGIC = 0x504c4353; // "SCLP" in little endian.
	static constexpr uint32_t PAGE_HEADER_SIZE = 72;

	struct Page {
		uint64_t stable_id = 0;
		uint64_t revision = 0;
		uint64_t content_hash = 0;
		AABB bounds;
		float geometric_error = 0.0f;
		uint32_t material_index = 0;
		uint32_t lod_level = 0;
		uint32_t vertex_count = 0;
		uint32_t index_count = 0;
		uint64_t parent_id = 0;
		PackedInt64Array child_ids;
		String blob_path;
		uint64_t blob_offset = 0;
		uint64_t blob_length = 0;
		bool persistent = false;
	};

private:
	uint32_t format_version = FORMAT_VERSION;
	uint64_t source_hash = 0;
	Vector<Page> pages;
	Vector<PackedByteArray> pending_payloads;
	Vector<Ref<Material>> materials;
	uint64_t total_blob_bytes = 0;
	uint64_t maximum_blob_bytes = 0;

	static uint64_t _hash_bytes(const uint8_t *p_data, uint64_t p_size, uint64_t p_seed = 1469598103934665603ull);
	static uint64_t _make_stable_id(uint64_t p_content_hash, uint32_t p_lod_level, uint32_t p_group);
	static PackedByteArray _pack_page(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices);
	static Error _build_local_page(const PackedVector3Array &p_source_vertices, const Vector<unsigned int> &p_global_indices, uint32_t p_material_index, uint32_t p_lod_level, uint32_t p_group, float p_error, bool p_persistent, Page &r_page, PackedByteArray &r_payload);
	Dictionary _page_to_dictionary(const Page &p_page) const;
	Error _page_from_dictionary(const Dictionary &p_dictionary, Page &r_page) const;

protected:
	static void _bind_methods();

public:
	void set_format_version(uint32_t p_version) { format_version = p_version; }
	uint32_t get_format_version() const { return format_version; }
	void set_source_hash(uint64_t p_hash) { source_hash = p_hash; }
	uint64_t get_source_hash() const { return source_hash; }
	void set_pages(const Array &p_pages);
	Array get_pages() const;
	void set_materials(const TypedArray<Material> &p_materials);
	TypedArray<Material> get_materials() const;

	int get_page_count() const { return pages.size(); }
	int get_persistent_page_count() const;
	uint64_t get_total_blob_bytes() const { return total_blob_bytes; }
	uint64_t get_maximum_blob_bytes() const { return maximum_blob_bytes; }
	const Page *get_page(int p_index) const;
	int find_page(uint64_t p_stable_id) const;
	PackedByteArray load_page_payload(int p_page, Error *r_error = nullptr) const;
	PackedByteArray load_page_payload_bind(int p_page) const { return load_page_payload(p_page); }

	Error build_from_arrays(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices, uint32_t p_material_index = 0, uint32_t p_max_triangles_per_cluster = 124, uint32_t p_clusters_per_group = 8);
	Error save_cache(const String &p_manifest_path);
};
