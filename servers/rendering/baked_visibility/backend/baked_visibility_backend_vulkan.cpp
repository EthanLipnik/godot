/**************************************************************************/
/*  baked_visibility_backend_vulkan.cpp                                   */
/**************************************************************************/

#include "baked_visibility_backend_vulkan.h"

#include "core/error/error_macros.h"
#include "core/object/callable_mp.h"
#include "core/os/semaphore.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include <cstring>

namespace {

// All pipeline stages deliberately declare the same set so that the generic
// RenderingDevice pipeline layout is stable across the raygen/miss/hit group.
static constexpr const char *VULKAN_BATCH_BINDINGS = R"(
layout(set = 0, binding = 0) uniform accelerationStructureEXT blocker_tlas;
layout(std430, set = 0, binding = 1) readonly buffer CandidateBuffer { vec4 candidate_bounds[]; } candidates;
layout(std430, set = 0, binding = 2) buffer CandidateMaskBuffer { uint candidate_mask[]; } masks;
layout(std430, set = 0, binding = 3) buffer BlockerHintBuffer { uint blocker_hint[]; } hints;
layout(push_constant) uniform BatchParameters {
    vec4 query_minimum;
    vec4 query_maximum;
    vec4 query_center;
    uint candidate_count;
    uint padding0;
    uint padding1;
    uint padding2;
} parameters;
)";

static constexpr const char *VULKAN_VERSION = R"(
#version 460
#extension GL_EXT_ray_tracing : require
)";

static constexpr const char *VULKAN_RAYGEN_BODY = R"(
layout(location = 0) rayPayloadEXT uint blocker_hit;
bool overlaps_query(vec4 candidate_minimum, vec4 candidate_maximum) {
    return all(lessThanEqual(candidate_minimum.xyz, parameters.query_maximum.xyz)) &&
           all(greaterThanEqual(candidate_maximum.xyz, parameters.query_minimum.xyz));
}
void main() {
    const uint candidate_index = gl_LaunchIDEXT.x;
    if (candidate_index >= parameters.candidate_count) {
        return;
    }
    const vec4 candidate_minimum = candidates.candidate_bounds[candidate_index * 2u];
    const vec4 candidate_maximum = candidates.candidate_bounds[candidate_index * 2u + 1u];
    masks.candidate_mask[candidate_index] = overlaps_query(candidate_minimum, candidate_maximum) ? 1u : 0u;
    const vec3 destination = 0.5 * (candidate_minimum.xyz + candidate_maximum.xyz);
    const vec3 delta = destination - parameters.query_center.xyz;
    const float ray_length = length(delta);
    blocker_hit = 0u;
    if (ray_length > 0.000001) {
        traceRayEXT(blocker_tlas, gl_RayFlagsOpaqueEXT, 0xff, 0, 0, 0,
                parameters.query_center.xyz, 0.0001, delta / ray_length,
                ray_length - 0.0001, 0);
    }
    hints.blocker_hint[candidate_index] = blocker_hit;
}
)";

static constexpr const char *VULKAN_MISS_BODY = R"(
layout(location = 0) rayPayloadInEXT uint blocker_hit;
void main() { blocker_hit = 0u; }
)";

static constexpr const char *VULKAN_CLOSEST_HIT_BODY = R"(
layout(location = 0) rayPayloadInEXT uint blocker_hit;
hitAttributeEXT vec2 hit_attributes;
void main() { blocker_hit = 1u; }
)";

struct alignas(16) VulkanBatchParameters {
	float query_minimum[4] = {};
	float query_maximum[4] = {};
	float query_center[4] = {};
	uint32_t candidate_count = 0;
	uint32_t padding[3] = {};
};

struct VulkanBatchResources {
	RID vertex_buffer;
	RID candidate_buffer;
	RID mask_buffer;
	RID hint_buffer;
	RID blas;
	RID tlas;
	RID raygen;
	RID miss;
	RID closest_hit;
	RID pipeline;
	RID hit_sbt;
	RID uniform_set;
};

struct CanonicalCandidateOrder {
	const Vector<BakedVisibilityBackendCandidate> *candidates = nullptr;

