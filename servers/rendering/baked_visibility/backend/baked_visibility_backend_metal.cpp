/**************************************************************************/
/*  baked_visibility_backend_metal.cpp                                    */
/**************************************************************************/

#ifdef METAL_ENABLED

#include "baked_visibility_backend_metal.h"

#include "core/error/error_macros.h"
#include "core/object/callable_mp.h"
#include "core/os/semaphore.h"
#include "drivers/metal/metal3_objects.h"
#include "drivers/metal/rendering_device_driver_metal.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include <Metal/Metal.hpp>

namespace {

// Every field is explicitly sized to match MSL's float4/uint layout.
struct alignas(16) MetalBatchCandidate {
	float minimum[4] = {};
	float maximum[4] = {};
	uint32_t canonical_index = 0;
	uint32_t source_index = 0;
	uint32_t padding[2] = {};
};

struct alignas(16) MetalBatchParameters {
	float query_minimum[4] = {};
	float query_maximum[4] = {};
	float query_center[4] = {};
	uint32_t candidate_count = 0;
	uint32_t padding[3] = {};
};

static constexpr const char *METAL_BATCH_MSL = R"(
#include <metal_raytracing>
#include <metal_stdlib>
using namespace metal;

struct Candidate {
    float4 minimum;
    float4 maximum;
    uint canonical_index;
    uint source_index;
    uint2 padding;
};
struct Parameters {
    float4 query_minimum;
    float4 query_maximum;
    float4 query_center;
    uint candidate_count;
    uint3 padding;
};
static bool intersects(const Candidate candidate, const Parameters parameters) {
    return all(candidate.minimum.xyz <= parameters.query_maximum.xyz) &&
           all(candidate.maximum.xyz >= parameters.query_minimum.xyz);
}
kernel void classify_candidates(const device Candidate *candidates [[buffer(0)]],
        device uint *mask [[buffer(1)]], constant Parameters &parameters [[buffer(2)]],
        uint index [[thread_position_in_grid]]) {
    if (index >= parameters.candidate_count) return;
    mask[index] = intersects(candidates[index], parameters) ? 1u : 0u;
}
kernel void trace_blockers(const device Candidate *candidates [[buffer(0)]],
        device uint *hints [[buffer(1)]],
        raytracing::instance_acceleration_structure scene [[buffer(2)]],
        constant Parameters &parameters [[buffer(3)]], uint index [[thread_position_in_grid]]) {
    if (index >= parameters.candidate_count) return;
    float3 destination = 0.5f * (candidates[index].minimum.xyz + candidates[index].maximum.xyz);
    float3 delta = destination - parameters.query_center.xyz;
    float distance_to_destination = length(delta);
    if (distance_to_destination <= 0.000001f) {
        hints[index] = 0u;
        return;
    }
    raytracing::ray ray = { parameters.query_center.xyz, delta / distance_to_destination, 0.0001f, distance_to_destination - 0.0001f };
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
    auto hit = intersector.intersect(ray, scene, 0xff);
    hints[index] = hit.type == raytracing::intersection_type::triangle ? 1u : 0u;
}
kernel void compact_and_sort(const device Candidate *candidates [[buffer(0)]],
        const device uint *mask [[buffer(1)]], device uint *output [[buffer(2)]],
        constant Parameters &parameters [[buffer(3)]], uint invocation [[thread_position_in_grid]]) {
    // Small offline fixtures take a one-invocation reference path. Larger
    // batches rank every retained candidate in parallel, preserving the same
    // canonical (canonical_index, source_index) order without atomics.
    if (parameters.candidate_count > 64u) {
        if (invocation >= parameters.candidate_count) return;
        if (invocation == 0u) {
            uint count = 0u;
            for (uint input = 0u; input < parameters.candidate_count; input++) count += mask[input] != 0u ? 1u : 0u;
            output[0] = count;
        }
        if (mask[invocation] == 0u) return;
        uint rank = 0u;
        for (uint input = 0u; input < parameters.candidate_count; input++) {
            if (mask[input] == 0u) continue;
            bool before = candidates[input].canonical_index < candidates[invocation].canonical_index ||
                    (candidates[input].canonical_index == candidates[invocation].canonical_index && candidates[input].source_index < candidates[invocation].source_index);
            rank += before ? 1u : 0u;
        }
        output[rank + 1u] = invocation;
        return;
    }
    if (invocation != 0u) return;
    uint count = 0u;
    for (uint input = 0u; input < parameters.candidate_count; input++) {
        if (mask[input] != 0u) output[++count] = input;
    }
    // The offline batch is deliberately canonical rather than parallel here:
    // source index breaks equal canonical-index ties deterministically.
    for (uint i = 2u; i <= count; i++) {
        uint value = output[i];
        uint cursor = i;
        while (cursor > 1u) {
            uint previous = output[cursor - 1u];
            bool before = candidates[value].canonical_index < candidates[previous].canonical_index ||
                    (candidates[value].canonical_index == candidates[previous].canonical_index && candidates[value].source_index < candidates[previous].source_index);
            if (!before) break;
            output[cursor] = previous;
            cursor--;
        }
        output[cursor] = value;
    }
    output[0] = count;
}
)";

