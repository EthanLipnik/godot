/**************************************************************************/
/*  virtual_geometry_storage.h                                            */
/**************************************************************************/

#pragma once

#include "servers/rendering/virtual_geometry/virtual_geometry_format.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_raster.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_ray.h"

#include "core/os/mutex.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_device.h"

namespace RendererVirtualGeometry {

// A non-relocating, first-fit heap used for virtual geometry streams.  Its
// addresses are published to GPU descriptors, therefore a successful
// allocation is stable until free().
class VirtualGeometryHeap {
public:
	struct Allocation {
		uint64_t offset = 0;
		uint64_t size = 0;
		bool is_valid() const { return size != 0; }
	};
	struct Diagnostics {
		uint64_t capacity = 0;
		uint64_t used = 0;
		uint64_t free = 0;
		uint64_t largest_free_block = 0;
		uint32_t free_block_count = 0;
		uint64_t fragmentation_bytes = 0;
	};

	VirtualGeometryHeap() = default;
	explicit VirtualGeometryHeap(uint64_t p_capacity) { reset(p_capacity); }
	void reset(uint64_t p_capacity);
	bool allocate(uint64_t p_size, uint64_t p_alignment, Allocation &r_allocation);
	bool free(const Allocation &p_allocation);
	Diagnostics get_diagnostics() const;

private:
	struct Block { uint64_t offset = 0; uint64_t size = 0; bool free = true; };
	Vector<Block> blocks;
	uint64_t capacity = 0;
};

enum class VirtualGeometryPageState : uint8_t {
	UNLOADED, REQUESTED, IO_PENDING, DECODING, UPLOAD_PENDING, RESIDENT,
	DESCRIPTOR_PENDING, ACTIVE, RETIRING, FAILED,
};

enum class VirtualGeometryRequestReason : uint8_t {
	PERSISTENT_ROOT, RASTER_VIEW, STEREO_UNION, TRANSPORT, PREDICTION, EDITOR_PIN,
};

struct VirtualGeometryBudgets {
	uint64_t compressed_cpu_bytes = 64 * 1024 * 1024;
	uint64_t decoded_cpu_bytes = 64 * 1024 * 1024;
	uint64_t position_heap_bytes = 128 * 1024 * 1024;
	uint64_t index_heap_bytes = 64 * 1024 * 1024;
	uint64_t attribute_heap_bytes = 128 * 1024 * 1024;
	uint64_t upload_bytes_per_frame = 16 * 1024 * 1024;
	uint32_t io_tasks_per_frame = 8;
	uint32_t decode_tasks_per_frame = 8;
};

struct VirtualGeometryPageDiagnostics {
	VirtualGeometryPageState state = VirtualGeometryPageState::UNLOADED;
	uint64_t generation = 0;
	uint64_t request_count = 0;
	uint64_t failure_count = 0;
	uint64_t upload_completion_serial = 0;
	uint64_t last_raster_serial = 0;
	uint64_t last_ray_serial = 0;
	uint64_t dependency_serial = 0;
	uint64_t last_interest_serial = 0;
	uint32_t priority = 0;
	bool persistent = false;
	String failure;
};

struct VirtualGeometryRuntimeDiagnostics {
	uint64_t resource_revision = 0;
	uint64_t active_pages = 0;
	uint64_t requested_pages = 0;
	uint64_t failed_pages = 0;
	uint64_t compressed_bytes = 0;
	uint64_t decoded_bytes = 0;
	uint64_t upload_bytes = 0;
	uint64_t cancelled_completions = 0;
	uint64_t coarse_fallback_exposures = 0;
	uint64_t budget_deferred_pages = 0;
	uint64_t evicted_pages = 0;
	uint64_t descriptor_publications = 0;
	VirtualGeometryHeap::Diagnostics positions;
	VirtualGeometryHeap::Diagnostics indices;
	VirtualGeometryHeap::Diagnostics attributes;
};

struct VirtualGeometryGPUClusterDescriptor {
	uint64_t stable_id = 0;
	uint64_t position_offset = 0;
	uint64_t index_offset = 0;
	uint64_t attribute_offset = 0;
	uint32_t index_count = 0;
	uint32_t material_slot = UINT32_MAX;
	uint32_t generation = 0;
	uint32_t flags = 0;
	uint32_t base_vertex = 0;
	uint32_t first_index = 0;
	uint32_t source_stream_flags = 0;
	uint32_t attribute_stride = 0;
};

struct VirtualGeometryGPUVertexAttributes {
	// Flux's ordinary scene shader consumes location 1 as the uncompressed
	// octahedral normal/tangent record, not as raw XYZ vectors.
	float axis_tangent[4] = { 0.5f, 1.0f, 1.0f, 0.75f };
	float uv0[2] = {};
	float uv1[2] = {};
	float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	uint32_t joints[4] = {};
	float weights[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	uint32_t source_stream_flags = 0;
};
static_assert(offsetof(VirtualGeometryGPUVertexAttributes, axis_tangent) == 0);
static_assert(offsetof(VirtualGeometryGPUVertexAttributes, uv0) == 16);
static_assert(offsetof(VirtualGeometryGPUVertexAttributes, uv1) == 24);
static_assert(offsetof(VirtualGeometryGPUVertexAttributes, color) == 32);
static_assert(offsetof(VirtualGeometryGPUVertexAttributes, joints) == 48);
static_assert(offsetof(VirtualGeometryGPUVertexAttributes, weights) == 64);
static_assert(sizeof(VirtualGeometryGPUVertexAttributes) == 84);

// Renderer-owned data only. Worker code may call decode_page_on_worker() and
// enqueue_worker_completion(); render_process() is the sole method permitted
// to allocate/publish GPU-facing addresses or activate descriptor records.
class VirtualGeometryStorage {
public:
	VirtualGeometryStorage();
	~VirtualGeometryStorage();
	void set_budgets(const VirtualGeometryBudgets &p_budgets);
	void set_rendering_device(RenderingDevice *p_rendering_device) { rendering_device = p_rendering_device; }
	const VirtualGeometryBudgets &get_budgets() const { return budgets; }
	Error set_package(const Package &p_package, uint64_t p_resource_revision);
	uint64_t get_resource_revision() const { return resource_revision; }
	const Package &get_package() const { return package; }

