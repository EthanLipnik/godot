/**************************************************************************/
/*  virtual_geometry_storage.cpp                                          */
/**************************************************************************/

#include "virtual_geometry_storage.h"

#include "core/error/error_macros.h"

namespace RendererVirtualGeometry {

static bool _align_up(uint64_t p_value, uint64_t p_alignment, uint64_t &r_value) {
	if (p_alignment == 0 || (p_alignment & (p_alignment - 1)) != 0 || p_value > UINT64_MAX - (p_alignment - 1)) return false;
	r_value = (p_value + p_alignment - 1) & ~(p_alignment - 1);
	return true;
}

void VirtualGeometryHeap::reset(uint64_t p_capacity) {
	capacity = p_capacity;
	blocks.clear();
	if (capacity) blocks.push_back({ 0, capacity, true });
}

bool VirtualGeometryHeap::allocate(uint64_t p_size, uint64_t p_alignment, Allocation &r_allocation) {
	r_allocation = Allocation();
	if (p_size == 0) return false;
	for (int i = 0; i < blocks.size(); i++) {
		Block block = blocks[i];
		uint64_t aligned = 0, end = 0;
		if (!block.free || !_align_up(block.offset, p_alignment, aligned) || !checked_add_u64(aligned, p_size, end) || end > block.offset + block.size) continue;
		Vector<Block> replacement;
		if (aligned > block.offset) replacement.push_back({ block.offset, aligned - block.offset, true });
		replacement.push_back({ aligned, p_size, false });
		if (end < block.offset + block.size) replacement.push_back({ end, block.offset + block.size - end, true });
		blocks.remove_at(i);
		for (int b = replacement.size() - 1; b >= 0; b--) blocks.insert(i, replacement[b]);
		r_allocation = { aligned, p_size };
		return true;
	}
	return false;
}

bool VirtualGeometryHeap::free(const Allocation &p_allocation) {
	if (!p_allocation.is_valid()) return false;
	for (int i = 0; i < blocks.size(); i++) {
		if (blocks[i].offset != p_allocation.offset || blocks[i].size != p_allocation.size || blocks[i].free) continue;
		blocks.write[i].free = true;
		if (i > 0 && blocks[i - 1].free) { blocks.write[i - 1].size += blocks[i].size; blocks.remove_at(i); i--; }
		if (i + 1 < blocks.size() && blocks[i + 1].free) { blocks.write[i].size += blocks[i + 1].size; blocks.remove_at(i + 1); }
		return true;
	}
	return false;
}

VirtualGeometryHeap::Diagnostics VirtualGeometryHeap::get_diagnostics() const {
	Diagnostics result; result.capacity = capacity;
	for (const Block &block : blocks) if (block.free) { result.free += block.size; result.largest_free_block = MAX(result.largest_free_block, block.size); result.free_block_count++; }
	result.used = capacity - result.free;
	result.fragmentation_bytes = result.free > result.largest_free_block ? result.free - result.largest_free_block : 0;
	return result;
}

VirtualGeometryStorage::VirtualGeometryStorage() { set_budgets(budgets); }

VirtualGeometryStorage::~VirtualGeometryStorage() {
	_wait_for_worker_tasks();
	_free_rd_heaps();
}

void VirtualGeometryStorage::_free_rd_heaps() {
	if (!rendering_device) return;
	for (RID rid : { index_array_rid, position_heap_rid, index_heap_rid, attribute_heap_rid, descriptor_heap_rid, pending_descriptor_heap_rid }) if (rid.is_valid()) rendering_device->free_rid(rid);
	index_array_rid = RID(); position_heap_rid = RID(); index_heap_rid = RID(); attribute_heap_rid = RID(); descriptor_heap_rid = RID(); pending_descriptor_heap_rid = RID();
	raster_integration_ready = false;
}

RID VirtualGeometryStorage::get_index_array_rid() {
	if (index_array_rid.is_valid()) return index_array_rid;
	if (!rendering_device || !index_heap_rid.is_valid()) return RID();
	index_array_rid = rendering_device->index_array_create(index_heap_rid, 0, uint32_t(budgets.index_heap_bytes / sizeof(uint32_t)));
	return index_array_rid;
}

bool VirtualGeometryStorage::_ensure_rd_heaps(String &r_error) {
	if (!rendering_device) return true; // CPU mock backend retains upload plans for deterministic tests.
	if (position_heap_rid.is_valid()) return true;
	if (budgets.position_heap_bytes > UINT32_MAX || budgets.index_heap_bytes > UINT32_MAX || budgets.attribute_heap_bytes > UINT32_MAX || uint64_t(package.manifest.clusters.size()) * sizeof(VirtualGeometryGPUClusterDescriptor) > UINT32_MAX) { r_error = "Virtual geometry RD heap exceeds RenderingDevice's uint32 buffer limit."; return false; }
	// These are whole heaps, not per-page buffers. AS_STORAGE makes the same
	// vertex/index allocations readable through storage descriptors while their
	// native buffer classes remain legal vertex_array/index_array inputs.
	position_heap_rid = rendering_device->vertex_buffer_create(uint32_t(budgets.position_heap_bytes), {}, RenderingDevice::BUFFER_CREATION_AS_STORAGE_BIT);
	attribute_heap_rid = rendering_device->vertex_buffer_create(uint32_t(budgets.attribute_heap_bytes), {}, RenderingDevice::BUFFER_CREATION_AS_STORAGE_BIT);
	index_heap_rid = rendering_device->index_buffer_create(uint32_t(budgets.index_heap_bytes / 4), RenderingDevice::INDEX_BUFFER_FORMAT_UINT32, {}, false, RenderingDevice::BUFFER_CREATION_AS_STORAGE_BIT);
	const uint32_t descriptor_bytes = MAX(uint32_t(1), uint32_t(package.manifest.clusters.size() * sizeof(VirtualGeometryGPUClusterDescriptor)));
	descriptor_heap_rid = rendering_device->storage_buffer_create(descriptor_bytes);
	pending_descriptor_heap_rid = rendering_device->storage_buffer_create(descriptor_bytes);
	if (!position_heap_rid.is_valid() || !index_heap_rid.is_valid() || !attribute_heap_rid.is_valid() || !descriptor_heap_rid.is_valid() || !pending_descriptor_heap_rid.is_valid()) { r_error = "RenderingDevice could not create virtual geometry heap buffers."; _free_rd_heaps(); return false; }
	return true;
}

void VirtualGeometryStorage::_wait_for_worker_tasks() {
	Vector<WorkerThreadPool::TaskID> tasks;
	{ MutexLock lock(worker_task_mutex); SWAP(tasks, worker_task_ids); }
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (!pool) return;
	for (WorkerThreadPool::TaskID task : tasks) if (task >= 0) pool->wait_for_task_completion(task);
}

void VirtualGeometryStorage::_reap_completed_worker_tasks() {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (!pool) return;
	Vector<WorkerThreadPool::TaskID> completed;
	{ MutexLock lock(worker_task_mutex); for (int i = worker_task_ids.size() - 1; i >= 0; i--) if (pool->is_task_completed(worker_task_ids[i])) { completed.push_back(worker_task_ids[i]); worker_task_ids.remove_at(i); } }
	for (WorkerThreadPool::TaskID task : completed) pool->wait_for_task_completion(task);
}

void VirtualGeometryStorage::set_budgets(const VirtualGeometryBudgets &p_budgets) {
	_free_rd_heaps();
	budgets = p_budgets;
	const uint64_t vertex_capacity = MIN(budgets.position_heap_bytes / 12, budgets.attribute_heap_bytes / sizeof(VirtualGeometryGPUVertexAttributes));
	vertex_slot_heap.reset(vertex_capacity);
	index_heap.reset(budgets.index_heap_bytes);
}

Error VirtualGeometryStorage::set_package(const Package &p_package, uint64_t p_resource_revision) {
	ERR_FAIL_COND_V(validate_package(p_package, true) != OK, ERR_INVALID_DATA);
	uint64_t compressed_bytes = 0;
	for (const PackedByteArray &compressed : p_package.compressed_pages) {
		ERR_FAIL_COND_V(!checked_add_u64(compressed_bytes, uint64_t(compressed.size()), compressed_bytes) || compressed_bytes > budgets.compressed_cpu_bytes, ERR_OUT_OF_MEMORY);
	}
	_wait_for_worker_tasks();
	_free_rd_heaps();
	package = p_package; resource_revision = p_resource_revision; page_indices.clear(); pages.clear(); cluster_slots.clear(); active_cluster_ids.clear(); decoded_bytes = upload_bytes = cancelled_completions = coarse_fallback_exposures = 0; budget_deferred_pages = evicted_pages = descriptor_publications = 0;
	const uint64_t vertex_capacity = MIN(budgets.position_heap_bytes / 12, budgets.attribute_heap_bytes / sizeof(VirtualGeometryGPUVertexAttributes));
	vertex_slot_heap.reset(vertex_capacity); index_heap.reset(budgets.index_heap_bytes); gpu_descriptors.clear(); staged_gpu_descriptors.clear(); pending_gpu_descriptors.clear(); pending_descriptor_generation = active_descriptor_generation = descriptor_publication_serial = 0; next_descriptor_generation = 1; descriptor_snapshot_dirty = false;
	gpu_descriptors.resize(package.manifest.clusters.size());
	staged_gpu_descriptors.resize(package.manifest.clusters.size());
	pending_gpu_descriptors.resize(package.manifest.clusters.size());
	for (uint32_t i = 0; i < uint32_t(package.manifest.clusters.size()); i++) cluster_slots.insert(package.manifest.clusters[i].stable_id, i);
	for (int i = 0; i < package.manifest.pages.size(); i++) { page_indices.insert(package.manifest.pages[i].stable_id, i); PageRuntime runtime; pages.insert(package.manifest.pages[i].stable_id, runtime); }
	for (const PageDescriptor &page : package.manifest.pages) if (page.persistent) request_page(page.stable_id, VirtualGeometryRequestReason::PERSISTENT_ROOT, UINT32_MAX);
	return OK;
}

const PageDescriptor *VirtualGeometryStorage::_page(uint64_t p_page_id) const { const int *index = page_indices.getptr(p_page_id); return index ? &package.manifest.pages[*index] : nullptr; }
const ClusterDescriptor *VirtualGeometryStorage::_cluster(uint64_t p_cluster_id) const { for (const ClusterDescriptor &cluster : package.manifest.clusters) if (cluster.stable_id == p_cluster_id) return &cluster; return nullptr; }

Error VirtualGeometryStorage::request_page(uint64_t p_page_id, VirtualGeometryRequestReason p_reason, uint32_t p_priority) {
	PageRuntime *runtime = pages.getptr(p_page_id); ERR_FAIL_NULL_V(runtime, ERR_DOES_NOT_EXIST);
	if (runtime->state == VirtualGeometryPageState::ACTIVE || runtime->state == VirtualGeometryPageState::RESIDENT || runtime->state == VirtualGeometryPageState::DESCRIPTOR_PENDING || runtime->state == VirtualGeometryPageState::UPLOAD_PENDING) return OK;
	if (runtime->state == VirtualGeometryPageState::FAILED) { runtime->generation++; runtime->failure.clear(); }
	runtime->state = VirtualGeometryPageState::REQUESTED; runtime->reason = p_reason; runtime->priority = MAX(runtime->priority, p_priority); runtime->request_count++;
	return OK;
}

void VirtualGeometryStorage::mark_raster_interest(uint64_t p_page_id, uint64_t p_submission_serial, uint32_t p_priority) {
	PageRuntime *runtime = pages.getptr(p_page_id);
	if (!runtime) return;
	runtime->last_interest_serial = MAX(runtime->last_interest_serial, p_submission_serial);
	runtime->priority = MAX(runtime->priority, p_priority);
}

Vector<uint64_t> VirtualGeometryStorage::take_io_requests(uint32_t p_maximum) {
	Vector<uint64_t> result;
	while (result.size() < int(MIN(p_maximum, budgets.io_tasks_per_frame))) {
		uint64_t candidate = 0; uint32_t priority = 0;
		for (const KeyValue<uint64_t, PageRuntime> &entry : pages) if (entry.value.state == VirtualGeometryPageState::REQUESTED && (!candidate || entry.value.priority > priority || (entry.value.priority == priority && entry.key < candidate))) { candidate = entry.key; priority = entry.value.priority; }
		if (!candidate) break;
		pages[candidate].state = VirtualGeometryPageState::IO_PENDING; result.push_back(candidate);
	}
	return result;
}

void VirtualGeometryStorage::_decode_worker_entry(void *p_data) {
	WorkerDecodeTask *task = static_cast<WorkerDecodeTask *>(p_data);
	PackedByteArray decoded;
	String error;
	const Error result = task->storage->decode_page_on_worker(task->page_id, task->generation, decoded, error);
	task->storage->enqueue_worker_completion(task->page_id, task->generation, decoded, result, error);
	memdelete(task);
}

void VirtualGeometryStorage::start_worker_decode_tasks(const Vector<uint64_t> &p_page_ids) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (!pool) return;
	for (uint64_t page_id : p_page_ids) {
		PageRuntime *page = pages.getptr(page_id);
		if (!page || page->state != VirtualGeometryPageState::IO_PENDING) continue;
		page->state = VirtualGeometryPageState::DECODING;
		WorkerDecodeTask *task = memnew(WorkerDecodeTask);
		task->storage = this;
		task->page_id = page_id;
		task->generation = page->generation;
		const WorkerThreadPool::TaskID task_id = pool->add_native_task(&_decode_worker_entry, task, false, SNAME("VirtualGeometryDecode"));
		MutexLock lock(worker_task_mutex);
		worker_task_ids.push_back(task_id);
	}
}