struct MetalBatchWork {
	NS::SharedPtr<MTL::ComputePipelineState> classify_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> compact_pipeline;
	NS::SharedPtr<MTL::Buffer> candidates;
	NS::SharedPtr<MTL::Buffer> vertices;
	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> blas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> blas;
	NS::SharedPtr<MTL::Buffer> blas_scratch;
	NS::SharedPtr<NS::Array> blas_array;
	NS::SharedPtr<MTL::Buffer> instances;
	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;
	NS::SharedPtr<MTL::Buffer> tlas_scratch;
	MTL::Buffer *mask = nullptr;
	MTL::Buffer *ray_hints = nullptr;
	MTL::Buffer *compacted = nullptr;
	MetalBatchParameters parameters;
	uint32_t candidate_count = 0;
};

static void _append_box_triangles(const AABB &p_bounds, Vector<MTL::PackedFloat3> &r_vertices) {
	const Vector3 minimum = p_bounds.position;
	const Vector3 maximum = p_bounds.get_end();
	const Vector3 corners[8] = {
		Vector3(minimum.x, minimum.y, minimum.z), Vector3(maximum.x, minimum.y, minimum.z),
		Vector3(maximum.x, maximum.y, minimum.z), Vector3(minimum.x, maximum.y, minimum.z),
		Vector3(minimum.x, minimum.y, maximum.z), Vector3(maximum.x, minimum.y, maximum.z),
		Vector3(maximum.x, maximum.y, maximum.z), Vector3(minimum.x, maximum.y, maximum.z),
	};
	static constexpr uint8_t indices[36] = {
		0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
		0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
		0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
	};
	for (uint8_t index : indices) {
		const Vector3 &vertex = corners[index];
		r_vertices.push_back(MTL::PackedFloat3(vertex.x, vertex.y, vertex.z));
	}
}

static void _retain_work_resources(MDCommandBufferBase *p_command, MetalBatchWork *p_work) {
	p_command->retain_resource(p_work->classify_pipeline.get());
	p_command->retain_resource(p_work->trace_pipeline.get());
	p_command->retain_resource(p_work->compact_pipeline.get());
	p_command->retain_resource(p_work->candidates.get());
	p_command->retain_resource(p_work->vertices.get());
	p_command->retain_resource(p_work->blas_descriptor.get());
	p_command->retain_resource(p_work->blas.get());
	p_command->retain_resource(p_work->blas_scratch.get());
	p_command->retain_resource(p_work->blas_array.get());
	p_command->retain_resource(p_work->instances.get());
	p_command->retain_resource(p_work->tlas_descriptor.get());
	p_command->retain_resource(p_work->tlas.get());
	p_command->retain_resource(p_work->tlas_scratch.get());
	p_command->retain_resource(p_work->mask);
	p_command->retain_resource(p_work->ray_hints);
	p_command->retain_resource(p_work->compacted);
}