	bool operator()(uint32_t p_a, uint32_t p_b) const {
		const BakedVisibilityBackendCandidate &a = (*candidates)[p_a];
		const BakedVisibilityBackendCandidate &b = (*candidates)[p_b];
		if (a.canonical_index != b.canonical_index) {
			return a.canonical_index < b.canonical_index;
		}
		return p_a < p_b;
	}
};

static String _join_shader(const char *p_body) {
	return String(VULKAN_VERSION) + String(VULKAN_BATCH_BINDINGS) + String(p_body);
}

static RID _create_stage_shader(RenderingDevice *p_rd, RenderingDevice::ShaderStage p_stage, const String &p_source, const String &p_name, String *r_error) {
	RenderingDevice::ShaderStageSPIRVData stage_data;
	stage_data.shader_stage = p_stage;
	stage_data.spirv = p_rd->shader_compile_spirv_from_source(p_stage, p_source, RenderingDevice::SHADER_LANGUAGE_GLSL, r_error);
	if (stage_data.spirv.is_empty()) {
		return RID();
	}
	Vector<RenderingDevice::ShaderStageSPIRVData> stages;
	stages.push_back(stage_data);
	return p_rd->shader_create_from_spirv(stages, p_name);
}

static void _append_blocker_box(const AABB &p_bounds, Vector<float> &r_vertices) {
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
		r_vertices.push_back(vertex.x);
		r_vertices.push_back(vertex.y);
		r_vertices.push_back(vertex.z);
	}
}

static void _release_batch_resources(RenderingDevice *p_rd, VulkanBatchResources &r_resources) {
	if (!p_rd) {
		return;
	}
	if (r_resources.uniform_set.is_valid()) p_rd->free_rid(r_resources.uniform_set);
	if (r_resources.hit_sbt.is_valid()) p_rd->free_rid(r_resources.hit_sbt);
	if (r_resources.pipeline.is_valid()) p_rd->free_rid(r_resources.pipeline);
	if (r_resources.closest_hit.is_valid()) p_rd->free_rid(r_resources.closest_hit);
	if (r_resources.miss.is_valid()) p_rd->free_rid(r_resources.miss);
	if (r_resources.raygen.is_valid()) p_rd->free_rid(r_resources.raygen);
	if (r_resources.tlas.is_valid()) p_rd->free_rid(r_resources.tlas);
	if (r_resources.blas.is_valid()) p_rd->free_rid(r_resources.blas);
	if (r_resources.hint_buffer.is_valid()) p_rd->free_rid(r_resources.hint_buffer);
	if (r_resources.mask_buffer.is_valid()) p_rd->free_rid(r_resources.mask_buffer);
	if (r_resources.candidate_buffer.is_valid()) p_rd->free_rid(r_resources.candidate_buffer);
	if (r_resources.vertex_buffer.is_valid()) p_rd->free_rid(r_resources.vertex_buffer);
	r_resources = VulkanBatchResources();
}