Error VirtualGeometryStorage::decode_page_on_worker(uint64_t p_page_id, uint64_t p_generation, PackedByteArray &r_decoded, String &r_error) const {
	r_decoded.clear(); r_error.clear(); const PageDescriptor *page = _page(p_page_id);
	// The generation is deliberately checked on the render-thread completion
	// boundary. Worker tasks read immutable package data only, so cancellation
	// cannot race a map mutation or publish a stale payload.
	if (!page || p_generation == 0) { r_error = "Invalid virtual geometry page request."; return ERR_SKIP; }
	if (page->decoded_size > budgets.decoded_cpu_bytes || page->compressed_size > budgets.compressed_cpu_bytes) { r_error = "Virtual geometry page exceeds its declared worker budget."; return ERR_OUT_OF_MEMORY; }
	const int *page_index = page_indices.getptr(p_page_id); ERR_FAIL_NULL_V(page_index, ERR_DOES_NOT_EXIST);
	const PackedByteArray &compressed = package.compressed_pages[*page_index];
	if (uint64_t(compressed.size()) != page->compressed_size || page->decoded_size > uint64_t(INT64_MAX) || r_decoded.resize(int64_t(page->decoded_size)) != OK) { r_error = "Invalid virtual geometry page range."; return ERR_FILE_CORRUPT; }
	const int64_t decoded = Compression::decompress(r_decoded.ptrw(), r_decoded.size(), compressed.ptr(), compressed.size(), Compression::Mode(page->compression_scheme));
	if (decoded != r_decoded.size() || hash_bytes(r_decoded.ptr(), r_decoded.size()) != page->content_hash) { r_decoded.clear(); r_error = "Virtual geometry page checksum failed."; return ERR_FILE_CORRUPT; }
	return OK;
}

