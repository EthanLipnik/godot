/**************************************************************************/
/*  streamed_cluster_runtime.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "streamed_cluster_runtime.h"

#include "core/os/os.h"
#include "core/templates/hash_set.h"

namespace RendererPathTracing {

void StreamedClusterResidencyManager::clear() {
	entries.clear();
	stable_order.clear();
	frame_stats = StreamedClusterFrameStats();
	current_frame = 0;
	next_task_token = 1;
	resident_bytes = 0;
	reserved_bytes = 0;
}

Error StreamedClusterResidencyManager::set_manifest(const Vector<StreamedClusterPageDescriptor> &p_pages) {
	clear();
	ERR_FAIL_COND_V_MSG(p_pages.is_empty(), ERR_INVALID_PARAMETER, "Streamed cluster manifest must contain at least one page.");
	bool has_persistent_page = false;
	for (const StreamedClusterPageDescriptor &page : p_pages) {
		ERR_FAIL_COND_V_MSG(page.stable_id == 0 || page.revision == 0 || entries.has(page.stable_id), ERR_INVALID_DATA, "Streamed cluster page IDs and revisions must be nonzero and IDs must be unique.");
		ERR_FAIL_COND_V_MSG(page.vertex_bytes == 0 || page.index_bytes == 0 || page.vertex_count == 0 || page.index_count == 0 || page.index_count % 3 != 0, ERR_INVALID_DATA, "Streamed cluster page has invalid triangle geometry.");
		ERR_FAIL_COND_V_MSG(uint64_t(page.vertex_count) * 12ull != page.vertex_bytes || uint64_t(page.index_count) * 4ull != page.index_bytes, ERR_INVALID_DATA, "Streamed cluster page byte counts do not match its supported position/index layout.");
		ERR_FAIL_COND_V_MSG(page.blob_bytes == 0 || page.blob_bytes > uint64_t(UINT32_MAX), ERR_INVALID_DATA, "Streamed cluster page blob exceeds the uint32 resource limit.");
		ERR_FAIL_COND_V_MSG(!page.bounds.is_finite() || !Math::is_finite(page.geometric_error) || page.geometric_error < 0.0f, ERR_INVALID_DATA, "Streamed cluster page has invalid bounds or geometric error.");
		ERR_FAIL_COND_V_MSG(page.persistent && page.parent_id != 0, ERR_INVALID_DATA, "A persistent streamed cluster fallback must be a root page.");
		Entry entry;
		entry.descriptor = page;
		entry.desired = page.persistent;
		entry.state = page.persistent ? StreamedClusterPageState::REQUESTED : StreamedClusterPageState::UNLOADED;
		entries.insert(page.stable_id, entry);
		stable_order.push_back(page.stable_id);
		has_persistent_page |= page.persistent;
	}
	stable_order.sort();
	ERR_FAIL_COND_V_MSG(!has_persistent_page, ERR_INVALID_DATA, "Streamed cluster manifest requires a persistent coarse fallback page.");
	for (uint64_t stable_id : stable_order) {
		const Entry &entry = entries[stable_id];
		if (entry.descriptor.parent_id != 0) {
			const Entry *parent = entries.getptr(entry.descriptor.parent_id);
			ERR_FAIL_COND_V_MSG(!parent || !parent->descriptor.child_ids.has(stable_id), ERR_INVALID_DATA, "Streamed cluster page references a missing or asymmetric parent.");
		}
		HashSet<uint64_t> unique_children;
		for (uint64_t child_id : entry.descriptor.child_ids) {
			const Entry *child = entries.getptr(child_id);
			ERR_FAIL_COND_V_MSG(unique_children.has(child_id) || !child || child->descriptor.parent_id != stable_id || !entry.descriptor.bounds.encloses(child->descriptor.bounds), ERR_INVALID_DATA, "Streamed cluster child link is duplicated, asymmetric, or outside its parent bounds.");
			unique_children.insert(child_id);
		}
	}
	// Every page must descend from a persistent root. This rejects disjoint
	// cycles as well as isolated nonpersistent roots while allowing multiple
	// independent persistent roots in stable order.
	HashSet<uint64_t> reachable;
	Vector<uint64_t> pending_roots;
	for (uint64_t stable_id : stable_order) {
		const Entry &entry = entries[stable_id];
		if (entry.descriptor.persistent) {
			pending_roots.push_back(stable_id);
		}
	}
	for (int pending_index = 0; pending_index < pending_roots.size(); pending_index++) {
		const uint64_t stable_id = pending_roots[pending_index];
		if (reachable.has(stable_id)) {
			continue;
		}
		reachable.insert(stable_id);
		const Entry &entry = entries[stable_id];
		for (uint64_t child_id : entry.descriptor.child_ids) {
			pending_roots.push_back(child_id);
		}
	}
	ERR_FAIL_COND_V_MSG(reachable.size() != entries.size(), ERR_INVALID_DATA, "Streamed cluster hierarchy contains a cycle or a page unreachable from a persistent root.");
	_refresh_counts();
	return OK;
}

bool StreamedClusterResidencyManager::_intersects_any_eye(const AABB &p_bounds, const Vector<AABB> &p_eye_regions) const {
	for (const AABB &region : p_eye_regions) {
		if (region.intersects(p_bounds)) {
			return true;
		}
	}
	return false;
}

bool StreamedClusterResidencyManager::_group_wants_refinement(const Entry &p_root, const StreamedClusterSelection &p_selection) const {
	return !p_root.descriptor.child_ids.is_empty() &&
			p_root.descriptor.geometric_error > p_selection.maximum_geometric_error &&
			_intersects_any_eye(p_root.descriptor.bounds, p_selection.eye_regions);
}

void StreamedClusterResidencyManager::update_selection(const StreamedClusterSelection &p_selection) {
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	current_frame = p_selection.frame;
	frame_stats.load_tasks = 0;
	frame_stats.retire_tasks = 0;
	frame_stats.loaded = 0;
	frame_stats.unloaded = 0;
	frame_stats.failed_loads = 0;
	frame_stats.budget_misses = 0;
	frame_stats.coarse_fallbacks = 0;
	frame_stats.transfer_bytes = 0;

	for (uint64_t stable_id : stable_order) {
		Entry &entry = entries[stable_id];
		entry.desired = entry.descriptor.persistent;
	}
	for (uint64_t stable_id : stable_order) {
		Entry &root = entries[stable_id];
		if (root.descriptor.parent_id != 0 || !_group_wants_refinement(root, p_selection)) {
			continue;
		}
		// Refinement is group-atomic: selecting a group requests every child. This
		// preserves border continuity and is a conservative superset for both eyes.
		for (uint64_t child_id : root.descriptor.child_ids) {
			entries[child_id].desired = true;
		}
	}

	for (uint64_t stable_id : stable_order) {
		Entry &entry = entries[stable_id];
		if (entry.desired && entry.state == StreamedClusterPageState::UNLOADED) {
			entry.state = StreamedClusterPageState::REQUESTED;
		} else if (!entry.desired && entry.state == StreamedClusterPageState::REQUESTED) {
			entry.state = StreamedClusterPageState::UNLOADED;
		} else if (!entry.desired && entry.state == StreamedClusterPageState::ACTIVE &&
				current_frame >= entry.activated_frame + budgets.minimum_resident_frames) {
			entry.state = StreamedClusterPageState::RETIRING;
			entry.retirement_issued = false;
			entry.retirement_completion_token = 0;
		}
	}
	frame_stats.selection_time_usec = OS::get_singleton()->get_ticks_usec() - start;
	_refresh_counts();
}

Vector<StreamedClusterLoadTask> StreamedClusterResidencyManager::take_load_tasks() {
	Vector<StreamedClusterLoadTask> tasks;
	uint64_t frame_transfer_bytes = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (uint64_t stable_id : stable_order) {
			Entry &entry = entries[stable_id];
			if (entry.state != StreamedClusterPageState::REQUESTED || !entry.desired || (pass == 0) != entry.descriptor.persistent) {
				continue;
			}
			const uint64_t bytes = uint64_t(entry.descriptor.vertex_bytes) + uint64_t(entry.descriptor.index_bytes);
			if (tasks.size() >= budgets.maximum_load_tasks_per_frame || bytes > budgets.maximum_load_bytes_per_frame - MIN(budgets.maximum_load_bytes_per_frame, frame_transfer_bytes) || bytes > budgets.maximum_resident_bytes - MIN(budgets.maximum_resident_bytes, resident_bytes + reserved_bytes)) {
				frame_stats.budget_misses++;
				continue;
			}
			StreamedClusterLoadTask task;
			task.task_token = next_task_token++;
			task.stable_id = stable_id;
			task.revision = entry.descriptor.revision;
			task.blob_bytes = entry.descriptor.blob_bytes;
			task.vertex_bytes = entry.descriptor.vertex_bytes;
			task.index_bytes = entry.descriptor.index_bytes;
			tasks.push_back(task);
			entry.state = StreamedClusterPageState::LOADING;
			entry.load_task_token = task.task_token;
			reserved_bytes += bytes;
			frame_transfer_bytes += bytes;
		}
	}
	frame_stats.load_tasks = tasks.size();
	frame_stats.transfer_bytes = frame_transfer_bytes;
	_refresh_counts();
	return tasks;
}

Error StreamedClusterResidencyManager::complete_load(uint64_t p_task_token, uint64_t p_upload_completion_token, RID p_vertex_buffer, RID p_index_buffer, bool p_success) {
	Entry *target = nullptr;
	for (uint64_t stable_id : stable_order) {
		Entry &entry = entries[stable_id];
		if (entry.state == StreamedClusterPageState::LOADING && entry.load_task_token == p_task_token) {
			target = &entry;
			break;
		}
	}
	ERR_FAIL_NULL_V(target, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V_MSG(p_success && p_upload_completion_token == 0, ERR_INVALID_PARAMETER, "Successful streamed-cluster uploads require a nonzero completion token.");
	const uint64_t bytes = uint64_t(target->descriptor.vertex_bytes) + uint64_t(target->descriptor.index_bytes);
	ERR_FAIL_COND_V(reserved_bytes < bytes, ERR_BUG);
	reserved_bytes -= bytes;
	if (!p_success || !p_vertex_buffer.is_valid() || !p_index_buffer.is_valid()) {
		target->state = target->desired ? StreamedClusterPageState::REQUESTED : StreamedClusterPageState::UNLOADED;
		target->load_task_token = 0;
		frame_stats.failed_loads++;
		_refresh_counts();
		return p_success ? ERR_INVALID_PARAMETER : FAILED;
	}
	target->vertex_buffer = p_vertex_buffer;
	target->index_buffer = p_index_buffer;
	target->upload_completion_token = p_upload_completion_token;
	target->state = StreamedClusterPageState::RESIDENT;
	target->load_task_token = 0;
	resident_bytes += bytes;
	_refresh_counts();
	return OK;
}

Vector<StreamedClusterRetireTask> StreamedClusterResidencyManager::take_retire_tasks() {
	Vector<StreamedClusterRetireTask> tasks;
	for (uint64_t stable_id : stable_order) {
		Entry &entry = entries[stable_id];
		if (entry.state != StreamedClusterPageState::RETIRING || entry.retirement_issued) {
			continue;
		}
		if (tasks.size() >= budgets.maximum_retire_tasks_per_frame) {
			break;
		}
		StreamedClusterRetireTask task;
		task.stable_id = stable_id;
		task.vertex_buffer = entry.vertex_buffer;
		task.index_buffer = entry.index_buffer;
		tasks.push_back(task);
		entry.retirement_issued = true;
	}
	frame_stats.retire_tasks = tasks.size();
	_refresh_counts();
	return tasks;
}

Error StreamedClusterResidencyManager::submit_retirement(uint64_t p_stable_id, uint64_t p_retirement_completion_token) {
	Entry *entry = entries.getptr(p_stable_id);
	ERR_FAIL_NULL_V(entry, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(entry->state != StreamedClusterPageState::RETIRING || !entry->retirement_issued, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(p_retirement_completion_token == 0, ERR_INVALID_PARAMETER, "Streamed-cluster retirement requires a nonzero completion token.");
	entry->retirement_completion_token = p_retirement_completion_token;
	return OK;
}

void StreamedClusterResidencyManager::update_completion_tokens(uint64_t p_completed_upload_token, uint64_t p_completed_retirement_token) {
	for (uint64_t stable_id : stable_order) {
		Entry &entry = entries[stable_id];
		if (entry.state == StreamedClusterPageState::RESIDENT && entry.upload_completion_token <= p_completed_upload_token) {
			entry.state = StreamedClusterPageState::ACTIVE;
			entry.activated_frame = current_frame;
			frame_stats.loaded++;
		} else if (entry.state == StreamedClusterPageState::RETIRING && entry.retirement_completion_token != 0 && entry.retirement_completion_token <= p_completed_retirement_token) {
			const uint64_t bytes = uint64_t(entry.descriptor.vertex_bytes) + uint64_t(entry.descriptor.index_bytes);
			ERR_CONTINUE(resident_bytes < bytes);
			resident_bytes -= bytes;
			entry.vertex_buffer = RID();
			entry.index_buffer = RID();
			entry.upload_completion_token = 0;
			entry.retirement_completion_token = 0;
			entry.retirement_issued = false;
			entry.state = entry.desired ? StreamedClusterPageState::REQUESTED : StreamedClusterPageState::UNLOADED;
			frame_stats.unloaded++;
		}
	}
	_refresh_counts();
}

StreamedClusterPageState StreamedClusterResidencyManager::get_page_state(uint64_t p_stable_id) const {
	const Entry *entry = entries.getptr(p_stable_id);
	return entry ? entry->state : StreamedClusterPageState::UNLOADED;
}

void StreamedClusterResidencyManager::_append_surface(const Entry &p_entry, Vector<StreamedClusterSurface> &r_surfaces) const {
	if (p_entry.state != StreamedClusterPageState::ACTIVE || !p_entry.vertex_buffer.is_valid() || !p_entry.index_buffer.is_valid()) {
		return;
	}
	StreamedClusterSurface surface;
	surface.stable_id = p_entry.descriptor.stable_id;
	surface.topology_revision = p_entry.descriptor.revision;
	surface.vertex_buffer = p_entry.vertex_buffer;
	surface.index_buffer = p_entry.index_buffer;
	surface.vertex_count = p_entry.descriptor.vertex_count;
	surface.index_count = p_entry.descriptor.index_count;
	surface.material_index = p_entry.descriptor.material_index;
	surface.bounds = p_entry.descriptor.bounds;
	r_surfaces.push_back(surface);
}

Vector<StreamedClusterSurface> StreamedClusterResidencyManager::get_active_surfaces() {
	Vector<StreamedClusterSurface> surfaces;
	for (uint64_t stable_id : stable_order) {
		Entry &root = entries[stable_id];
		if (root.descriptor.parent_id != 0 || !root.descriptor.persistent) {
			continue;
		}
		bool all_children_active = !root.descriptor.child_ids.is_empty();
		bool refinement_desired = false;
		for (uint64_t child_id : root.descriptor.child_ids) {
			const Entry &child = entries[child_id];
			refinement_desired |= child.desired;
			all_children_active &= child.state == StreamedClusterPageState::ACTIVE;
		}
		if (refinement_desired && all_children_active) {
			for (uint64_t child_id : root.descriptor.child_ids) {
				_append_surface(entries[child_id], surfaces);
			}
		} else {
			if (refinement_desired) {
				frame_stats.coarse_fallbacks++;
			}
			_append_surface(root, surfaces);
		}
	}
	return surfaces;
}

void StreamedClusterResidencyManager::_refresh_counts() {
	frame_stats.requested = 0;
	frame_stats.loading = 0;
	frame_stats.resident = 0;
	frame_stats.active = 0;
	frame_stats.retiring = 0;
	for (uint64_t stable_id : stable_order) {
		switch (entries[stable_id].state) {
			case StreamedClusterPageState::REQUESTED:
				frame_stats.requested++;
				break;
			case StreamedClusterPageState::LOADING:
				frame_stats.loading++;
				break;
			case StreamedClusterPageState::RESIDENT:
				frame_stats.resident++;
				break;
			case StreamedClusterPageState::ACTIVE:
				frame_stats.active++;
				break;
			case StreamedClusterPageState::RETIRING:
				frame_stats.retiring++;
				break;
			default:
				break;
		}
	}
	frame_stats.resident_bytes = resident_bytes;
	frame_stats.reserved_bytes = reserved_bytes;
}

} // namespace RendererPathTracing