static Error _build_pipeline_and_geometry(RenderingDevice *p_rd, const BakedVisibilityBackendBatchInput &p_input, VulkanBatchResources &r_resources, String *r_error) {
	Vector<float> vertices;
	for (const BakedVisibilityBackendBlocker &blocker : p_input.blockers) {
		_append_blocker_box(blocker.bounds, vertices);
	}
	Vector<uint8_t> vertex_bytes;
	vertex_bytes.resize(vertices.size() * sizeof(float));
	memcpy(vertex_bytes.ptrw(), vertices.ptr(), vertex_bytes.size());
	const BitField<RenderingDevice::BufferCreationBits> input_bits = RenderingDevice::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;
	r_resources.vertex_buffer = p_rd->_vertex_buffer_create(vertex_bytes.size(), vertex_bytes, input_bits);
	if (!r_resources.vertex_buffer.is_valid()) {
		if (r_error) *r_error = "Vulkan blocker vertex allocation failed";
		return ERR_CANT_CREATE;
	}
	RenderingDevice::AccelerationStructureGeometry geometry;
	geometry.flags = RenderingDevice::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
	geometry.vertex_buffer = r_resources.vertex_buffer;
	geometry.vertex_stride = sizeof(float) * 3;
	geometry.vertex_count = vertices.size() / 3;
	geometry.vertex_format = RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;
	const RenderingDevice::AccelerationStructureGeometry geometries[] = { geometry };
	r_resources.blas = p_rd->blas_create(geometries, RenderingDevice::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	if (!r_resources.blas.is_valid() || p_rd->blas_build(r_resources.blas) != OK) {
		if (r_error) *r_error = "Vulkan blocker BLAS creation/build failed";
		return ERR_CANT_CREATE;
	}

	r_resources.raygen = _create_stage_shader(p_rd, RenderingDevice::SHADER_STAGE_RAYGEN, _join_shader(VULKAN_RAYGEN_BODY), "BakedVisibilityVulkanRaygen", r_error);
	r_resources.miss = _create_stage_shader(p_rd, RenderingDevice::SHADER_STAGE_MISS, _join_shader(VULKAN_MISS_BODY), "BakedVisibilityVulkanMiss", r_error);
	r_resources.closest_hit = _create_stage_shader(p_rd, RenderingDevice::SHADER_STAGE_CLOSEST_HIT, _join_shader(VULKAN_CLOSEST_HIT_BODY), "BakedVisibilityVulkanClosestHit", r_error);
	if (!r_resources.raygen.is_valid() || !r_resources.miss.is_valid() || !r_resources.closest_hit.is_valid()) {
		return ERR_CANT_CREATE;
	}
	RenderingDevice::PipelineShader raygen_stage = { .shader = r_resources.raygen };
	RenderingDevice::PipelineShader miss_stage = { .shader = r_resources.miss };
	RenderingDevice::HitGroup hit_group;
	hit_group.closest_hit_shader.shader = r_resources.closest_hit;
	const RenderingDevice::PipelineShader raygen_stages[] = { raygen_stage };
	const RenderingDevice::PipelineShader miss_stages[] = { miss_stage };
	const RenderingDevice::HitGroup hit_groups[] = { hit_group };
	r_resources.pipeline = p_rd->raytracing_pipeline_create(raygen_stages, miss_stages, hit_groups, 1);
	if (!r_resources.pipeline.is_valid()) {
		if (r_error && r_error->is_empty()) *r_error = "Vulkan ray-tracing pipeline creation failed";
		return ERR_CANT_CREATE;
	}
	r_resources.hit_sbt = p_rd->hit_sbt_create(r_resources.pipeline, 1);
	const RenderingDevice::HitShaderBindingTableRange hit_range = p_rd->hit_sbt_range_alloc(r_resources.hit_sbt, 1);
	Vector<uint32_t> hit_indices;
	hit_indices.push_back(0);
	if (!r_resources.hit_sbt.is_valid() || !hit_range || p_rd->hit_sbt_range_update(r_resources.hit_sbt, hit_range, 0, hit_indices) != OK) {
		if (r_error) *r_error = "Vulkan hit shader binding table creation failed";
		return ERR_CANT_CREATE;
	}
	r_resources.tlas = p_rd->tlas_create(1, RenderingDevice::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	RenderingDevice::AccelerationStructureInstance instance;
	instance.blas = r_resources.blas;
	instance.hit_sbt_range = hit_range;
	const RenderingDevice::AccelerationStructureInstance instances[] = { instance };
	if (!r_resources.tlas.is_valid() || p_rd->tlas_build(r_resources.tlas, instances) != OK) {
		if (r_error) *r_error = "Vulkan blocker TLAS creation/build failed";
		return ERR_CANT_CREATE;
	}
	return OK;
}

static Error _execute_vulkan_batch_on_render_thread(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!BakedVisibilityVulkanBatchContract::is_supported(rd)) {
		if (r_error) *r_error = "Vulkan ray-tracing pipeline and ray-query features are required";
		return ERR_UNAVAILABLE;
	}
	if (p_input.candidates.is_empty() || p_input.blockers.is_empty()) {
		if (r_error) *r_error = "Vulkan hardware ray batch requires at least one candidate and blocker";
		return ERR_UNAVAILABLE;
	}

	VulkanBatchResources resources;
	Error error = _build_pipeline_and_geometry(rd, p_input, resources, r_error);
	if (error != OK) {
		_release_batch_resources(rd, resources);
		return error;
	}
	Vector<float> candidate_values;
	candidate_values.resize(p_input.candidates.size() * 8);
	for (uint32_t candidate = 0; candidate < p_input.candidates.size(); candidate++) {
		const AABB &bounds = p_input.candidates[candidate].bounds;
		const Vector3 maximum = bounds.get_end();
		float *values = candidate_values.ptrw() + candidate * 8;
		values[0] = bounds.position.x;
		values[1] = bounds.position.y;
		values[2] = bounds.position.z;
		values[3] = 0.0f;
		values[4] = maximum.x;
		values[5] = maximum.y;
		values[6] = maximum.z;
		values[7] = 0.0f;
	}
	Vector<uint8_t> candidate_bytes;
	candidate_bytes.resize(candidate_values.size() * sizeof(float));
	memcpy(candidate_bytes.ptrw(), candidate_values.ptr(), candidate_bytes.size());
	const uint32_t result_bytes = p_input.candidates.size() * sizeof(uint32_t);
	resources.candidate_buffer = rd->storage_buffer_create(candidate_bytes.size(), candidate_bytes);
	resources.mask_buffer = rd->storage_buffer_create(result_bytes);
	resources.hint_buffer = rd->storage_buffer_create(result_bytes);
	if (!resources.candidate_buffer.is_valid() || !resources.mask_buffer.is_valid() || !resources.hint_buffer.is_valid()) {
		if (r_error) *r_error = "Vulkan batch input/output buffer allocation failed";
		_release_batch_resources(rd, resources);
		return ERR_CANT_CREATE;
	}
	Vector<RenderingDevice::Uniform> uniforms;
	uniforms.push_back(RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, resources.tlas));
	uniforms.push_back(RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, resources.candidate_buffer));
	uniforms.push_back(RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 2, resources.mask_buffer));
	uniforms.push_back(RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 3, resources.hint_buffer));
	resources.uniform_set = rd->uniform_set_create(uniforms, resources.raygen, 0);
	if (!resources.uniform_set.is_valid()) {
		if (r_error) *r_error = "Vulkan ray-tracing descriptor set creation failed";
		_release_batch_resources(rd, resources);
		return ERR_CANT_CREATE;
	}
	VulkanBatchParameters parameters;
	const Vector3 query_end = p_input.query_bounds.get_end();
	const Vector3 query_center = p_input.query_bounds.get_center();
	parameters.query_minimum[0] = p_input.query_bounds.position.x;
	parameters.query_minimum[1] = p_input.query_bounds.position.y;
	parameters.query_minimum[2] = p_input.query_bounds.position.z;
	parameters.query_maximum[0] = query_end.x;
	parameters.query_maximum[1] = query_end.y;
	parameters.query_maximum[2] = query_end.z;
	parameters.query_center[0] = query_center.x;
	parameters.query_center[1] = query_center.y;
	parameters.query_center[2] = query_center.z;
	parameters.candidate_count = p_input.candidates.size();
	const RenderingDevice::RaytracingListID list = rd->raytracing_list_begin();
	rd->raytracing_list_bind_raytracing_pipeline(list, resources.pipeline);
	rd->raytracing_list_bind_uniform_set(list, resources.uniform_set, 0);
	rd->raytracing_list_set_push_constant(list, &parameters, sizeof(parameters));
	rd->raytracing_list_trace_rays(list, 0, resources.hit_sbt, p_input.candidates.size(), 1, 1);
	rd->raytracing_list_end();

	const Vector<uint8_t> mask_data = rd->buffer_get_data(resources.mask_buffer);
	const Vector<uint8_t> hint_data = rd->buffer_get_data(resources.hint_buffer);
	if (mask_data.size() != result_bytes || hint_data.size() != result_bytes) {
		if (r_error) *r_error = "Vulkan batch readback was incomplete";
		_release_batch_resources(rd, resources);
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	BakedVisibilityBackendBatchOutput cpu_output;
	BakedVisibilityBackend::execute_cpu_reference(p_input, cpu_output);
	r_output = cpu_output; // CPU remains the only certification oracle.
	const uint32_t *mask_values = reinterpret_cast<const uint32_t *>(mask_data.ptr());
	const uint32_t *hint_values = reinterpret_cast<const uint32_t *>(hint_data.ptr());
	r_output.candidate_mask.resize(p_input.candidates.size());
	r_output.compacted_candidate_indices.clear();
	r_output.hardware_blocker_hit_hints.resize(p_input.candidates.size());
	for (uint32_t candidate = 0; candidate < p_input.candidates.size(); candidate++) {
		const bool included = mask_values[candidate] != 0;
		r_output.candidate_mask.write[candidate] = included ? 1 : 0;
		r_output.hardware_blocker_hit_hints.write[candidate] = hint_values[candidate] != 0 ? 1 : 0;
		if (included) {
			r_output.compacted_candidate_indices.push_back(candidate);
		}
	}
	CanonicalCandidateOrder order;
	order.candidates = &p_input.candidates;
	SortArray<uint32_t, CanonicalCandidateOrder> sorter;
	sorter.compare = order;
	sorter.sort(r_output.compacted_candidate_indices.ptrw(), r_output.compacted_candidate_indices.size());
	const bool canonical_match = r_output.candidate_mask == cpu_output.candidate_mask && r_output.compacted_candidate_indices == cpu_output.compacted_candidate_indices;
	_release_batch_resources(rd, resources);
	if (!canonical_match) {
		if (r_error) *r_error = "Vulkan candidate mask or canonical compaction differed from CPU oracle";
		return ERR_INVALID_DATA;
	}
	r_output.dispatch_count = 1;
	r_output.ray_query_count = p_input.candidates.size();
	r_output.gpu_executed = true;
	r_output.hardware_ray_queries_executed = true;
	r_output.diagnostic = "Vulkan ray-tracing batch executed; CPU certification retained";
	return OK;
}