void VirtualGeometryStorage::enqueue_worker_completion(uint64_t p_page_id, uint64_t p_generation, const PackedByteArray &p_decoded, Error p_result, const String &p_error) {
	MutexLock lock(completion_mutex); WorkerCompletion completion; completion.page_id = p_page_id; completion.generation = p_generation; completion.decoded = p_decoded; completion.result = p_result; completion.error = p_error; worker_completions.push_back(completion);
}

bool VirtualGeometryStorage::_parse_page_upload(uint64_t p_page_id, const PackedByteArray &p_decoded, PageRuntime &r_runtime, String &r_error) {
	const PageDescriptor *page = _page(p_page_id);
	if (!page) return false;
	r_runtime.position_upload.clear(); r_runtime.index_upload.clear(); r_runtime.attribute_upload.clear(); r_runtime.descriptors.clear();
	auto append = [](PackedByteArray &to, const uint8_t *from, uint64_t size) -> bool { const int64_t offset = to.size(); return size <= uint64_t(INT64_MAX) && to.resize(offset + int64_t(size)) == OK && (memcpy(to.ptrw() + offset, from, size), true); };
	for (uint64_t id : page->cluster_ids) {
		const ClusterDescriptor *cluster = _cluster(id);
		if (!cluster || !checked_range_u64(cluster->payload_offset, cluster->payload_size, p_decoded.size()) || cluster->payload_size < 16) { r_error = "Virtual geometry cluster payload range is invalid."; return false; }
		const uint8_t *data = p_decoded.ptr() + cluster->payload_offset;
		uint32_t header[4]; memcpy(header, data, sizeof(header));
		if (header[0] != 0x31434756 || header[2] != cluster->vertex_count || header[3] != cluster->triangle_count || header[1] == 0 || (header[1] & ~uint32_t(STREAM_POSITION | STREAM_NORMAL | STREAM_TANGENT | STREAM_UV0 | STREAM_UV1 | STREAM_COLOR | STREAM_JOINTS | STREAM_WEIGHTS))) { r_error = "Virtual geometry cluster payload header/schema is invalid."; return false; }
		uint64_t stride = 12;
		if (header[1] & STREAM_NORMAL) stride += 12; if (header[1] & STREAM_TANGENT) stride += 16; if (header[1] & STREAM_UV0) stride += 8; if (header[1] & STREAM_UV1) stride += 8; if (header[1] & STREAM_COLOR) stride += sizeof(Color); if (header[1] & STREAM_JOINTS) stride += 16; if (header[1] & STREAM_WEIGHTS) stride += 16;
		uint64_t vertex_bytes = 0, index_bytes = 0, required = 16;
		if (!checked_add_u64(required, stride * header[2], required) || !checked_add_u64(required, uint64_t(header[3]) * 3 * 4, required) || required != cluster->payload_size || !checked_add_u64(uint64_t(header[2]) * 12, 0, vertex_bytes) || !checked_add_u64(uint64_t(header[3]) * 12, 0, index_bytes)) { r_error = "Virtual geometry cluster payload size overflow."; return false; }
		const uint8_t *vertices = data + 16;
		for (uint32_t vertex = 0; vertex < header[2]; vertex++) {
			const uint8_t *source = vertices + uint64_t(vertex) * stride;
			VirtualGeometryGPUVertexAttributes attributes;
			attributes.source_stream_flags = header[1];
			uint64_t cursor = 12;
			Vector3 normal(0.0f, 1.0f, 0.0f);
			Vector3 tangent(1.0f, 0.0f, 0.0f);
			float tangent_sign = 1.0f;
			if (header[1] & STREAM_NORMAL) { memcpy(&normal, source + cursor, 12); cursor += 12; }
			if (header[1] & STREAM_TANGENT) { float source_tangent[4]; memcpy(source_tangent, source + cursor, 16); tangent = Vector3(source_tangent[0], source_tangent[1], source_tangent[2]); tangent_sign = source_tangent[3]; cursor += 16; }
			if (!normal.is_finite() || normal.is_zero_approx()) normal = Vector3(0.0f, 1.0f, 0.0f);
			if (!tangent.is_finite() || tangent.is_zero_approx()) tangent = Vector3(1.0f, 0.0f, 0.0f);
			const Vector2 encoded_normal = normal.normalized().octahedron_encode();
			const Vector2 encoded_tangent = tangent.normalized().octahedron_tangent_encode(tangent_sign < 0.0f ? -1.0f : 1.0f);
			attributes.axis_tangent[0] = encoded_normal.x;
			attributes.axis_tangent[1] = encoded_normal.y;
			attributes.axis_tangent[2] = encoded_tangent.x;
			attributes.axis_tangent[3] = encoded_tangent.y;
			if (header[1] & STREAM_UV0) { memcpy(attributes.uv0, source + cursor, 8); cursor += 8; }
			if (header[1] & STREAM_UV1) { memcpy(attributes.uv1, source + cursor, 8); cursor += 8; }
			if (header[1] & STREAM_COLOR) { Color color; memcpy(&color, source + cursor, sizeof(Color)); attributes.color[0] = color.r; attributes.color[1] = color.g; attributes.color[2] = color.b; attributes.color[3] = color.a; cursor += sizeof(Color); }
			if (header[1] & STREAM_JOINTS) { memcpy(attributes.joints, source + cursor, 16); cursor += 16; }
			if (header[1] & STREAM_WEIGHTS) { memcpy(attributes.weights, source + cursor, 16); cursor += 16; }
			if (cursor != stride || !append(r_runtime.position_upload, source, 12) || !append(r_runtime.attribute_upload, reinterpret_cast<const uint8_t *>(&attributes), sizeof(attributes))) { r_error = "Virtual geometry canonical attribute staging failed."; return false; }
		}
		const uint8_t *indices = vertices + stride * header[2];
		for (uint32_t index = 0; index < header[3] * 3; index++) { uint32_t value; memcpy(&value, indices + uint64_t(index) * 4, 4); if (value >= header[2]) { r_error = "Virtual geometry cluster index is out of range."; return false; } }
		if (!append(r_runtime.index_upload, indices, index_bytes)) { r_error = "Virtual geometry index staging allocation failed."; return false; }
		VirtualGeometryGPUClusterDescriptor descriptor; descriptor.stable_id = cluster->stable_id; descriptor.index_count = header[3] * 3; descriptor.material_slot = cluster->material_slot; descriptor.flags = cluster->flags; descriptor.source_stream_flags = header[1]; descriptor.attribute_stride = sizeof(VirtualGeometryGPUVertexAttributes); r_runtime.descriptors.push_back(descriptor);
	}
	return true;
}