static void _metal_batch_dispatch(RDD *p_driver, RDD::CommandBufferID p_command_buffer, MetalBatchWork *p_work) {
	(void)p_driver;
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	MTL::CommandBuffer *native_command = metal_command->get_command_buffer();
	MTL::AccelerationStructureCommandEncoder *as_encoder = native_command->accelerationStructureCommandEncoder();
	as_encoder->buildAccelerationStructure(p_work->blas.get(), p_work->blas_descriptor.get(), p_work->blas_scratch.get(), 0);
	as_encoder->buildAccelerationStructure(p_work->tlas.get(), p_work->tlas_descriptor.get(), p_work->tlas_scratch.get(), 0);
	as_encoder->endEncoding();

	MTL::ComputeCommandEncoder *compute = native_command->computeCommandEncoder();
	const MTL::Size grid(p_work->candidate_count, 1, 1);
	const MTL::Size threads(64, 1, 1);
	compute->setComputePipelineState(p_work->classify_pipeline.get());
	compute->setBuffer(p_work->candidates.get(), 0, 0);
	compute->setBuffer(p_work->mask, 0, 1);
	compute->setBytes(&p_work->parameters, sizeof(MetalBatchParameters), 2);
	compute->dispatchThreads(grid, threads);

	compute->setComputePipelineState(p_work->trace_pipeline.get());
	compute->setBuffer(p_work->candidates.get(), 0, 0);
	compute->setBuffer(p_work->ray_hints, 0, 1);
	compute->setAccelerationStructure(p_work->tlas.get(), 2);
	compute->setBytes(&p_work->parameters, sizeof(MetalBatchParameters), 3);
	compute->dispatchThreads(grid, threads);

	compute->setComputePipelineState(p_work->compact_pipeline.get());
	compute->setBuffer(p_work->candidates.get(), 0, 0);
	compute->setBuffer(p_work->mask, 0, 1);
	compute->setBuffer(p_work->compacted, 0, 2);
	compute->setBytes(&p_work->parameters, sizeof(MetalBatchParameters), 3);
	compute->dispatchThreads(grid, threads);
	compute->endEncoding();
	_retain_work_resources(command, p_work);
	memdelete(p_work);
}

static Error _create_pipeline(MTL::Device *p_device, MTL::Library *p_library, const char *p_name, NS::SharedPtr<MTL::ComputePipelineState> &r_pipeline, String *r_error) {
	NS::SharedPtr<MTL::Function> function = NS::TransferPtr(p_library->newFunction(NS::String::string(p_name, NS::UTF8StringEncoding)));
	NS::Error *error = nullptr;
	r_pipeline = NS::TransferPtr(p_device->newComputePipelineState(function.get(), &error));
	if (!r_pipeline) {
		if (r_error) {
			*r_error = error ? String::utf8(error->localizedDescription()->utf8String()) : "Metal compute pipeline creation failed";
		}
		return ERR_CANT_CREATE;
	}
	return OK;
}