	Error request_page(uint64_t p_page_id, VirtualGeometryRequestReason p_reason, uint32_t p_priority = 0);
	Vector<uint64_t> take_io_requests(uint32_t p_maximum);
	// Schedules the portable IO/decode stage. It never touches RD objects; the
	// result is only consumed by render_process() at a frame boundary.
	void start_worker_decode_tasks(const Vector<uint64_t> &p_page_ids);
	// This performs only portable decompression/hash/range validation. It must be
	// called from a worker, never from the render thread.
	Error decode_page_on_worker(uint64_t p_page_id, uint64_t p_generation, PackedByteArray &r_decoded, String &r_error) const;
	void enqueue_worker_completion(uint64_t p_page_id, uint64_t p_generation, const PackedByteArray &p_decoded, Error p_result, const String &p_error = String());
	// Render-thread boundary. p_pending_submission_serial must come from
	// RenderingDevice::get_pending_submission_serial().
	void render_process(uint64_t p_completed_submission_serial, uint64_t p_pending_submission_serial);
	void notify_submission_completed(uint64_t p_completed_submission_serial);

	void mark_page_used(uint64_t p_page_id, uint64_t p_raster_serial, uint64_t p_ray_serial, uint64_t p_dependency_serial = 0);
	void mark_raster_interest(uint64_t p_page_id, uint64_t p_submission_serial, uint32_t p_priority);
	// The horizon is measured in RD submission serials, not rendered frames. A
	// normal Flux frame owns several submissions, so it must span more than one
	// frame to avoid evict/reload oscillation while still coarsening promptly.
	void evict_stale_pages(uint64_t p_completed_submission_serial, uint64_t p_interest_horizon = 32);
	Error retire_page(uint64_t p_page_id);
	void process_retirements(uint64_t p_completed_submission_serial);