bool VirtualGeometryStorage::_enqueue_rd_upload(PageRuntime &r_runtime, String &r_error) {
	if (!_ensure_rd_heaps(r_error)) return false;
	for (VirtualGeometryGPUClusterDescriptor &descriptor : r_runtime.descriptors) {
		const uint32_t *slot = cluster_slots.getptr(descriptor.stable_id);
		if (!slot) { r_error = "Virtual geometry descriptor has no fixed manifest slot."; return false; }
		staged_gpu_descriptors.write[*slot] = descriptor;
	}
	descriptor_snapshot_dirty = true;
	if (!rendering_device) return true;
	if (rendering_device->buffer_update(position_heap_rid, uint32_t(r_runtime.positions.offset), uint32_t(r_runtime.position_upload.size()), r_runtime.position_upload.ptr()) != OK || rendering_device->buffer_update(index_heap_rid, uint32_t(r_runtime.indices.offset), uint32_t(r_runtime.index_upload.size()), r_runtime.index_upload.ptr()) != OK || (!r_runtime.attribute_upload.is_empty() && rendering_device->buffer_update(attribute_heap_rid, uint32_t(r_runtime.attributes.offset), uint32_t(r_runtime.attribute_upload.size()), r_runtime.attribute_upload.ptr()) != OK)) { r_error = "RenderingDevice rejected virtual geometry heap upload."; return false; }
	return true;
}