struct VulkanRenderThreadRequest {
	const BakedVisibilityBackendBatchInput *input = nullptr;
	BakedVisibilityBackendBatchOutput *output = nullptr;
	String *error_text = nullptr;
	Error error = ERR_UNAVAILABLE;
	Semaphore completed;
};

static void _execute_vulkan_render_thread(uint64_t p_request_address) {
	VulkanRenderThreadRequest *request = reinterpret_cast<VulkanRenderThreadRequest *>(uintptr_t(p_request_address));
	request->error = _execute_vulkan_batch_on_render_thread(*request->input, *request->output, request->error_text);
	request->completed.post();
}

} // namespace

bool BakedVisibilityVulkanBatchContract::is_supported(const RenderingDevice *p_rd) {
	return p_rd && p_rd->get_device_api_name() == "Vulkan" && p_rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE) && p_rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY);
}

String BakedVisibilityVulkanBatchContract::raygen_source() {
	return _join_shader(VULKAN_RAYGEN_BODY);
}

String BakedVisibilityVulkanBatchContract::miss_source() {
	return _join_shader(VULKAN_MISS_BODY);
}

String BakedVisibilityVulkanBatchContract::closest_hit_source() {
	return _join_shader(VULKAN_CLOSEST_HIT_BODY);
}

Error BakedVisibilityVulkanBatchContract::execute_batch(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!is_supported(rd)) {
		if (r_error) *r_error = "Vulkan ray-tracing pipeline and ray-query features are unavailable";
		return ERR_UNAVAILABLE;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (!rendering_server) {
		return _execute_vulkan_batch_on_render_thread(p_input, r_output, r_error);
	}
	VulkanRenderThreadRequest request;
	request.input = &p_input;
	request.output = &r_output;
	request.error_text = r_error;
	rendering_server->call_on_render_thread(callable_mp_static(&_execute_vulkan_render_thread).bind(uint64_t(uintptr_t(&request))));
	rendering_server->sync();
	request.completed.wait();
	return request.error;
}