static Error _metal_execute_on_render_thread(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || rd->get_device_api_name() != "Metal") {
		if (r_error) {
			*r_error = "No live Metal RenderingDevice";
		}
		return ERR_UNAVAILABLE;
	}
	RenderingDeviceDriverMetal *driver = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	MTL::Device *device = driver ? driver->get_device() : nullptr;
	if (!device || !device->supportsRaytracing()) {
		if (r_error) {
			*r_error = "Metal hardware ray tracing is unavailable on the active device";
		}
		return ERR_UNAVAILABLE;
	}
	if (p_input.candidates.is_empty() || p_input.blockers.is_empty()) {
		if (r_error) {
			*r_error = "Metal hardware ray batch requires at least one candidate and blocker";
		}
		return ERR_UNAVAILABLE;
	}

	NS::Error *compile_error = nullptr;
	NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(NS::String::string(METAL_BATCH_MSL, NS::UTF8StringEncoding), nullptr, &compile_error));
	if (!library) {
		if (r_error) {
			*r_error = compile_error ? String::utf8(compile_error->localizedDescription()->utf8String()) : "Metal baked visibility shader compilation failed";
		}
		return ERR_CANT_CREATE;
	}

	MetalBatchWork *work = memnew(MetalBatchWork);
	Error error = _create_pipeline(device, library.get(), "classify_candidates", work->classify_pipeline, r_error);
	if (error == OK) {
		error = _create_pipeline(device, library.get(), "trace_blockers", work->trace_pipeline, r_error);
	}
	if (error == OK) {
		error = _create_pipeline(device, library.get(), "compact_and_sort", work->compact_pipeline, r_error);
	}
	if (error != OK) {
		memdelete(work);
		return error;
	}

	Vector<MetalBatchCandidate> candidates;
	candidates.resize(p_input.candidates.size());
	for (uint32_t index = 0; index < p_input.candidates.size(); index++) {
		const BakedVisibilityBackendCandidate &source = p_input.candidates[index];
		MetalBatchCandidate &target = candidates.write[index];
		const Vector3 maximum = source.bounds.get_end();
		target.minimum[0] = source.bounds.position.x;
		target.minimum[1] = source.bounds.position.y;
		target.minimum[2] = source.bounds.position.z;
		target.maximum[0] = maximum.x;
		target.maximum[1] = maximum.y;
		target.maximum[2] = maximum.z;
		target.canonical_index = source.canonical_index;
		target.source_index = index;
	}
	work->candidates = NS::TransferPtr(device->newBuffer(candidates.ptr(), candidates.size() * sizeof(MetalBatchCandidate), MTL::ResourceStorageModeShared));
	Vector<MTL::PackedFloat3> vertices;
	for (const BakedVisibilityBackendBlocker &blocker : p_input.blockers) {
		_append_box_triangles(blocker.bounds, vertices);
	}
	work->vertices = NS::TransferPtr(device->newBuffer(vertices.ptr(), vertices.size() * sizeof(MTL::PackedFloat3), MTL::ResourceStorageModeShared));
	work->blas_descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> triangle = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
	triangle->setVertexBuffer(work->vertices.get());
	triangle->setVertexFormat(MTL::AttributeFormatFloat3);
	triangle->setVertexStride(sizeof(MTL::PackedFloat3));
	triangle->setTriangleCount(vertices.size() / 3);
	triangle->setOpaque(true);
	NS::Object *triangle_object = triangle.get();
	NS::SharedPtr<NS::Array> geometry_array = NS::TransferPtr(NS::Array::array(&triangle_object, 1)->retain());
	work->blas_descriptor->setGeometryDescriptors(geometry_array.get());
	work->blas_descriptor->setUsage(MTL::AccelerationStructureUsagePreferFastIntersection);
	const MTL::AccelerationStructureSizes blas_sizes = device->accelerationStructureSizes(work->blas_descriptor.get());
	work->blas = NS::TransferPtr(device->newAccelerationStructure(blas_sizes.accelerationStructureSize));
	work->blas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), blas_sizes.buildScratchBufferSize), MTL::ResourceStorageModePrivate));
	MTL::AccelerationStructure *blas_pointer = work->blas.get();
	work->blas_array = NS::TransferPtr(NS::Array::array(reinterpret_cast<NS::Object *const *>(&blas_pointer), 1)->retain());
	MTL::AccelerationStructureUserIDInstanceDescriptor instance = {};
	instance.transformationMatrix = MTL::PackedFloat4x3(MTL::PackedFloat3(1, 0, 0), MTL::PackedFloat3(0, 1, 0), MTL::PackedFloat3(0, 0, 1), MTL::PackedFloat3(0, 0, 0));
	instance.options = MTL::AccelerationStructureInstanceOptionOpaque;
	instance.mask = 0xff;
	instance.accelerationStructureIndex = 0;
	work->instances = NS::TransferPtr(device->newBuffer(&instance, sizeof(instance), MTL::ResourceStorageModeShared));
	work->tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
	work->tlas_descriptor->setInstancedAccelerationStructures(work->blas_array.get());
	work->tlas_descriptor->setInstanceDescriptorBuffer(work->instances.get());
	work->tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
	work->tlas_descriptor->setInstanceCount(1);
	const MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(work->tlas_descriptor.get());
	work->tlas = NS::TransferPtr(device->newAccelerationStructure(tlas_sizes.accelerationStructureSize));
	work->tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), tlas_sizes.buildScratchBufferSize), MTL::ResourceStorageModePrivate));
	work->candidate_count = candidates.size();
	const Vector3 query_end = p_input.query_bounds.get_end();
	const Vector3 query_center = p_input.query_bounds.get_center();
	work->parameters.query_minimum[0] = p_input.query_bounds.position.x;
	work->parameters.query_minimum[1] = p_input.query_bounds.position.y;
	work->parameters.query_minimum[2] = p_input.query_bounds.position.z;
	work->parameters.query_maximum[0] = query_end.x;
	work->parameters.query_maximum[1] = query_end.y;
	work->parameters.query_maximum[2] = query_end.z;
	work->parameters.query_center[0] = query_center.x;
	work->parameters.query_center[1] = query_center.y;
	work->parameters.query_center[2] = query_center.z;
	work->parameters.candidate_count = candidates.size();

	const uint32_t candidate_bytes = candidates.size() * sizeof(uint32_t);
	RID mask_rid = rd->storage_buffer_create(candidate_bytes);
	RID ray_rid = rd->storage_buffer_create(candidate_bytes);
	RID compact_rid = rd->storage_buffer_create((candidates.size() + 1) * sizeof(uint32_t));
	work->mask = reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, mask_rid));
	work->ray_hints = reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, ray_rid));
	work->compacted = reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, compact_rid));
	if (!work->mask || !work->ray_hints || !work->compacted) {
		rd->free_rid(mask_rid);
		rd->free_rid(ray_rid);
		rd->free_rid(compact_rid);
		memdelete(work);
		if (r_error) {
			*r_error = "Metal RenderingDevice did not expose storage buffers to the native callback";
		}
		return ERR_UNAVAILABLE;
	}
	const RenderingDevice::CallbackResource callback_resources[] = {
		{ .rid = mask_rid, .type = RenderingDevice::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE },
		{ .rid = ray_rid, .type = RenderingDevice::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE },
		{ .rid = compact_rid, .type = RenderingDevice::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE },
	};
	error = rd->driver_callback_add((RDD::DriverCallback)_metal_batch_dispatch, work, VectorView<RenderingDevice::CallbackResource>(callback_resources, 3));
	if (error != OK) {
		memdelete(work);
		rd->free_rid(mask_rid);
		rd->free_rid(ray_rid);
		rd->free_rid(compact_rid);
		if (r_error && r_error->is_empty()) {
			*r_error = "Metal driver callback scheduling failed";
		}
		return error;
	}

	const Vector<uint8_t> mask_data = rd->buffer_get_data(mask_rid);
	const Vector<uint8_t> ray_data = rd->buffer_get_data(ray_rid);
	const Vector<uint8_t> compact_data = rd->buffer_get_data(compact_rid);
	rd->free_rid(mask_rid);
	rd->free_rid(ray_rid);
	rd->free_rid(compact_rid);
	if (mask_data.size() != candidate_bytes || ray_data.size() != candidate_bytes || compact_data.size() < int(sizeof(uint32_t))) {
		if (r_error) {
			*r_error = "Metal baked visibility readback was incomplete";
		}
		return ERR_CANT_ACQUIRE_RESOURCE;
	}

	BakedVisibilityBackendBatchOutput cpu_output;
	BakedVisibilityBackend::execute_cpu_reference(p_input, cpu_output);
	r_output = cpu_output;
	const uint32_t *mask_values = reinterpret_cast<const uint32_t *>(mask_data.ptr());
	const uint32_t *ray_values = reinterpret_cast<const uint32_t *>(ray_data.ptr());
	const uint32_t *compact_values = reinterpret_cast<const uint32_t *>(compact_data.ptr());
	const uint32_t compact_count = compact_values[0];
	if (compact_count > p_input.candidates.size() || compact_data.size() < int((compact_count + 1) * sizeof(uint32_t))) {
		if (r_error) {
			*r_error = "Metal compacted candidate count was invalid";
		}
		return ERR_BUG;
	}
	r_output.candidate_mask.resize(p_input.candidates.size());
	r_output.compacted_candidate_indices.resize(compact_count);
	r_output.hardware_blocker_hit_hints.resize(p_input.candidates.size());
	for (uint32_t index = 0; index < p_input.candidates.size(); index++) {
		r_output.candidate_mask.write[index] = mask_values[index] != 0 ? 1 : 0;
		r_output.hardware_blocker_hit_hints.write[index] = ray_values[index] != 0 ? 1 : 0;
	}
	for (uint32_t index = 0; index < compact_count; index++) {
		r_output.compacted_candidate_indices.write[index] = compact_values[index + 1];
	}
	r_output.dispatch_count = 3;
	r_output.ray_query_count = p_input.candidates.size();
	r_output.gpu_executed = true;
	r_output.hardware_ray_queries_executed = true;
	r_output.diagnostic = "Metal compute and hardware ray batch executed; CPU certification retained";
	return OK;
}