bool VirtualGeometryStorage::_publish_descriptor_snapshot(uint64_t p_submission_serial, String &r_error) {
	if (!descriptor_snapshot_dirty || pending_descriptor_generation != 0) return true;
	const uint32_t generation = next_descriptor_generation++;
	if (next_descriptor_generation == 0) next_descriptor_generation = 1;
	pending_gpu_descriptors = staged_gpu_descriptors;
	for (VirtualGeometryGPUClusterDescriptor &descriptor : pending_gpu_descriptors) {
		descriptor.generation = descriptor.index_count == 0 ? 0 : generation;
	}
	if (rendering_device && !pending_gpu_descriptors.is_empty() && rendering_device->buffer_update(pending_descriptor_heap_rid, 0, uint32_t(pending_gpu_descriptors.size() * sizeof(VirtualGeometryGPUClusterDescriptor)), pending_gpu_descriptors.ptr()) != OK) {
		r_error = "RenderingDevice rejected virtual geometry descriptor snapshot publication.";
		return false;
	}
	pending_descriptor_generation = generation;
	descriptor_publication_serial = p_submission_serial;
	descriptor_snapshot_dirty = false;
	descriptor_publications++;
	return true;
}

bool VirtualGeometryStorage::_allocate_page(uint64_t p_page_id, const PackedByteArray &p_decoded, uint64_t p_completion_serial, String &r_error) {
	PageRuntime *runtime = pages.getptr(p_page_id); if (!runtime || !_parse_page_upload(p_page_id, p_decoded, *runtime, r_error)) return false;
	const uint64_t vertex_count = uint64_t(runtime->position_upload.size()) / 12;
	if (runtime->attribute_upload.size() != int64_t(vertex_count * sizeof(VirtualGeometryGPUVertexAttributes)) || !vertex_slot_heap.allocate(MAX(uint64_t(1), vertex_count), 1, runtime->vertices) || !index_heap.allocate(MAX(uint64_t(1), uint64_t(runtime->index_upload.size())), 16, runtime->indices)) { _free_page(*runtime); r_error = "Virtual geometry heap budget or fragmentation prevented allocation."; return false; }
	runtime->positions = { runtime->vertices.offset * 12, runtime->vertices.size * 12 };
	runtime->attributes = { runtime->vertices.offset * sizeof(VirtualGeometryGPUVertexAttributes), runtime->vertices.size * sizeof(VirtualGeometryGPUVertexAttributes) };
	uint64_t vertex_cursor = runtime->vertices.offset, index_cursor = runtime->indices.offset;
	for (VirtualGeometryGPUClusterDescriptor &descriptor : runtime->descriptors) { descriptor.position_offset = vertex_cursor * 12; descriptor.index_offset = index_cursor; descriptor.attribute_offset = vertex_cursor * sizeof(VirtualGeometryGPUVertexAttributes); ERR_FAIL_COND_V_MSG(vertex_cursor > uint64_t(INT32_MAX), false, "Virtual geometry base vertex exceeds indexed indirect range."); descriptor.base_vertex = uint32_t(vertex_cursor); descriptor.first_index = uint32_t(index_cursor / 4); const ClusterDescriptor *cluster = _cluster(descriptor.stable_id); vertex_cursor += uint64_t(cluster->vertex_count); index_cursor += uint64_t(cluster->triangle_count) * 12; }
	runtime->upload_completion_serial = p_completion_serial;
	if (!_enqueue_rd_upload(*runtime, r_error)) { _free_page(*runtime); return false; }
	runtime->state = VirtualGeometryPageState::UPLOAD_PENDING; upload_bytes += p_decoded.size(); return true;
}