	VirtualGeometryPageState get_page_state(uint64_t p_page_id) const;
	VirtualGeometryPageDiagnostics get_page_diagnostics(uint64_t p_page_id) const;
	VirtualGeometryRuntimeDiagnostics get_diagnostics() const;
	// Becomes true only when a renderer binds the selected page heaps to an
	// actual Flux primary draw pass. VG3 currently exposes the reference
	// selection contract, not a synthetic render-success claim.
	bool is_raster_integration_enabled() const { return raster_integration_ready; }
	RID get_position_heap_rid() const { return position_heap_rid; }
	RID get_index_heap_rid() const { return index_heap_rid; }
	RID get_attribute_heap_rid() const { return attribute_heap_rid; }
	RID get_cluster_descriptor_rid() const { return descriptor_heap_rid; }
	RID get_position_vertex_buffer_rid() const { return position_heap_rid; }
	RID get_attribute_vertex_buffer_rid() const { return attribute_heap_rid; }
	RID get_index_buffer_rid() const { return index_heap_rid; }
	// Whole-heap index view used by VG3 indirect commands. Individual command
	// records provide first_index/index_count; no page creates an RD object.
	RID get_index_array_rid();
	uint32_t get_position_vertex_count() const { return uint32_t(MIN(budgets.position_heap_bytes / 12, budgets.attribute_heap_bytes / sizeof(VirtualGeometryGPUVertexAttributes))); }
	uint32_t get_attribute_stride() const { return sizeof(VirtualGeometryGPUVertexAttributes); }
	uint64_t get_active_descriptor_generation() const { return active_descriptor_generation; }
	const Vector<VirtualGeometryGPUClusterDescriptor> &get_gpu_cluster_descriptors() const { return gpu_descriptors; }
	const uint32_t *get_gpu_cluster_descriptor_slot(uint64_t p_cluster_id) const { return cluster_slots.getptr(p_cluster_id); }
	const VirtualGeometryGPUClusterDescriptor *get_gpu_cluster_descriptor(uint64_t p_cluster_id) const;
	const ClusterDescriptor *get_cluster_descriptor(uint64_t p_cluster_id) const { return _cluster(p_cluster_id); }
	const Vector<RayGroupDescriptor> &get_ray_group_descriptors() const { return package.manifest.ray_groups; }
	// Selection consumes renderer-owned active page records only. It does not
	// materialize ArrayMesh objects, RIDs, or scene nodes for pages/clusters.
	VirtualGeometryRasterSelection select_raster_reference(const VirtualGeometryRasterSelectionInput &p_input, VirtualGeometryRasterSelectionState *p_state = nullptr) const;
	const HashSet<uint64_t> &get_active_cluster_ids() const;

private:
	struct PageRuntime {
		VirtualGeometryPageState state = VirtualGeometryPageState::UNLOADED;
		uint64_t generation = 1;
		uint64_t request_count = 0;
		uint32_t priority = 0;
		VirtualGeometryRequestReason reason = VirtualGeometryRequestReason::RASTER_VIEW;
		uint64_t upload_completion_serial = 0;
		uint64_t last_raster_serial = 0;
		uint64_t last_ray_serial = 0;
		uint64_t dependency_serial = 0;
		uint64_t last_interest_serial = 0;
		uint64_t failure_count = 0;
		String failure;
		// Position and attribute buffers share one logical vertex ordinal. This is
		// required because indexed vertex_offset applies to every vertex binding.
		VirtualGeometryHeap::Allocation vertices, positions, attributes, indices;
		Vector<VirtualGeometryGPUClusterDescriptor> descriptors;
		PackedByteArray position_upload, index_upload, attribute_upload;
	};
	struct WorkerCompletion { uint64_t page_id = 0; uint64_t generation = 0; PackedByteArray decoded; Error result = OK; String error; };
	struct WorkerDecodeTask { VirtualGeometryStorage *storage = nullptr; uint64_t page_id = 0; uint64_t generation = 0; };
	static void _decode_worker_entry(void *p_data);
	void _wait_for_worker_tasks();
	void _reap_completed_worker_tasks();
	const PageDescriptor *_page(uint64_t p_page_id) const;
	const ClusterDescriptor *_cluster(uint64_t p_cluster_id) const;
	bool _allocate_page(uint64_t p_page_id, const PackedByteArray &p_decoded, uint64_t p_completion_serial, String &r_error);
	bool _ensure_rd_heaps(String &r_error);
	bool _parse_page_upload(uint64_t p_page_id, const PackedByteArray &p_decoded, PageRuntime &r_runtime, String &r_error);
	bool _enqueue_rd_upload(PageRuntime &r_runtime, String &r_error);
	bool _publish_descriptor_snapshot(uint64_t p_submission_serial, String &r_error);
	void _free_rd_heaps();
	bool _group_fine_complete(const RefinementGroupDescriptor &p_group) const;
	void _activate_ready_groups(uint64_t p_completed_submission_serial);
	bool _has_active_coarse_coverage(uint64_t p_page_id) const;
	void _free_page(PageRuntime &r_page);

	Package package;
	HashMap<uint64_t, int> page_indices;
	HashMap<uint64_t, PageRuntime> pages;
	VirtualGeometryBudgets budgets;
	VirtualGeometryHeap vertex_slot_heap, index_heap;
	RenderingDevice *rendering_device = nullptr;
	RID position_heap_rid, index_heap_rid, attribute_heap_rid, descriptor_heap_rid;
	RID pending_descriptor_heap_rid;
	RID index_array_rid;
	Vector<VirtualGeometryGPUClusterDescriptor> gpu_descriptors;
	Vector<VirtualGeometryGPUClusterDescriptor> staged_gpu_descriptors;
	Vector<VirtualGeometryGPUClusterDescriptor> pending_gpu_descriptors;
	HashMap<uint64_t, uint32_t> cluster_slots;
	HashSet<uint64_t> active_cluster_ids;
	uint64_t pending_descriptor_generation = 0;
	uint64_t active_descriptor_generation = 0;
	uint64_t descriptor_publication_serial = 0;
	uint32_t next_descriptor_generation = 1;
	bool descriptor_snapshot_dirty = false;
	bool raster_integration_ready = false;
	uint64_t resource_revision = 0;
	uint64_t decoded_bytes = 0;
	uint64_t upload_bytes = 0;
	uint64_t cancelled_completions = 0;
	uint64_t coarse_fallback_exposures = 0;
	uint64_t budget_deferred_pages = 0;
	uint64_t evicted_pages = 0;
	uint64_t descriptor_publications = 0;
	mutable Mutex completion_mutex;
	Vector<WorkerCompletion> worker_completions;
	Mutex worker_task_mutex;
	Vector<WorkerThreadPool::TaskID> worker_task_ids;
};

} // namespace RendererVirtualGeometry