struct MetalRenderThreadRequest {
	const BakedVisibilityBackendBatchInput *input = nullptr;
	BakedVisibilityBackendBatchOutput *output = nullptr;
	String *error_text = nullptr;
	Error error = ERR_UNAVAILABLE;
	Semaphore completed;
};

static void _execute_metal_render_thread(uint64_t p_request_address) {
	MetalRenderThreadRequest *request = reinterpret_cast<MetalRenderThreadRequest *>(uintptr_t(p_request_address));
	request->error = _metal_execute_on_render_thread(*request->input, *request->output, request->error_text);
	request->completed.post();
}

} // namespace

BakedVisibilityBackendCapabilities baked_visibility_metal_probe() {
	BakedVisibilityBackendCapabilities result;
	result.kind = BakedVisibilityBackendKind::METAL;
	result.available = false;
	result.can_discover_candidates = false;
	result.can_certify_invisibility = false;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || rd->get_device_api_name() != "Metal") {
		result.diagnostic = "No live Metal RenderingDevice; CPU fallback required";
		return result;
	}
	RenderingDeviceDriverMetal *driver = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	MTL::Device *device = driver ? driver->get_device() : nullptr;
	if (!device || !device->supportsRaytracing()) {
		result.diagnostic = "Active Metal device lacks hardware ray tracing; CPU fallback required";
		return result;
	}
	result.available = true;
	result.can_discover_candidates = true;
	result.supports_gpu_batches = true;
	result.supports_hardware_ray_queries = true;
	result.diagnostic = "Metal compute and hardware ray batch adapter is schedulable";
	return result;
}

Error baked_visibility_metal_execute(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (!rendering_server) {
		// Focused native-device tests own their RenderingDevice directly and run
		// on its render thread. Editor/service calls use the scheduler below.
		return _metal_execute_on_render_thread(p_input, r_output, r_error);
	}
	MetalRenderThreadRequest request;
	request.input = &p_input;
	request.output = &r_output;
	request.error_text = r_error;
	rendering_server->call_on_render_thread(callable_mp_static(&_execute_metal_render_thread).bind(uint64_t(uintptr_t(&request))));
	// Flushes a queued render-thread callable before the offline caller blocks.
	rendering_server->sync();
	request.completed.wait();
	return request.error;
}

#endif // METAL_ENABLED