void VirtualGeometryStorage::render_process(uint64_t p_completed_submission_serial, uint64_t p_pending_submission_serial) {
	_reap_completed_worker_tasks();
	notify_submission_completed(p_completed_submission_serial);
	evict_stale_pages(p_completed_submission_serial);
	Vector<WorkerCompletion> completions; { MutexLock lock(completion_mutex); SWAP(completions, worker_completions); }
	uint64_t frame_bytes = 0;
	for (const WorkerCompletion &completion : completions) {
		PageRuntime *runtime = pages.getptr(completion.page_id);
		if (!runtime || runtime->generation != completion.generation || (runtime->state != VirtualGeometryPageState::IO_PENDING && runtime->state != VirtualGeometryPageState::DECODING)) { cancelled_completions++; continue; }
		if (completion.result != OK || uint64_t(completion.decoded.size()) > budgets.decoded_cpu_bytes || frame_bytes + uint64_t(completion.decoded.size()) > budgets.upload_bytes_per_frame) { runtime->state = VirtualGeometryPageState::FAILED; runtime->failure_count++; runtime->failure = completion.error.is_empty() ? "Virtual geometry worker decode failed or exceeded budget." : completion.error; continue; }
		String error; if (!_allocate_page(completion.page_id, completion.decoded, p_pending_submission_serial, error)) { runtime->state = VirtualGeometryPageState::REQUESTED; runtime->generation++; runtime->failure = error; budget_deferred_pages++; continue; }
		frame_bytes += completion.decoded.size(); decoded_bytes += completion.decoded.size();
	}
	String publication_error;
	if (!_publish_descriptor_snapshot(p_pending_submission_serial, publication_error)) {
		ERR_PRINT(publication_error);
	}
}

void VirtualGeometryStorage::_activate_ready_groups(uint64_t p_completed_submission_serial) {
	for (KeyValue<uint64_t, PageRuntime> &entry : pages) if (entry.value.state == VirtualGeometryPageState::UPLOAD_PENDING && entry.value.upload_completion_serial <= p_completed_submission_serial) entry.value.state = VirtualGeometryPageState::RESIDENT;
	// Descriptor publication is render-thread-only. In VG2 the descriptor table
	// is a renderer-owned CPU record; VG3 will bind the same transition to its
	// GPU descriptor buffer update. It is intentionally a distinct lifecycle
	// edge so a decoded/uploaded page cannot be mistaken for selectable data.
	for (KeyValue<uint64_t, PageRuntime> &entry : pages) if (entry.value.state == VirtualGeometryPageState::RESIDENT) entry.value.state = VirtualGeometryPageState::DESCRIPTOR_PENDING;
	for (const RefinementGroupDescriptor &group : package.manifest.groups) {
		bool coarse_ready = true;
		for (uint64_t cluster_id : group.coarse_cluster_ids) { const ClusterDescriptor *cluster = _cluster(cluster_id); const PageRuntime *page = cluster ? pages.getptr(cluster->page_id) : nullptr; coarse_ready &= page && (page->state == VirtualGeometryPageState::DESCRIPTOR_PENDING || page->state == VirtualGeometryPageState::ACTIVE); }
		if (coarse_ready && group.persistent_root) for (uint64_t cluster_id : group.coarse_cluster_ids) { const ClusterDescriptor *cluster = _cluster(cluster_id); if (cluster && pages[cluster->page_id].state == VirtualGeometryPageState::DESCRIPTOR_PENDING) pages[cluster->page_id].state = VirtualGeometryPageState::ACTIVE; }
		if (_group_fine_complete(group)) {
			for (uint64_t cluster_id : group.fine_cluster_ids) { const ClusterDescriptor *cluster = _cluster(cluster_id); if (cluster && pages[cluster->page_id].state == VirtualGeometryPageState::DESCRIPTOR_PENDING) pages[cluster->page_id].state = VirtualGeometryPageState::ACTIVE; }
		}
	}
	if (pending_descriptor_generation != 0 && descriptor_publication_serial <= p_completed_submission_serial) {
		SWAP(descriptor_heap_rid, pending_descriptor_heap_rid);
		gpu_descriptors = pending_gpu_descriptors;
		active_descriptor_generation = pending_descriptor_generation;
		pending_descriptor_generation = 0;
		descriptor_publication_serial = 0;
		active_cluster_ids.clear();
		for (const VirtualGeometryGPUClusterDescriptor &descriptor : gpu_descriptors) {
			const ClusterDescriptor *cluster = descriptor.index_count > 0 ? _cluster(descriptor.stable_id) : nullptr;
			const PageRuntime *page = cluster ? pages.getptr(cluster->page_id) : nullptr;
			if (page && page->state == VirtualGeometryPageState::ACTIVE && descriptor.generation == uint32_t(active_descriptor_generation)) active_cluster_ids.insert(descriptor.stable_id);
		}
		raster_integration_ready = rendering_device != nullptr && position_heap_rid.is_valid() && index_heap_rid.is_valid() && attribute_heap_rid.is_valid() && descriptor_heap_rid.is_valid();
	}
}

bool VirtualGeometryStorage::_group_fine_complete(const RefinementGroupDescriptor &p_group) const {
	for (uint64_t cluster_id : p_group.fine_cluster_ids) { const ClusterDescriptor *cluster = _cluster(cluster_id); const PageRuntime *page = cluster ? pages.getptr(cluster->page_id) : nullptr; if (!page || (page->state != VirtualGeometryPageState::DESCRIPTOR_PENDING && page->state != VirtualGeometryPageState::ACTIVE)) return false; }
	return true;
}

void VirtualGeometryStorage::notify_submission_completed(uint64_t p_completed_submission_serial) { _activate_ready_groups(p_completed_submission_serial); process_retirements(p_completed_submission_serial); }
void VirtualGeometryStorage::mark_page_used(uint64_t p_page_id, uint64_t p_raster_serial, uint64_t p_ray_serial, uint64_t p_dependency_serial) { PageRuntime *page = pages.getptr(p_page_id); if (!page) return; page->last_raster_serial = MAX(page->last_raster_serial, p_raster_serial); page->last_ray_serial = MAX(page->last_ray_serial, p_ray_serial); page->dependency_serial = MAX(page->dependency_serial, p_dependency_serial); }

void VirtualGeometryStorage::evict_stale_pages(uint64_t p_completed_submission_serial, uint64_t p_interest_horizon) {
	Vector<uint64_t> candidates;
	for (const KeyValue<uint64_t, PageRuntime> &entry : pages) {
		const PageDescriptor *descriptor = _page(entry.key);
		const PageRuntime &runtime = entry.value;
		if (!descriptor || descriptor->persistent || runtime.state != VirtualGeometryPageState::ACTIVE) continue;
		const uint64_t most_recent = MAX(runtime.last_interest_serial, MAX(runtime.last_raster_serial, runtime.last_ray_serial));
		if (most_recent + p_interest_horizon <= p_completed_submission_serial && _has_active_coarse_coverage(entry.key)) candidates.push_back(entry.key);
	}
	candidates.sort();
	for (uint64_t page_id : candidates) {
		if (retire_page(page_id) == OK) evicted_pages++;
	}
}
Error VirtualGeometryStorage::retire_page(uint64_t p_page_id) { PageRuntime *page = pages.getptr(p_page_id); ERR_FAIL_NULL_V(page, ERR_DOES_NOT_EXIST); if (page->state != VirtualGeometryPageState::ACTIVE && page->state != VirtualGeometryPageState::RESIDENT) return ERR_BUSY; if (!_has_active_coarse_coverage(p_page_id)) return ERR_BUSY; page->state = VirtualGeometryPageState::RETIRING; return OK; }
bool VirtualGeometryStorage::_has_active_coarse_coverage(uint64_t p_page_id) const { const PageDescriptor *page = _page(p_page_id); if (page && page->persistent) return false; for (const RefinementGroupDescriptor &group : package.manifest.groups) { bool contains = false, coarse_active = true; for (uint64_t fine : group.fine_cluster_ids) { const ClusterDescriptor *cluster = _cluster(fine); if (cluster && cluster->page_id == p_page_id) contains = true; } if (!contains) continue; for (uint64_t coarse : group.coarse_cluster_ids) { const ClusterDescriptor *cluster = _cluster(coarse); const PageRuntime *runtime = cluster ? pages.getptr(cluster->page_id) : nullptr; coarse_active &= runtime && runtime->state == VirtualGeometryPageState::ACTIVE; } if (coarse_active) return true; } return false; }
void VirtualGeometryStorage::_free_page(PageRuntime &r_page) {
	for (const VirtualGeometryGPUClusterDescriptor &descriptor : r_page.descriptors) {
		const uint32_t *slot = cluster_slots.getptr(descriptor.stable_id);
		if (slot) staged_gpu_descriptors.write[*slot] = VirtualGeometryGPUClusterDescriptor();
		active_cluster_ids.erase(descriptor.stable_id);
	}
	descriptor_snapshot_dirty = true;
	vertex_slot_heap.free(r_page.vertices); index_heap.free(r_page.indices); r_page.vertices = {}; r_page.indices = {}; r_page.positions = {}; r_page.attributes = {};
}
void VirtualGeometryStorage::process_retirements(uint64_t p_completed_submission_serial) { for (KeyValue<uint64_t, PageRuntime> &entry : pages) { PageRuntime &page = entry.value; if (page.state != VirtualGeometryPageState::RETIRING) continue; const uint64_t last = MAX(page.upload_completion_serial, MAX(page.last_raster_serial, MAX(page.last_ray_serial, page.dependency_serial))); if (last <= p_completed_submission_serial) { _free_page(page); page.state = VirtualGeometryPageState::UNLOADED; page.generation++; } } }
VirtualGeometryPageState VirtualGeometryStorage::get_page_state(uint64_t p_page_id) const { const PageRuntime *page = pages.getptr(p_page_id); return page ? page->state : VirtualGeometryPageState::FAILED; }
VirtualGeometryPageDiagnostics VirtualGeometryStorage::get_page_diagnostics(uint64_t p_page_id) const { VirtualGeometryPageDiagnostics result; const PageRuntime *page = pages.getptr(p_page_id); const PageDescriptor *descriptor = _page(p_page_id); if (!page) return result; result.state = page->state; result.generation = page->generation; result.request_count = page->request_count; result.failure_count = page->failure_count; result.upload_completion_serial = page->upload_completion_serial; result.last_raster_serial = page->last_raster_serial; result.last_ray_serial = page->last_ray_serial; result.dependency_serial = page->dependency_serial; result.last_interest_serial = page->last_interest_serial; result.priority = page->priority; result.failure = page->failure; result.persistent = descriptor && descriptor->persistent; return result; }
VirtualGeometryRuntimeDiagnostics VirtualGeometryStorage::get_diagnostics() const {
	VirtualGeometryRuntimeDiagnostics result;
	result.resource_revision = resource_revision;
	result.decoded_bytes = decoded_bytes;
	result.upload_bytes = upload_bytes;
	result.cancelled_completions = cancelled_completions;
	result.coarse_fallback_exposures = coarse_fallback_exposures;
	result.budget_deferred_pages = budget_deferred_pages;
	result.evicted_pages = evicted_pages;
	result.descriptor_publications = descriptor_publications;
	const VirtualGeometryHeap::Diagnostics slots = vertex_slot_heap.get_diagnostics();
	auto scaled = [](const VirtualGeometryHeap::Diagnostics &p_source, uint64_t p_stride) {
		VirtualGeometryHeap::Diagnostics result = p_source;
		result.capacity *= p_stride; result.used *= p_stride; result.free *= p_stride; result.largest_free_block *= p_stride; result.fragmentation_bytes *= p_stride;
		return result;
	};
	result.positions = scaled(slots, 12);
	result.attributes = scaled(slots, sizeof(VirtualGeometryGPUVertexAttributes));
	result.indices = index_heap.get_diagnostics();
	for (const KeyValue<uint64_t, PageRuntime> &entry : pages) {
		if (entry.value.state == VirtualGeometryPageState::ACTIVE) result.active_pages++;
		if (entry.value.state == VirtualGeometryPageState::REQUESTED || entry.value.state == VirtualGeometryPageState::IO_PENDING) result.requested_pages++;
		if (entry.value.state == VirtualGeometryPageState::FAILED) result.failed_pages++;
	}
	for (const PackedByteArray &data : package.compressed_pages) result.compressed_bytes += data.size();
	return result;
}


const HashSet<uint64_t> &VirtualGeometryStorage::get_active_cluster_ids() const {
	return active_cluster_ids;
}

const VirtualGeometryGPUClusterDescriptor *VirtualGeometryStorage::get_gpu_cluster_descriptor(uint64_t p_cluster_id) const {
	if (!active_cluster_ids.has(p_cluster_id)) return nullptr;
	for (const VirtualGeometryGPUClusterDescriptor &descriptor : gpu_descriptors) {
		if (descriptor.stable_id == p_cluster_id && descriptor.generation == uint32_t(active_descriptor_generation)) return &descriptor;
	}
	return nullptr;
}

VirtualGeometryRasterSelection VirtualGeometryStorage::select_raster_reference(const VirtualGeometryRasterSelectionInput &p_input, VirtualGeometryRasterSelectionState *p_state) const {
	return VirtualGeometryRasterSelector::select(package, get_active_cluster_ids(), p_input, p_state);
}

} // namespace RendererVirtualGeometry
