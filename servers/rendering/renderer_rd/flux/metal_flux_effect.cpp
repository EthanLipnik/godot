/**************************************************************************/
/*  metal_flux_effect.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#ifdef METAL_ENABLED

#include "metal_flux_effect.h"

#include "drivers/metal/metal3_objects.h"
#include "drivers/metal/rendering_device_driver_metal.h"
#include "servers/rendering/path_tracing/light_sampling.h"
#include "servers/rendering/path_tracing/reusable_path_sample.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"

#include <Metal/Metal.hpp>
#include <mach/mach_time.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>

namespace RendererRD {

static constexpr uint32_t HYBRID_FALLBACK_MATERIAL_TEXTURES = 16u;
static constexpr uint32_t HYBRID_MAX_BINDLESS_MATERIAL_TEXTURES = 2048u;
// These prototype proposal caches do not yet carry the complete PDF/domain
// contract required for final transport. Keep their host and shader gates
// fail-closed until that contract is implemented and validated.
static constexpr bool METAL_FLUX_REGIR_SUPPORTED = false;
static constexpr bool METAL_FLUX_REUSABLE_PATH_SUPPORTED = false;
static constexpr bool METAL_FLUX_RESTIR_DI_TEMPORAL_SPATIAL_REUSE_SUPPORTED = false;

struct MetalFluxCachedGeometry {
	uint64_t topology_revision = 0;
	uint64_t deformation_revision = 0;
	uint64_t last_seen_frame = 0;
	bool opaque = true;
	NS::SharedPtr<MTL::AccelerationStructure> acceleration_structure;
	NS::SharedPtr<MTL::Buffer> compacted_size_readback;
	NS::SharedPtr<MTL::AccelerationStructure> pending_compacted_structure;
	uint64_t compaction_query_serial = 0;
	uint64_t compaction_copy_serial = 0;
	uint64_t last_use_serial = 0;
	bool compaction_query_pending = false;
	bool compaction_copy_pending = false;
};

struct MetalFluxRetiredAccelerationStructure {
	NS::SharedPtr<MTL::AccelerationStructure> acceleration_structure;
	uint64_t last_use_serial = 0;
};

struct MetalFluxTimingCapture {
	NS::SharedPtr<MTL::CounterSampleBuffer> samples;
	std::atomic_bool complete = false;
	MTL::Timestamp cpu_begin = 0;
	MTL::Timestamp gpu_begin = 0;
	MTL::Timestamp cpu_end = 0;
	MTL::Timestamp gpu_end = 0;
	uint32_t view_count = 0;
	bool shadow_only = false;
	uint64_t diagnostics_owner_id = 0;
	uint64_t diagnostics_frame = 0;
};

struct MetalFluxEnvironmentDiagnosticCapture {
	NS::SharedPtr<MTL::Buffer> values;
	std::atomic_bool complete = false;
	uint64_t source_id = 0;
	uint64_t generation = 0;
	uint64_t checksum = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct MetalFluxMaterialDiagnosticCapture {
	NS::SharedPtr<MTL::Buffer> values;
	std::atomic_bool complete = false;
	bool shadow_only = false;
	bool trace_compaction_active = false;
	bool trace_compaction_fallback = false;
	bool stage_probe = false;
	bool report_metalfx_reactive_coverage = false;
	uint64_t diagnostics_owner_id = 0;
	uint64_t diagnostics_frame = 0;
	uint64_t submission_frame = 0;
	uint64_t light_revision = 0;
};

enum MetalFluxEnvironmentDiagnosticWord : uint32_t {
	ENVIRONMENT_DIAGNOSTIC_NONFINITE_COUNT,
	ENVIRONMENT_DIAGNOSTIC_PEAK_LUMINANCE,
	ENVIRONMENT_DIAGNOSTIC_MAXIMUM_WEIGHT,
	ENVIRONMENT_DIAGNOSTIC_PEAK_RED,
	ENVIRONMENT_DIAGNOSTIC_PEAK_GREEN,
	ENVIRONMENT_DIAGNOSTIC_PEAK_BLUE,
	ENVIRONMENT_DIAGNOSTIC_TOTAL_WEIGHT,
	ENVIRONMENT_DIAGNOSTIC_RESERVED,
	ENVIRONMENT_DIAGNOSTIC_WORD_COUNT,
};

enum MetalFluxMaterialDiagnosticWord : uint32_t {
	MATERIAL_DIAGNOSTIC_ALPHA_CANDIDATES,
	MATERIAL_DIAGNOSTIC_ALPHA_REJECTIONS,
	MATERIAL_DIAGNOSTIC_ALPHA_CANDIDATE_EXHAUSTIONS,
	MATERIAL_DIAGNOSTIC_GENERATION_REJECTIONS,
	MATERIAL_DIAGNOSTIC_MIXED_INTERSECTIONS,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_QUERIES_PRIMARY,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_QUERIES_VISIBILITY,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_QUERIES_REFLECTION,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_QUERIES_GI,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_HITS_PRIMARY,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_HITS_VISIBILITY,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_HITS_REFLECTION,
	MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_HITS_GI,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_QUERIES_PRIMARY,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_QUERIES_VISIBILITY,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_QUERIES_REFLECTION,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_QUERIES_GI,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_HITS_PRIMARY,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_HITS_VISIBILITY,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_HITS_REFLECTION,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_HITS_GI,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_REJECTIONS_PRIMARY,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_REJECTIONS_VISIBILITY,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_REJECTIONS_REFLECTION,
	MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_REJECTIONS_GI,
	MATERIAL_DIAGNOSTIC_SPLIT_MIXED_FALLBACK_PRIMARY,
	MATERIAL_DIAGNOSTIC_SPLIT_MIXED_FALLBACK_VISIBILITY,
	MATERIAL_DIAGNOSTIC_SPLIT_MIXED_FALLBACK_REFLECTION,
	MATERIAL_DIAGNOSTIC_SPLIT_MIXED_FALLBACK_GI,
	MATERIAL_DIAGNOSTIC_REAR_OPAQUE_HITS,
	MATERIAL_DIAGNOSTIC_ALPHA_PRIMARY_CANDIDATES,
	MATERIAL_DIAGNOSTIC_ALPHA_PRIMARY_REJECTIONS,
	MATERIAL_DIAGNOSTIC_ALPHA_VISIBILITY_CANDIDATES,
	MATERIAL_DIAGNOSTIC_ALPHA_VISIBILITY_REJECTIONS,
	MATERIAL_DIAGNOSTIC_ALPHA_REFLECTION_CANDIDATES,
	MATERIAL_DIAGNOSTIC_ALPHA_REFLECTION_REJECTIONS,
	MATERIAL_DIAGNOSTIC_ALPHA_INDIRECT_CANDIDATES,
	MATERIAL_DIAGNOSTIC_ALPHA_INDIRECT_REJECTIONS,
	MATERIAL_DIAGNOSTIC_ALPHA_MAX_CANDIDATES_PER_RAY,
	MATERIAL_DIAGNOSTIC_OCCUPANCY_EMPTY_REJECTIONS,
	MATERIAL_DIAGNOSTIC_OCCUPANCY_OPAQUE_ACCEPTS,
	MATERIAL_DIAGNOSTIC_OCCUPANCY_MIXED_SAMPLES,
	MATERIAL_DIAGNOSTIC_METALFX_REACTIVE_OPAQUE_PIXELS,
	MATERIAL_DIAGNOSTIC_METALFX_REACTIVE_REJECTED_PIXELS,
	MATERIAL_DIAGNOSTIC_INVALID_PDF_SAMPLES,
	MATERIAL_DIAGNOSTIC_NONFINITE_LOBE_SAMPLES,
	MATERIAL_DIAGNOSTIC_REJECTED_ENERGY_SAMPLES,
	MATERIAL_DIAGNOSTIC_PRIMARY_VALID_PIXELS,
	MATERIAL_DIAGNOSTIC_PRIMARY_INVALID_PIXELS,
	MATERIAL_DIAGNOSTIC_PRIMARY_LIT_PIXELS,
	MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_SELECTED,
	MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_CONTRIBUTED,
	MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_VISIBILITY_TESTS,
	MATERIAL_DIAGNOSTIC_STAGE_RAW_EMISSION,
	MATERIAL_DIAGNOSTIC_STAGE_RAW_EMISSIVE,
	MATERIAL_DIAGNOSTIC_STAGE_RAW_ANALYTIC,
	MATERIAL_DIAGNOSTIC_STAGE_RAW_INDIRECT,
	MATERIAL_DIAGNOSTIC_STAGE_TRACE_COMBINED,
	MATERIAL_DIAGNOSTIC_STAGE_SPATIAL,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_INPUT,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_OUTPUT,
	MATERIAL_DIAGNOSTIC_STAGE_COMPOSITE_BASE,
	MATERIAL_DIAGNOSTIC_STAGE_COMPOSITE_INPUT,
	MATERIAL_DIAGNOSTIC_STAGE_COMPOSITE_OUTPUT,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_REUSED_PIXELS,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_REJECTED_PIXELS,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_HISTORY_SAMPLES,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_SECOND_MOMENT,
	MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_VARIANCE,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_STAGED,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_UPDATES,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_QUERIES,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_VALID_CANDIDATES,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_REUSED_CANDIDATES,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_REJECTIONS,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_OCCUPIED,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_INVALID_RECORD,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ENDPOINT_BLOCKED,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_SHADING_INVALID,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ZERO_TARGET,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_INVALID_WEIGHT,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_CONSIDERED,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ACCEPTED,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_SELECTED,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_REEVALUATIONS,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_RECONNECTION_VISIBILITY,
	MATERIAL_DIAGNOSTIC_RESTIR_GI_CURRENT_CANDIDATES,
	MATERIAL_DIAGNOSTIC_RESTIR_GI_REUSED_CANDIDATES,
	MATERIAL_DIAGNOSTIC_RESTIR_GI_SELECTED_REUSE,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_LIGHTING_REEVALUATIONS,
	MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ENVIRONMENT_REEVALUATIONS,
	MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_CANDIDATES,
	MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_VALID,
	MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_CONTRIBUTED,
	MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_VISIBILITY_RAYS,
	MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_REJECTIONS,
	MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_NONFINITE_OR_PDF_FAILURES,
	MATERIAL_DIAGNOSTIC_DIRECT_CANDIDATE_EVALUATIONS,
	MATERIAL_DIAGNOSTIC_DIRECT_SELECTED_VISIBILITY,
	MATERIAL_DIAGNOSTIC_DIRECT_TEMPORAL_REUSE,
	MATERIAL_DIAGNOSTIC_DIRECT_SPATIAL_REUSE,
	MATERIAL_DIAGNOSTIC_GI_FRESH_RAYS,
	MATERIAL_DIAGNOSTIC_REFLECTION_RAYS,
	MATERIAL_DIAGNOSTIC_GI_CONVERGED_SKIPS,
	MATERIAL_DIAGNOSTIC_REFLECTION_CONVERGED_SKIPS,
	MATERIAL_DIAGNOSTIC_REFLECTION_ENVIRONMENT_MISSES,
	MATERIAL_DIAGNOSTIC_REFLECTION_ENVIRONMENT_CONTRIBUTIONS,
	MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_REPROJECTION_REJECTIONS,
	MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_SURFACE_REJECTIONS,
	MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_MOTION_REJECTIONS,
	MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_REFRESHES,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_INACTIVE_PIXELS,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ACTIVE_PIXELS,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_DIRECT,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_GI,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_REFLECTION,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_EXACT_ALPHA,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_COMPLEX,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_DIRECT,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_GI,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_REFLECTION,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_EXACT_ALPHA,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_COMPLEX,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_DIRECT,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_GI,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_REFLECTION,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_EXACT_ALPHA,
	MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_COMPLEX,
	MATERIAL_DIAGNOSTIC_WORD_COUNT,
};

static float _environment_diagnostic_float(uint32_t p_bits) {
	float value = 0.0f;
	static_assert(sizeof(value) == sizeof(p_bits));
	memcpy(&value, &p_bits, sizeof(value));
	return value;
}

struct MetalFluxResidencyTexturePlanEntry {
	RID rid;
	RendererPathTracing::HybridResidencyTextureChannel channel = RendererPathTracing::HybridResidencyTextureChannel::NONE;
	uint64_t generation = 1;
	uint64_t bytes = 1;
	MTL::Texture *texture = nullptr;
	uint32_t slot = RendererPathTracing::HYBRID_RESIDENCY_INVALID_SLOT;
	bool resident = false;
};

struct MetalFluxStableResidencyPlan {
	bool valid = false;
	bool complete = false;
	uint64_t identity = 0;
	uint64_t visibility_generation = 0;
	Vector<RendererPathTracing::HybridResidencyResourceKey> visible_keys;
	Vector<MetalFluxResidencyTexturePlanEntry> textures;
	Vector<MTL::Texture *> material_textures;
	uint32_t requested[static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::MAX)] = {};
	uint32_t resident[static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::MAX)] = {};
	uint32_t misses[static_cast<uint32_t>(RendererPathTracing::HybridResidencyTextureChannel::MAX)] = {};
	uint32_t texture_fallbacks = 0;
};

struct MetalFluxEffectCache {
	HashMap<uint64_t, MetalFluxCachedGeometry *> geometries;
	Vector<MetalFluxRetiredAccelerationStructure> retired_acceleration_structures;
	NS::SharedPtr<MTL::ComputePipelineState> trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> shadow_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> alpha_trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> alpha_shadow_pipeline;
	NS::SharedPtr<MTL::Function> trace_compute_function;
	NS::SharedPtr<MTL::Function> shadow_compute_function;
	NS::SharedPtr<MTL::Function> alpha_intersection_function;
	NS::SharedPtr<MTL::ComputePipelineState> filter_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> temporal_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> split_reconstruction_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> composite_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> environment_build_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> environment_reduce_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> environment_diagnostic_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_build_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_block_scan_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_block_prefix_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_finalize_pipeline;
	// The ReGIR proposal cache is deliberately cache-owned rather than attached
	// to a view history. It is a bounded world-keyed structure and is enabled
	// only for the current single-view Metal prototype.
	NS::SharedPtr<MTL::ComputePipelineState> regir_scroll_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> regir_classify_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> regir_reduce_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> reusable_path_clear_staging_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> reusable_path_reduce_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> trace_classify_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> trace_indirect_finalize_pipeline;
	NS::SharedPtr<MTL::Buffer> trace_queues;
	NS::SharedPtr<MTL::Buffer> trace_queue_counts;
	NS::SharedPtr<MTL::Buffer> trace_indirect_arguments;
	uint32_t trace_queue_capacity = 0;
	NS::SharedPtr<MTL::Buffer> regir_cells[2];
	NS::SharedPtr<MTL::Buffer> regir_header;
	NS::SharedPtr<MTL::Buffer> regir_staging;
	uint32_t regir_current = 0;
	uint32_t regir_stage_width = 0;
	uint32_t regir_stage_height = 0;
	int32_t regir_center_x = 0;
	int32_t regir_center_y = 0;
	int32_t regir_center_z = 0;
	uint64_t regir_revision[4] = {};
	bool regir_valid = false;
	// Unlike the screen histories and diffuse radiance texture, reusable paths
	// are cache-owned ABI-v3 geometry proposals. The two buffers enforce an
	// immutable previous-frame read and a separate post-trace update target.
	NS::SharedPtr<MTL::Buffer> reusable_path_cells[2];
	NS::SharedPtr<MTL::Buffer> reusable_path_staging;
	// One atomic claim per world cell turns visible first-hit staging into a
	// bounded per-cell producer rather than requiring an unrelated hash-selected
	// screen pixel to happen to be visible.
	NS::SharedPtr<MTL::Buffer> reusable_path_staging_claims;
	NS::SharedPtr<MTL::Buffer> reusable_path_header;
	uint32_t reusable_path_current = 0;
	int32_t reusable_path_center_x = 0;
	int32_t reusable_path_center_y = 0;
	int32_t reusable_path_center_z = 0;
	// Geometry/residency are cache invalidators. Lighting and environment are
	// replayed at the current receiver and therefore remain proposal-only.
	uint64_t reusable_path_revision[2] = {};
	bool reusable_path_valid = false;
	NS::SharedPtr<MTL::SamplerState> albedo_sampler;
	NS::SharedPtr<MTL::SamplerState> environment_sampler;
	NS::SharedPtr<MTL::Texture> environment_importance;
	NS::SharedPtr<MTL::Texture> environment_fallback_radiance;
	NS::SharedPtr<MTL::Texture> environment_fallback_importance;
	NS::SharedPtr<MTL::Texture> standalone_fallback_albedo;
	// Renderer-generated scalar STBN R16Uint texture2d_array, cache-owned and
	// uploaded once. A failed allocation preserves the progressive LDS path.
	NS::SharedPtr<MTL::Texture> stbn_scalar_volume;
	uint64_t stbn_scalar_volume_checksum = 0;
	bool stbn_scalar_volume_attempted = false;
	uint64_t environment_distribution_key = 0;
	uint32_t environment_mip_count = 0;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;
	Vector<MTL::AccelerationStructure *> tlas_blas_order;
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> tlas_instances;
	NS::SharedPtr<MTL::AccelerationStructure> opaque_tlas;
	Vector<MTL::AccelerationStructure *> opaque_tlas_blas_order;
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> opaque_tlas_instances;
	NS::SharedPtr<MTL::AccelerationStructure> alpha_tlas;
	Vector<MTL::AccelerationStructure *> alpha_tlas_blas_order;
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> alpha_tlas_instances;
	// These immutable upload buffers are keyed by exact record bytes. They are
	// safe to share across submitted command buffers and are replaced only after
	// an address, material, transform, residency slot, or generation changes.
	NS::SharedPtr<MTL::Buffer> cached_material_records;
	NS::SharedPtr<MTL::Buffer> cached_geometry_records;
	Vector<uint8_t> cached_material_record_bytes;
	Vector<uint8_t> cached_geometry_record_bytes;
	Vector<std::shared_ptr<MetalFluxTimingCapture>> timing_captures;
	Vector<std::shared_ptr<MetalFluxEnvironmentDiagnosticCapture>> environment_diagnostic_captures;
	Vector<std::shared_ptr<MetalFluxMaterialDiagnosticCapture>> material_diagnostic_captures;
	Vector<MetalFluxEffect::WorkAttribution> completed_work_attribution;
	uint64_t material_alpha_candidates = 0;
	uint64_t material_alpha_rejections = 0;
	uint64_t material_alpha_candidate_exhaustions = 0;
	uint64_t material_alpha_mixed_intersections = 0;
	uint64_t material_alpha_rear_opaque_hits = 0;
	uint64_t material_alpha_primary_candidates = 0;
	uint64_t material_alpha_primary_rejections = 0;
	uint64_t material_alpha_visibility_candidates = 0;
	uint64_t material_alpha_visibility_rejections = 0;
	uint64_t material_alpha_reflection_candidates = 0;
	uint64_t material_alpha_reflection_rejections = 0;
	uint64_t material_alpha_indirect_candidates = 0;
	uint64_t material_alpha_indirect_rejections = 0;
	uint32_t material_alpha_max_candidates_per_ray = 0;
	uint64_t material_occupancy_empty_rejections = 0;
	uint64_t material_occupancy_opaque_accepts = 0;
	uint64_t material_occupancy_mixed_samples = 0;
	uint64_t metalfx_reactive_opaque_pixels = 0;
	uint64_t metalfx_reactive_rejected_pixels = 0;
	uint64_t invalid_pdf_samples = 0;
	uint64_t nonfinite_lobe_samples = 0;
	uint64_t rejected_energy_samples = 0;
	uint64_t primary_valid_pixels = 0;
	uint64_t primary_invalid_pixels = 0;
	uint64_t primary_lit_pixels = 0;
	uint64_t primary_analytic_selected = 0;
	uint64_t primary_analytic_contributed = 0;
	uint64_t primary_analytic_visibility_tests = 0;
	uint64_t material_generation_rejects = 0;
	RendererPathTracing::HybridResidencyPlanner residency_planner;
	RendererPathTracing::HybridResidencyBudgets residency_budgets;
	std::shared_ptr<std::atomic<uint64_t>> completed_residency_token = std::make_shared<std::atomic<uint64_t>>(0);
	NS::SharedPtr<MTL::ArgumentEncoder> material_texture_argument_encoder;
	NS::SharedPtr<MTL::ArgumentEncoder> alpha_material_texture_argument_encoder;
	bool material_residency_configured = false;
	MetalFluxStableResidencyPlan stable_residency_plan;
	uint64_t stable_residency_plan_hits = 0;
	uint64_t stable_residency_plan_misses = 0;
	uint64_t stable_residency_plan_rebuilds = 0;
	bool alpha_intersection_configured = false;
	bool alpha_intersection_supported = false;
	bool bindless_material_textures = false;
	uint32_t material_texture_capacity = 0;
	// These textures are deliberately owned by the Metal adapter. They never
	// alias Forward+ history and are indexed by the submitted view order, so a
	// stereo pair has independent screen-space reservoirs and split histories.
	struct PerViewTransportState {
		uint64_t owner_identity = 0;
	NS::SharedPtr<MTL::Texture> reservoir[2];
		NS::SharedPtr<MTL::Texture> reservoir_sample[2];
		NS::SharedPtr<MTL::Texture> reservoir_surface[2];
		NS::SharedPtr<MTL::Texture> reservoir_metadata[2];
		NS::SharedPtr<MTL::Texture> primary_identity[2];
		// Compact material guide retained alongside the exact primary identity.
		// Split reconstruction uses it only to reject a material boundary or an
		// authored albedo/roughness change; it never supplies shading itself.
		NS::SharedPtr<MTL::Texture> primary_shading[2];
		// Exact ray-validated primary positions, not a raster reconstruction.
		NS::SharedPtr<MTL::Texture> primary_world_position[2];
		NS::SharedPtr<MTL::Texture> diffuse_history[2];
		NS::SharedPtr<MTL::Texture> specular_history[2];
		// Raw, validated ray samples retained for secondary transport reuse. These
		// are not an image history: MetalFX receives one selected sample unchanged
		// and remains the only image-space temporal/spatial denoiser.
		NS::SharedPtr<MTL::Texture> diffuse_transport_sample[2];
		NS::SharedPtr<MTL::Texture> specular_transport_sample[2];
		// Final selected direct visibility is retained separately from the
		NS::SharedPtr<MTL::Texture> diffuse_moments[2];
		NS::SharedPtr<MTL::Texture> specular_moments[2];
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t current = 0;
		uint64_t distribution_identity = 0;
		uint64_t reset_identity = 0;
		uint64_t admitted_geometry_generation = 0;
		uint64_t visibility_residency_generation = 0;
		uint64_t light_distribution_generation = 0;
		uint64_t environment_generation = 0;
	};
	Vector<PerViewTransportState> transport_views;
	uint64_t frame = 0;

	~MetalFluxEffectCache() {
		for (const KeyValue<uint64_t, MetalFluxCachedGeometry *> &entry : geometries) {
			memdelete(entry.value);
		}
	}
};

static constexpr const char *HYBRID_MSL = R"(
#include <metal_raytracing>
#include <metal_stdlib>
using namespace metal;

#ifndef HYBRID_BINDLESS_MATERIALS
#define HYBRID_BINDLESS_MATERIALS 0
#endif
#define HYBRID_FALLBACK_MATERIAL_TEXTURES 16u
#define HYBRID_MAX_BINDLESS_MATERIAL_TEXTURES 2048u
#if HYBRID_BINDLESS_MATERIALS
#define HYBRID_ALPHA_MATERIAL_TEXTURES HYBRID_MAX_BINDLESS_MATERIAL_TEXTURES
#else
#define HYBRID_ALPHA_MATERIAL_TEXTURES HYBRID_FALLBACK_MATERIAL_TEXTURES
#endif

#if HYBRID_BINDLESS_MATERIALS
struct MaterialTextureTable {
	array<texture2d<float, access::sample>, HYBRID_MAX_BINDLESS_MATERIAL_TEXTURES> textures [[id(0)]];
};
#endif

// Triangle intersection functions cannot receive texture arguments directly.
// Keep their renderer-owned resource table separate from the trace table: this
// lets the alpha path use the same individual texture views on both the Tier-2
// and bounded fallback paths without mutating an in-flight argument buffer.
struct AlphaMaterialTextureTable {
	array<texture2d<float, access::sample>, HYBRID_ALPHA_MATERIAL_TEXTURES> textures [[id(0)]];
};

struct Parameters {
    float4x4 world_from_view;
    float4x4 view_from_clip;
    float4x4 clip_from_world;
    float4x4 prev_clip_from_world;
    float4 light_direction_and_reflection_strength;
	float4 directional_light_radiance_enabled;
    float4 ao_distance_strength_roughness_flags;
	float4 contact_visibility_info; // Distance, strength, sample count, enabled.
    uint2 dimensions;
    uint shadow_sample_count;
    float directional_light_angular_radius;
    uint gi_sample_count;
    uint frame_index;
	float gi_strength;
	uint history_valid;
	uint emissive_count;
	uint emissive_triangle_count;
	uint punctual_light_count;
	float transport_max_distance; // Zero retains legacy unbounded secondary rays.
	float4x4 world_from_radiance;
	float4x4 radiance_from_world;
	float4 environment_info; // border, active scale, mip count, active flag.
	uint2 environment_dimensions; // Active lower-resolution proposal dimensions.
	uint2 environment_importance_dimensions; // Padded power-of-two pyramid base.
	float4 solar_current_direction_radius;
	float4 solar_previous_direction_transmittance;
	float4 solar_perpendicular_irradiance_enabled;
	uint4 solar_identity;
	uint4 solar_generations;
	uint2 light_distribution_identity;
	uint2 cache_revision;
	uint2 admitted_geometry_generation;
	uint2 visibility_residency_generation;
	uint2 light_distribution_generation;
	uint2 environment_generation;
	uint portal_count;
	uint portal_generation;
	uint adaptive_min_samples;
	uint adaptive_max_samples;
	float adaptive_variance_reference;
	float diffuse_cache_cell_size;
	uint alpha_mask_instance_count;
	uint material_texture_capacity;
	uint geometry_record_count;
	uint raster_primary_surface;
	// Bit 1 enables reactive-mask telemetry; bit 2 enables stage probes; bit 4
	// marks the MetalFX-only image-denoising path. Bit 3 is Flux-local secondary
	// convergence and must not feed an image history into MetalFX.
	uint reconstruction_flags;
	uint directional_light_cull_mask;
	uint directional_shadow_caster_mask;
	float directional_shadow_opacity;
	float directional_specular_amount;
	uint directional_flags;
	// Experimental feature mask. An unset bit must be fail-closed: neither an
	// optional resource nor a cached record is read for that estimator.
	uint experimental_feature_flags;
	uint sampling_sequence_mode; // 0 progressive LDS, 1 scalar STBN.
	uint sampling_tile_width;
	uint sampling_tile_height;
	uint sampling_tile_depth;
	uint sampling_tile_channels;
	uint bidirectional_caustic_mirror_count;
	uint bidirectional_caustic_source_count;
	float bidirectional_caustic_delta_roughness_threshold;
	uint bidirectional_caustic_flags; // bit 0 enabled, bit 1 active.
};

struct EnvironmentDiagnosticAtomic {
	atomic_uint nonfinite_texel_count;
	atomic_uint finite_peak_luminance_bits;
	atomic_uint maximum_texel_weight_bits;
	atomic_uint finite_peak_red_bits;
	atomic_uint finite_peak_green_bits;
	atomic_uint finite_peak_blue_bits;
	atomic_uint total_importance_weight_bits;
	atomic_uint reserved;
};

struct MaterialRecord {
    float4 albedo_metallic;
    float4 emission_roughness;
	float4 uv_scale_offset;
	float4 metallic_texture_channel;
	float4 roughness_texture_channel;
	float4 ao_texture_channel;
	float4 material_factors; // normal scale, AO strength, alpha cutoff, emission texture scale.
	float albedo_alpha;
	float specular;
	float2 thin_transmission_ior;
	uint alpha_occupancy_texture_index;
	uint face_flags;
	uint albedo_texture_index;
	uint normal_texture_index;
	uint orm_texture_index;
	uint metallic_texture_index;
	uint roughness_texture_index;
	uint ao_texture_index;
	uint emission_texture_index;
	uint opacity_texture_index;
	uint visibility_mask;
	uint flags; // bit 0 alpha mask, bit 1 packed ORM, bit 2 additive emission.
	uint generation_low;
	uint generation_high;
};

struct MaterialDiagnosticAtomic {
	atomic_uint alpha_candidates;
	atomic_uint alpha_rejections;
	atomic_uint alpha_candidate_exhaustions;
	atomic_uint generation_rejects;
	atomic_uint mixed_intersections;
	atomic_uint split_opaque_queries[4];
	atomic_uint split_opaque_hits[4];
	atomic_uint split_alpha_queries[4];
	atomic_uint split_alpha_hits[4];
	atomic_uint split_alpha_rejections[4];
	atomic_uint split_mixed_fallbacks[4];
	atomic_uint rear_opaque_hits;
	atomic_uint alpha_primary_candidates;
	atomic_uint alpha_primary_rejections;
	atomic_uint alpha_visibility_candidates;
	atomic_uint alpha_visibility_rejections;
	atomic_uint alpha_reflection_candidates;
	atomic_uint alpha_reflection_rejections;
	atomic_uint alpha_indirect_candidates;
	atomic_uint alpha_indirect_rejections;
	atomic_uint alpha_max_candidates_per_ray;
	atomic_uint occupancy_empty_rejections;
	atomic_uint occupancy_opaque_accepts;
	atomic_uint occupancy_mixed_samples;
	atomic_uint metalfx_reactive_opaque_pixels;
	atomic_uint metalfx_reactive_rejected_pixels;
	atomic_uint invalid_pdf_samples;
	atomic_uint nonfinite_lobe_samples;
	atomic_uint rejected_energy_samples;
	atomic_uint primary_valid_pixels;
	atomic_uint primary_invalid_pixels;
	atomic_uint primary_lit_pixels;
	atomic_uint primary_analytic_selected;
	atomic_uint primary_analytic_contributed;
	atomic_uint primary_analytic_visibility_tests;
	atomic_uint stage_raw_emission;
	atomic_uint stage_raw_emissive;
	atomic_uint stage_raw_analytic;
	atomic_uint stage_raw_indirect;
	atomic_uint stage_trace_combined;
	atomic_uint stage_spatial;
	atomic_uint stage_temporal_input;
	atomic_uint stage_temporal_output;
	atomic_uint stage_composite_base;
	atomic_uint stage_composite_input;
	atomic_uint stage_composite_output;
	atomic_uint stage_temporal_reused_pixels;
	atomic_uint stage_temporal_rejected_pixels;
	atomic_uint stage_temporal_history_samples;
	atomic_uint stage_temporal_second_moment;
	atomic_uint stage_temporal_variance;
	atomic_uint reusable_path_staged;
	atomic_uint reusable_path_updates;
	atomic_uint reusable_path_queries;
	atomic_uint reusable_path_valid_candidates;
	atomic_uint reusable_path_reused_candidates;
	atomic_uint reusable_path_rejections;
	atomic_uint reusable_path_occupied;
	atomic_uint reusable_path_invalid_record;
	atomic_uint reusable_path_endpoint_blocked;
	atomic_uint reusable_path_shading_invalid;
	atomic_uint reusable_path_zero_target;
	atomic_uint reusable_path_invalid_weight;
	atomic_uint reusable_path_considered;
	atomic_uint reusable_path_accepted;
	atomic_uint reusable_path_selected;
	atomic_uint reusable_path_reevaluations;
	atomic_uint reusable_path_reconnection_visibility;
	atomic_uint restir_gi_current_candidates;
	atomic_uint restir_gi_reused_candidates;
	atomic_uint restir_gi_selected_reuse;
	atomic_uint reusable_path_lighting_reevaluations;
	atomic_uint reusable_path_environment_reevaluations;
	atomic_uint bidirectional_caustic_candidates;
	atomic_uint bidirectional_caustic_valid;
	atomic_uint bidirectional_caustic_contributed;
	atomic_uint bidirectional_caustic_visibility_rays;
	atomic_uint bidirectional_caustic_rejections;
	atomic_uint bidirectional_caustic_nonfinite_or_pdf_failures;
	atomic_uint direct_candidate_evaluations;
	atomic_uint direct_selected_visibility;
	atomic_uint direct_temporal_reuse;
	atomic_uint direct_spatial_reuse;
	atomic_uint gi_fresh_rays;
	atomic_uint reflection_rays;
	atomic_uint gi_converged_skips;
	atomic_uint reflection_converged_skips;
	atomic_uint reflection_environment_misses;
	atomic_uint reflection_environment_contributions;
	atomic_uint reflection_validation_reprojection_rejections;
	atomic_uint reflection_validation_surface_rejections;
	atomic_uint reflection_validation_motion_rejections;
	atomic_uint reflection_validation_refreshes;
	atomic_uint trace_compaction_inactive_pixels;
	atomic_uint trace_compaction_active_pixels;
	atomic_uint trace_compaction_need_direct;
	atomic_uint trace_compaction_need_gi;
	atomic_uint trace_compaction_need_reflection;
	atomic_uint trace_compaction_need_exact_alpha;
	atomic_uint trace_compaction_need_complex;
	atomic_uint trace_compaction_enqueued_direct;
	atomic_uint trace_compaction_enqueued_gi;
	atomic_uint trace_compaction_enqueued_reflection;
	atomic_uint trace_compaction_enqueued_exact_alpha;
	atomic_uint trace_compaction_enqueued_complex;
	atomic_uint trace_compaction_dispatched_direct;
	atomic_uint trace_compaction_dispatched_gi;
	atomic_uint trace_compaction_dispatched_reflection;
	atomic_uint trace_compaction_dispatched_exact_alpha;
	atomic_uint trace_compaction_dispatched_complex;
};

// These bits are deliberately shared by classification and trace. A compact
// entry retains the complete mask even though its queue is selected by the
// highest-cost applicable bit.
constant uint TRACE_NEED_DIRECT = 1u;
constant uint TRACE_NEED_GI = 2u;
constant uint TRACE_NEED_REFLECTION = 4u;
constant uint TRACE_NEED_EXACT_ALPHA = 8u;
constant uint TRACE_NEED_COMPLEX = 16u;

struct TraceCompactEntry {
	uint pixel_index;
	uint need_mask;
};

struct TraceCompactContext {
	uint enabled;
	uint queue_index;
	uint queue_capacity;
	uint reserved;
};

struct TraceIndirectArguments {
	uint threadgroupsPerGrid[3];
};

enum AlphaRayClass {
	ALPHA_RAY_CLASS_PRIMARY,
	ALPHA_RAY_CLASS_VISIBILITY,
	ALPHA_RAY_CLASS_REFLECTION,
	ALPHA_RAY_CLASS_INDIRECT,
};

struct AlphaIntersectionPayload {
	float ray_spread;
	uint material_texture_capacity;
	uint ray_class;
	uint candidate_count;
	uint rejection_count;
	uint occupancy_empty_rejections;
	uint occupancy_opaque_accepts;
	uint occupancy_mixed_samples;
};

struct GeometryRecord {
    device const uchar *vertex_data;
    device const uchar *index_data;
    device const uchar *attribute_data;
    uint vertex_count;
	uint index_count;
    uint index_type;
    uint position_stride;
    uint normal_offset;
    uint normal_stride;
    uint has_normals;
	uint compressed;
	uint attribute_stride;
	uint uv_offset;
	uint has_uv;
	uint instance_identity_low;
	uint instance_identity_high;
	uint has_tangents;
    float4x4 normal_from_object;
	float4x4 world_from_object;
	float4 position_scale;
	float4 position_offset;
};

struct EmissiveRecord {
	uint instance_id;
	uint triangle_count;
	float selection_pdf;
	float cdf;
	uint source_identity;
	uint padding;
};

// Fixed slots make the distribution and the current/previous source mapping
// deterministic without a CPU readback. A zero-weight slot is not selectable.
struct EmissiveTriangleRecord {
	uint instance_id;
	uint primitive_id;
	uint instance_identity_low;
	uint instance_identity_high;
	float area;
	float weight;
	float selection_pdf;
	float cdf;
};

// CPU extraction admits only opaque, static, canonical, factor-only metallic
// delta surfaces. The shader still resolves the current triangle and material
// before contribution, so this table is a proposal domain rather than a cache.
struct BidirectionalCausticMirrorTriangleRecord {
	uint instance_id;
	uint primitive_id;
	uint instance_identity_low;
	uint instance_identity_high;
	uint material_generation_low;
	uint material_generation_high;
	uint visibility_mask;
	uint padding;
};

struct EmissiveTriangleBuildParameters {
	uint emissive_count;
	uint triangle_capacity;
	uint block_count;
	uint padding;
};

struct PunctualLightRecord {
	float4 position_range;
	float4 radiance_attenuation;
	float4 direction_spot_outer;
	float4 area_u_spot_attenuation;
	float4 area_v;
	uint type;
	uint cull_mask;
	uint source_identity;
	uint shadow_caster_mask;
	uint2 stable_identity;
	uint flags; // bit 0 shadow enabled, bit 1 negative.
	float shadow_opacity;
	float specular_amount;
	float indirect_energy;
	uint2 padding;
};

struct PortalRecord {
	float4 center_weight;
	float4 axis_u;
	float4 axis_v;
};

static uint triangle_vertex_index(constant GeometryRecord &geometry, uint primitive_id, uint corner) {
	uint index = primitive_id * 3u + corner;
	if (geometry.index_data == nullptr) return index;
	if (geometry.index_type == 16u) return reinterpret_cast<device const ushort *>(geometry.index_data)[index];
	return reinterpret_cast<device const uint *>(geometry.index_data)[index];
}

static float3 world_vertex_position(constant GeometryRecord &geometry, uint vertex_index) {
	float3 object_position;
	if (geometry.compressed != 0u) {
		device const ushort4 *packed = reinterpret_cast<device const ushort4 *>(geometry.vertex_data + vertex_index * geometry.position_stride);
		object_position = float4(*packed).xyz / 65535.0f * geometry.position_scale.xyz + geometry.position_offset.xyz;
	} else {
		device const float3 *position = reinterpret_cast<device const float3 *>(geometry.vertex_data + vertex_index * geometry.position_stride);
		object_position = *position;
	}
	return (geometry.world_from_object * float4(object_position, 1.0f)).xyz;
}

static uint geometry_triangle_count(constant GeometryRecord &geometry) {
	return (geometry.index_data != nullptr ? geometry.index_count : geometry.vertex_count) / 3u;
}

// Stage 1: one deterministic output slot per input triangle. The input
// ordering is only an implementation detail: temporal reuse validates the
// retained instance identity and primitive ID, then remaps against this table.
kernel void emissive_triangle_build(
		constant GeometryRecord *geometries [[buffer(0)]],
		constant MaterialRecord *materials [[buffer(1)]],
		constant EmissiveTriangleBuildParameters &parameters [[buffer(2)]],
		device EmissiveTriangleRecord *triangles [[buffer(3)]],
		constant EmissiveRecord *emissives [[buffer(4)]],
		uint slot [[thread_position_in_grid]]) {
	if (slot >= parameters.triangle_capacity) return;
	EmissiveTriangleRecord record = {};
	uint remaining = slot;
	// Slots enumerate only selectable emissive sources. A large non-emissive
	// scene must never consume this bounded table before a ceiling emitter.
	for (uint emitter_index = 0u; emitter_index < parameters.emissive_count; emitter_index++) {
		const EmissiveRecord emitter = emissives[emitter_index];
		const uint instance = emitter.instance_id;
		const uint count = emitter.triangle_count;
		if (remaining >= count) { remaining -= count; continue; }
		const MaterialRecord material = materials[instance];
		const float3 emission = material.emission_roughness.rgb;
		if (all(emission <= float3(0.0001f))) break;
		constant GeometryRecord &geometry = geometries[instance];
		const float3 p0 = world_vertex_position(geometry, triangle_vertex_index(geometry, remaining, 0u));
		const float3 p1 = world_vertex_position(geometry, triangle_vertex_index(geometry, remaining, 1u));
		const float3 p2 = world_vertex_position(geometry, triangle_vertex_index(geometry, remaining, 2u));
		const float area = 0.5f * length(cross(p1 - p0, p2 - p0));
		const float luminance = max(dot(emission, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
		if (area > 0.000001f && luminance > 0.0f && isfinite(area) && isfinite(luminance)) {
			record.instance_id = instance;
			record.primitive_id = remaining;
			record.instance_identity_low = geometry.instance_identity_low;
			record.instance_identity_high = geometry.instance_identity_high;
			record.area = area;
			record.weight = area * luminance;
		}
		break;
	}
	triangles[slot] = record;
}

// Stage 2 uses fixed 256-entry blocks. The small in-block serial sum is
// deterministic and avoids atomics/readback; the next stage scans block sums.
kernel void emissive_triangle_block_scan(
		device EmissiveTriangleRecord *triangles [[buffer(0)]],
		device float *block_sums [[buffer(1)]],
		constant EmissiveTriangleBuildParameters &parameters [[buffer(2)]],
		uint slot [[thread_position_in_grid]]) {
	if (slot >= parameters.triangle_capacity) return;
	const uint begin = (slot / 256u) * 256u;
	float prefix = 0.0f;
	for (uint i = begin; i <= slot; i++) prefix += max(triangles[i].weight, 0.0f);
	triangles[slot].cdf = prefix;
	const uint block_end = min(begin + 256u, parameters.triangle_capacity) - 1u;
	if (slot == block_end) block_sums[slot / 256u] = prefix;
}

kernel void emissive_triangle_block_prefix(
		device float *block_sums [[buffer(0)]],
		device float *distribution_total [[buffer(1)]],
		constant EmissiveTriangleBuildParameters &parameters [[buffer(2)]],
		uint slot [[thread_position_in_grid]]) {
	if (slot != 0u) return;
	float prefix = 0.0f;
	for (uint block = 0u; block < parameters.block_count; block++) {
		const float weight = block_sums[block];
		block_sums[block] = prefix;
		prefix += max(weight, 0.0f);
	}
	distribution_total[0] = prefix;
}

kernel void emissive_triangle_finalize(
		device EmissiveTriangleRecord *triangles [[buffer(0)]],
		device float *block_offsets [[buffer(1)]],
		device float *distribution_total [[buffer(2)]],
		constant EmissiveTriangleBuildParameters &parameters [[buffer(3)]],
		uint slot [[thread_position_in_grid]]) {
	if (slot >= parameters.triangle_capacity) return;
	const float total = distribution_total[0];
	EmissiveTriangleRecord record = triangles[slot];
	if (total > 0.0f && isfinite(total) && record.weight > 0.0f) {
		record.selection_pdf = record.weight / total;
		record.cdf = (block_offsets[slot / 256u] + record.cdf) / total;
	} else {
		record.selection_pdf = 0.0f;
		record.cdf = total > 0.0f ? (block_offsets[slot / 256u] + record.cdf) / total : 0.0f;
	}
	triangles[slot] = record;
}

static bool valid_direction(float3 value) {
	return all(isfinite(value)) && dot(value, value) > 0.000000000001f;
}

static float3 normalize_or_fallback(float3 value, float3 fallback) {
	if (valid_direction(value)) return normalize(value);
	if (valid_direction(fallback)) return normalize(fallback);
	return float3(0.0f, 1.0f, 0.0f);
}

// Keep these decoders byte-for-byte equivalent in meaning to Godot's
// uint_to_vec2(), oct_to_vec3(), and decode_uint_oct_to_tang(). Tangent Y
// stores handedness; it is not the normal-map green channel.
static float2 decode_uint_to_vec2(uint packed) {
	return float2(packed & 0xffffu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
}

static float3 oct_to_vec3(float2 oct) {
	float3 value = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
	const float fold = max(-value.z, 0.0f);
	value.xy += fold * -sign(value.xy);
	return normalize_or_fallback(value, float3(0.0f, 0.0f, 1.0f));
}

static float3 decode_oct_normal(uint packed) {
	return oct_to_vec3(decode_uint_to_vec2(packed));
}

static float4 decode_oct_tangent(uint packed) {
	const float2 oct_sign_encoded = decode_uint_to_vec2(packed);
	const float2 oct = float2(oct_sign_encoded.x, abs(oct_sign_encoded.y) * 2.0f - 1.0f);
	return float4(oct_to_vec3(oct), sign(oct_sign_encoded.y));
}

static void axis_angle_to_tbn(float3 axis, float angle, thread float3 &tangent, thread float3 &binormal, thread float3 &normal) {
	const float cosine = cos(angle);
	const float sine = sin(angle);
	const float3 one_minus_cosine_axis = (1.0f - cosine) * axis;
	const float3 sine_axis = sine * axis;
	tangent = one_minus_cosine_axis.xxx * axis + float3(cosine, -sine_axis.z, sine_axis.y);
	binormal = one_minus_cosine_axis.yyy * axis + float3(sine_axis.z, cosine, -sine_axis.x);
	normal = one_minus_cosine_axis.zzz * axis + float3(-sine_axis.y, sine_axis.x, cosine);
}

static void decode_vertex_tbn(constant GeometryRecord &geometry, uint vertex_index, thread float3 &normal, thread float3 &tangent, thread float3 &binormal) {
	device const uint *packed_normal = reinterpret_cast<device const uint *>(geometry.vertex_data + geometry.normal_offset + vertex_index * geometry.normal_stride);
	if (geometry.compressed != 0u) {
		const float3 axis = decode_oct_normal(*packed_normal);
		device const ushort4 *packed_position = reinterpret_cast<device const ushort4 *>(geometry.vertex_data + vertex_index * geometry.position_stride);
		float encoded_angle = float((*packed_position).w) / 65535.0f;
		const float binormal_sign = encoded_angle > 0.5f ? 1.0f : -1.0f;
		const float angle = abs(encoded_angle * 2.0f - 1.0f) * 3.14159265358979323846f;
		axis_angle_to_tbn(axis, angle, tangent, binormal, normal);
		binormal *= binormal_sign;
	} else {
		normal = decode_oct_normal(*packed_normal);
		if (geometry.has_tangents != 0u && geometry.normal_stride >= 8u) {
			const float4 decoded_tangent = decode_oct_tangent(*(packed_normal + 1));
			tangent = decoded_tangent.xyz;
			binormal = cross(normal, tangent) * decoded_tangent.w;
		} else {
			tangent = float3(0.0f);
			binormal = float3(0.0f);
		}
	}
}

static float3 intersection_normal(constant GeometryRecord *geometry_records, uint instance_id, uint primitive_id, float2 barycentric, float3 fallback) {
	constant GeometryRecord &geometry = geometry_records[instance_id];
	if (geometry.has_normals == 0u) return normalize_or_fallback(fallback, float3(0.0f, 1.0f, 0.0f));
	float3 normals[3];
	for (uint corner = 0u; corner < 3u; corner++) {
		float3 unused_tangent;
		float3 unused_binormal;
		decode_vertex_tbn(geometry, triangle_vertex_index(geometry, primitive_id, corner), normals[corner], unused_tangent, unused_binormal);
	}
	const float3 weights = float3(1.0f - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
	const float3 object_normal = normals[0] * weights.x + normals[1] * weights.y + normals[2] * weights.z;
	return normalize_or_fallback((geometry.normal_from_object * float4(object_normal, 0.0f)).xyz, fallback);
}

static float2 intersection_uv(constant GeometryRecord *geometry_records, uint instance_id, uint primitive_id, float2 barycentric) {
	constant GeometryRecord &geometry = geometry_records[instance_id];
	if (geometry.has_uv == 0u || geometry.attribute_data == nullptr) return float2(0.0f);
	const uint indices[3] = {
		triangle_vertex_index(geometry, primitive_id, 0u),
		triangle_vertex_index(geometry, primitive_id, 1u),
		triangle_vertex_index(geometry, primitive_id, 2u),
	};
	float2 uvs[3];
	for (uint corner = 0u; corner < 3u; corner++) {
		device const uchar *attribute = geometry.attribute_data + geometry.uv_offset + indices[corner] * geometry.attribute_stride;
		if (geometry.compressed != 0u) {
			uvs[corner] = float2(*reinterpret_cast<device const ushort2 *>(attribute)) / 65535.0f;
		} else {
			uvs[corner] = *reinterpret_cast<device const float2 *>(attribute);
		}
	}
	const float3 weights = float3(1.0f - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
	return uvs[0] * weights.x + uvs[1] * weights.y + uvs[2] * weights.z;
}

// Ray queries have no hardware derivatives. Estimate a footprint in texture
// space from the actual world/UV triangle Jacobian and an explicitly propagated
// cone radius instead of forcing the highest-frequency mip at every secondary
// hit. Degenerate mapping and single-level textures deliberately retain mip 0.
static float intersection_texture_lod(
		constant GeometryRecord &geometry,
		uint instance_id,
		uint primitive_id,
		texture2d<float, access::sample> texture,
		float ray_distance,
		float ray_spread) {
	if (geometry.has_uv == 0u || geometry.attribute_data == nullptr || texture.get_num_mip_levels() <= 1u || ray_distance <= 0.0f || ray_spread <= 0.0f) return 0.0f;
	const uint i0 = triangle_vertex_index(geometry, primitive_id, 0u);
	const uint i1 = triangle_vertex_index(geometry, primitive_id, 1u);
	const uint i2 = triangle_vertex_index(geometry, primitive_id, 2u);
	const float3 p0 = world_vertex_position(geometry, i0);
	const float3 dpdu = world_vertex_position(geometry, i1) - p0;
	const float3 dpdv = world_vertex_position(geometry, i2) - p0;
	const float2 uv0 = intersection_uv(&geometry, instance_id, primitive_id, float2(0.0f));
	const float2 uv1 = intersection_uv(&geometry, instance_id, primitive_id, float2(1.0f, 0.0f));
	const float2 uv2 = intersection_uv(&geometry, instance_id, primitive_id, float2(0.0f, 1.0f));
	const float uv_area_twice = abs(uv1.x * uv2.y - uv1.y * uv2.x + uv2.x * uv0.y - uv2.y * uv0.x + uv0.x * uv1.y - uv0.y * uv1.x);
	const float world_area_twice = length(cross(dpdu, dpdv));
	if (!(uv_area_twice > 0.0000001f) || !(world_area_twice > 0.0000001f) || !isfinite(uv_area_twice) || !isfinite(world_area_twice)) return 0.0f;
	const float world_units_per_uv = sqrt(world_area_twice / uv_area_twice);
	if (!(world_units_per_uv > 0.000001f) || !isfinite(world_units_per_uv)) return 0.0f;
	const float footprint_uv = max(ray_distance * ray_spread / world_units_per_uv, 0.0f);
	const float texel_footprint = footprint_uv * float(max(texture.get_width(0), texture.get_height(0)));
	if (!(texel_footprint > 1.0f) || !isfinite(texel_footprint)) return 0.0f;
	return clamp(log2(texel_footprint), 0.0f, float(texture.get_num_mip_levels() - 1u));
}

static float4 material_texture_sample(
		uint texture_index,
		float4 fallback,
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float2 barycentric,
		float2 uv_scale,
		float2 uv_offset,
		float ray_distance,
		float ray_spread,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		uint material_texture_capacity) {
	if (texture_index >= material_texture_capacity) return fallback;
	const float2 uv = intersection_uv(geometry_records, instance_id, primitive_id, barycentric) * uv_scale + uv_offset;
#if HYBRID_BINDLESS_MATERIALS
	const texture2d<float, access::sample> texture = material_textures.textures[texture_index];
#else
	const texture2d<float, access::sample> texture = material_textures[texture_index];
#endif
	const float lod = intersection_texture_lod(geometry_records[instance_id], instance_id, primitive_id, texture, ray_distance, ray_spread);
	return texture.sample(material_sampler, uv, level(lod));
}

struct MaterialSample {
	float3 albedo;
	float3 emission;
	float metallic;
	float roughness;
	float ambient_occlusion;
};

static MaterialSample sample_material(
		thread const MaterialRecord &material,
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float2 barycentric,
		float ray_distance,
		float ray_spread,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		uint material_texture_capacity) {
	MaterialSample sample;
	const float2 uv_scale = material.uv_scale_offset.xy;
	const float2 uv_offset = material.uv_scale_offset.zw;
	const float4 albedo_texel = material_texture_sample(material.albedo_texture_index, float4(1.0f), geometry_records, instance_id, primitive_id, barycentric, uv_scale, uv_offset, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity);
	sample.albedo = material.albedo_metallic.rgb * albedo_texel.rgb;
	sample.metallic = material.albedo_metallic.a;
	sample.roughness = material.emission_roughness.a;
	sample.ambient_occlusion = 1.0f;
	if ((material.flags & 2u) != 0u) {
		const float4 orm = material_texture_sample(material.orm_texture_index, float4(1.0f), geometry_records, instance_id, primitive_id, barycentric, uv_scale, uv_offset, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity);
		sample.ambient_occlusion = orm.r;
		sample.roughness = orm.g;
		sample.metallic = orm.b;
	} else {
		const float4 metallic_texel = material_texture_sample(material.metallic_texture_index, float4(1.0f), geometry_records, instance_id, primitive_id, barycentric, uv_scale, uv_offset, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity);
		const float4 roughness_texel = material_texture_sample(material.roughness_texture_index, float4(1.0f), geometry_records, instance_id, primitive_id, barycentric, uv_scale, uv_offset, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity);
		const float4 ao_texel = material_texture_sample(material.ao_texture_index, float4(1.0f), geometry_records, instance_id, primitive_id, barycentric, uv_scale, uv_offset, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity);
		sample.metallic *= dot(metallic_texel, material.metallic_texture_channel);
		sample.roughness *= dot(roughness_texel, material.roughness_texture_channel);
		sample.ambient_occlusion = dot(ao_texel, material.ao_texture_channel);
	}
	const float3 emission_texel = material_texture_sample(material.emission_texture_index, float4((material.flags & 4u) != 0u ? 0.0f : 1.0f), geometry_records, instance_id, primitive_id, barycentric, uv_scale, uv_offset, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity).rgb;
	sample.emission = (material.flags & 4u) != 0u ? material.emission_roughness.rgb + emission_texel * material.material_factors.w : material.emission_roughness.rgb * emission_texel;
	sample.metallic = clamp(sample.metallic, 0.0f, 1.0f);
	sample.roughness = clamp(sample.roughness, 0.0f, 1.0f);
	sample.ambient_occlusion = mix(1.0f, clamp(sample.ambient_occlusion, 0.0f, 1.0f), clamp(material.material_factors.y, 0.0f, 1.0f));
	return sample;
}

static float sample_material_alpha(
		thread const MaterialRecord &material,
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float2 barycentric,
		float ray_distance,
		float ray_spread,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		uint material_texture_capacity) {
	return material_texture_sample(material.opacity_texture_index, float4(1.0f), geometry_records, instance_id, primitive_id, barycentric, material.uv_scale_offset.xy, material.uv_scale_offset.zw, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity).a * material.albedo_alpha;
}

// This duplicates only the strict-alpha sampling path for use by Metal's
// intersection-function table. Intersection functions may receive buffers but
// not texture arguments, so their texture table is an argument buffer.
static float4 alpha_intersection_texture_sample(
		uint texture_index,
		float4 fallback,
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float2 barycentric,
		float2 uv_scale,
		float2 uv_offset,
		float ray_distance,
		float ray_spread,
		constant AlphaMaterialTextureTable &material_textures,
		uint material_texture_capacity) {
	if (texture_index >= material_texture_capacity) return fallback;
	const float2 uv = intersection_uv(geometry_records, instance_id, primitive_id, barycentric) * uv_scale + uv_offset;
	const texture2d<float, access::sample> texture = material_textures.textures[texture_index];
	constexpr sampler material_sampler(coord::normalized, address::repeat, filter::linear, mip_filter::linear);
	const float lod = intersection_texture_lod(geometry_records[instance_id], instance_id, primitive_id, texture, ray_distance, ray_spread);
	return texture.sample(material_sampler, uv, level(lod));
}

[[intersection(triangle, raytracing::triangle_data, raytracing::instancing)]] bool hybrid_alpha_triangle_intersection(
		uint primitive_id [[primitive_id]],
		uint user_instance_id [[user_instance_id]],
		float2 barycentric [[barycentric_coord]],
		float distance [[distance]],
		constant MaterialRecord *materials [[buffer(0)]],
		constant GeometryRecord *geometry_records [[buffer(1)]],
		constant AlphaMaterialTextureTable &material_textures [[buffer(3)]],
		ray_data AlphaIntersectionPayload &payload [[payload]]) {
	const MaterialRecord material = materials[user_instance_id];
	if ((material.flags & 1u) == 0u) return true;
	payload.candidate_count++;
	const uint material_texture_capacity = min(payload.material_texture_capacity, HYBRID_ALPHA_MATERIAL_TEXTURES);
	if (material.alpha_occupancy_texture_index < material_texture_capacity) {
		const float2 uv = intersection_uv(geometry_records, user_instance_id, primitive_id, barycentric) * material.uv_scale_offset.xy + material.uv_scale_offset.zw;
		const texture2d<float, access::sample> occupancy_texture = material_textures.textures[material.alpha_occupancy_texture_index];
		constexpr sampler occupancy_sampler(coord::normalized, address::repeat, filter::nearest, mip_filter::nearest);
		const float lod = intersection_texture_lod(geometry_records[user_instance_id], user_instance_id, primitive_id, occupancy_texture, distance, payload.ray_spread);
		const float occupancy = occupancy_texture.sample(occupancy_sampler, uv, level(lod)).r;
		if (occupancy < 0.25f) {
			payload.rejection_count++;
			payload.occupancy_empty_rejections++;
			return false;
		}
		if (occupancy > 0.75f) {
			payload.occupancy_opaque_accepts++;
			return true;
		}
		payload.occupancy_mixed_samples++;
	}
	const float alpha = alpha_intersection_texture_sample(material.opacity_texture_index, float4(1.0f), geometry_records, user_instance_id, primitive_id, barycentric, material.uv_scale_offset.xy, material.uv_scale_offset.zw, distance, payload.ray_spread, material_textures, material_texture_capacity).a * material.albedo_alpha;
	if (alpha >= material.material_factors.z) return true;
	payload.rejection_count++;
	return false;
}

static bool intersection_authored_tangent_basis(
		constant GeometryRecord &geometry,
		uint primitive_id,
		float2 barycentric,
		float3 base_normal,
		thread float3 &tangent,
		thread float3 &binormal) {
	if (geometry.has_tangents == 0u || geometry.has_normals == 0u) return false;
	float3 object_tangents[3];
	float3 object_binormals[3];
	for (uint corner = 0u; corner < 3u; corner++) {
		float3 object_normal;
		decode_vertex_tbn(geometry, triangle_vertex_index(geometry, primitive_id, corner), object_normal, object_tangents[corner], object_binormals[corner]);
		if (!valid_direction(object_normal) || !valid_direction(object_tangents[corner]) || !valid_direction(object_binormals[corner])) return false;
	}
	const float3 weights = float3(1.0f - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
	const float3 object_tangent = object_tangents[0] * weights.x + object_tangents[1] * weights.y + object_tangents[2] * weights.z;
	const float3 object_binormal = object_binormals[0] * weights.x + object_binormals[1] * weights.y + object_binormals[2] * weights.z;
	float3 world_tangent = (geometry.world_from_object * float4(object_tangent, 0.0f)).xyz;
	const float3 world_binormal = (geometry.world_from_object * float4(object_binormal, 0.0f)).xyz;
	if (!valid_direction(base_normal) || !valid_direction(world_tangent) || !valid_direction(world_binormal)) return false;
	world_tangent -= base_normal * dot(base_normal, world_tangent);
	if (!valid_direction(world_tangent)) return false;
	tangent = normalize(world_tangent);
	const float orientation = dot(cross(base_normal, tangent), world_binormal);
	if (!isfinite(orientation) || abs(orientation) <= 0.0000001f) return false;
	binormal = normalize_or_fallback(cross(base_normal, tangent) * (orientation < 0.0f ? -1.0f : 1.0f), float3(0.0f));
	return valid_direction(binormal);
}

static bool intersection_uv_derivative_tangent_basis(
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float3 base_normal,
		float2 uv_scale,
		thread float3 &tangent,
		thread float3 &binormal) {
	constant GeometryRecord &geometry = geometry_records[instance_id];
	const uint i0 = triangle_vertex_index(geometry, primitive_id, 0u);
	const uint i1 = triangle_vertex_index(geometry, primitive_id, 1u);
	const uint i2 = triangle_vertex_index(geometry, primitive_id, 2u);
	const float3 p0 = world_vertex_position(geometry, i0);
	const float3 p1 = world_vertex_position(geometry, i1);
	const float3 p2 = world_vertex_position(geometry, i2);
	const float2 uv0 = intersection_uv(geometry_records, instance_id, primitive_id, float2(0.0f));
	const float2 uv1 = intersection_uv(geometry_records, instance_id, primitive_id, float2(1.0f, 0.0f));
	const float2 uv2 = intersection_uv(geometry_records, instance_id, primitive_id, float2(0.0f, 1.0f));
	const float2 duv1 = (uv1 - uv0) * uv_scale;
	const float2 duv2 = (uv2 - uv0) * uv_scale;
	const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
	if (abs(determinant) <= 0.0000001f || !isfinite(determinant)) return false;
	float3 world_tangent = ((p1 - p0) * duv2.y - (p2 - p0) * duv1.y) / determinant;
	const float3 world_binormal = ((p2 - p0) * duv1.x - (p1 - p0) * duv2.x) / determinant;
	if (!valid_direction(base_normal) || !valid_direction(world_tangent) || !valid_direction(world_binormal)) return false;
	world_tangent -= base_normal * dot(base_normal, world_tangent);
	if (!valid_direction(world_tangent)) return false;
	tangent = normalize(world_tangent);
	const float orientation = dot(cross(base_normal, tangent), world_binormal);
	if (!isfinite(orientation) || abs(orientation) <= 0.0000001f) return false;
	binormal = normalize_or_fallback(cross(base_normal, tangent) * (orientation < 0.0f ? -1.0f : 1.0f), float3(0.0f));
	return valid_direction(binormal);
}

static float3 material_shading_normal(
		thread const MaterialRecord &material,
		constant GeometryRecord *geometry_records,
		uint instance_id,
		uint primitive_id,
		float2 barycentric,
		float3 base_normal,
		float ray_distance,
		float ray_spread,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		uint material_texture_capacity) {
	if (material.normal_texture_index >= material_texture_capacity || geometry_records[instance_id].has_uv == 0u) return base_normal;
	constant GeometryRecord &geometry = geometry_records[instance_id];
	float3 tangent;
	float3 binormal;
	if (!intersection_authored_tangent_basis(geometry, primitive_id, barycentric, base_normal, tangent, binormal) &&
			!intersection_uv_derivative_tangent_basis(geometry_records, instance_id, primitive_id, base_normal, material.uv_scale_offset.xy, tangent, binormal)) {
		return base_normal;
	}
	float3 tangent_normal = material_texture_sample(material.normal_texture_index, float4(0.5f, 0.5f, 1.0f, 1.0f), geometry_records, instance_id, primitive_id, barycentric, material.uv_scale_offset.xy, material.uv_scale_offset.zw, ray_distance, ray_spread, material_textures, material_sampler, material_texture_capacity).xyz * 2.0f - 1.0f;
	tangent_normal.xy *= material.material_factors.x;
	if (!all(isfinite(tangent_normal))) return base_normal;
	// Godot's normal maps use the OpenGL (+Y) convention. Handedness belongs
	// to the authored binormal; the sampled green channel is deliberately not inverted.
	return normalize_or_fallback(tangent * tangent_normal.x + binormal * tangent_normal.y + base_normal * tangent_normal.z, base_normal);
}

struct HybridIntersection {
	raytracing::intersection_type type;
	uint instance_id;
	uint primitive_id;
	float2 triangle_barycentric_coord;
	float distance;
};

static HybridIntersection hybrid_intersect(
		raytracing::ray ray,
		raytracing::instance_acceleration_structure scene,
		uint mask,
		float ray_spread,
		uint alpha_ray_class,
		constant Parameters &parameters,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		device MaterialDiagnosticAtomic &diagnostic,
		raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &fast_intersector) {
	HybridIntersection result = { raytracing::intersection_type::none, 0u, 0u, float2(0.0f), 0.0f };
	if (parameters.alpha_mask_instance_count == 0u) {
		auto hit = fast_intersector.intersect(ray, scene, mask);
		result.type = hit.type;
		if (hit.type == raytracing::intersection_type::triangle) {
			result.instance_id = hit.instance_id;
			result.primitive_id = hit.primitive_id;
			result.triangle_barycentric_coord = hit.triangle_barycentric_coord;
			result.distance = hit.distance;
		}
		return result;
	}
	atomic_fetch_add_explicit(&diagnostic.mixed_intersections, 1u, memory_order_relaxed);
	AlphaIntersectionPayload payload = { ray_spread, min(parameters.material_texture_capacity, HYBRID_ALPHA_MATERIAL_TEXTURES), alpha_ray_class, 0u, 0u, 0u, 0u, 0u };
	auto hit = fast_intersector.intersect(ray, scene, mask, alpha_intersection_table, payload);
	if (payload.candidate_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_candidates, payload.candidate_count, memory_order_relaxed);
	if (payload.rejection_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_rejections, payload.rejection_count, memory_order_relaxed);
	if (payload.occupancy_empty_rejections > 0u) atomic_fetch_add_explicit(&diagnostic.occupancy_empty_rejections, payload.occupancy_empty_rejections, memory_order_relaxed);
	if (payload.occupancy_opaque_accepts > 0u) atomic_fetch_add_explicit(&diagnostic.occupancy_opaque_accepts, payload.occupancy_opaque_accepts, memory_order_relaxed);
	if (payload.occupancy_mixed_samples > 0u) atomic_fetch_add_explicit(&diagnostic.occupancy_mixed_samples, payload.occupancy_mixed_samples, memory_order_relaxed);
	if (payload.candidate_count > 0u) atomic_fetch_max_explicit(&diagnostic.alpha_max_candidates_per_ray, payload.candidate_count, memory_order_relaxed);
	switch (payload.ray_class) {
		case ALPHA_RAY_CLASS_PRIMARY:
			if (payload.candidate_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_primary_candidates, payload.candidate_count, memory_order_relaxed);
			if (payload.rejection_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_primary_rejections, payload.rejection_count, memory_order_relaxed);
			break;
		case ALPHA_RAY_CLASS_VISIBILITY:
			if (payload.candidate_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_visibility_candidates, payload.candidate_count, memory_order_relaxed);
			if (payload.rejection_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_visibility_rejections, payload.rejection_count, memory_order_relaxed);
			break;
		case ALPHA_RAY_CLASS_REFLECTION:
			if (payload.candidate_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_reflection_candidates, payload.candidate_count, memory_order_relaxed);
			if (payload.rejection_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_reflection_rejections, payload.rejection_count, memory_order_relaxed);
			break;
		default:
			if (payload.candidate_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_indirect_candidates, payload.candidate_count, memory_order_relaxed);
			if (payload.rejection_count > 0u) atomic_fetch_add_explicit(&diagnostic.alpha_indirect_rejections, payload.rejection_count, memory_order_relaxed);
			break;
	}
	result.type = hit.type;
	if (hit.type == raytracing::intersection_type::triangle) {
		result.instance_id = hit.instance_id;
		result.primitive_id = hit.primitive_id;
		result.triangle_barycentric_coord = hit.triangle_barycentric_coord;
		result.distance = hit.distance;
		if (payload.rejection_count > 0u && (materials[hit.instance_id].flags & 1u) == 0u) atomic_fetch_add_explicit(&diagnostic.rear_opaque_hits, 1u, memory_order_relaxed);
	}
	return result;
}

// A paired traversal is only legal when C++ constructed both disjoint domains.
// The mixed TLAS remains the conservative answer for every partial/unproven
// configuration. The alpha query continues to use the authored intersection
// function; the opaque query intentionally cannot invoke that table.
static HybridIntersection hybrid_intersect_split(
		raytracing::ray ray,
		raytracing::instance_acceleration_structure mixed_scene,
		raytracing::instance_acceleration_structure opaque_scene,
		raytracing::instance_acceleration_structure alpha_scene,
		uint mask,
		float ray_spread,
		uint alpha_ray_class,
		constant Parameters &parameters,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		device MaterialDiagnosticAtomic &diagnostic,
		raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &fast_intersector) {
	if ((parameters.experimental_feature_flags & 16u) == 0u) {
		atomic_fetch_add_explicit(&diagnostic.split_mixed_fallbacks[min(alpha_ray_class, 3u)], 1u, memory_order_relaxed);
		return hybrid_intersect(ray, mixed_scene, mask, ray_spread, alpha_ray_class, parameters, materials, geometry_records, material_textures, material_sampler, diagnostic, alpha_intersection_table, fast_intersector);
	}
	// The ordinary opaque intersector is sufficient for the opaque-only TLAS.
	atomic_fetch_add_explicit(&diagnostic.split_opaque_queries[min(alpha_ray_class, 3u)], 1u, memory_order_relaxed);
	const auto opaque_hit = fast_intersector.intersect(ray, opaque_scene, mask);
	HybridIntersection opaque = { opaque_hit.type, 0u, 0u, float2(0.0f), 0.0f };
	if (opaque_hit.type == raytracing::intersection_type::triangle) {
		opaque.instance_id = opaque_hit.instance_id;
		opaque.primitive_id = opaque_hit.primitive_id;
		opaque.triangle_barycentric_coord = opaque_hit.triangle_barycentric_coord;
		opaque.distance = opaque_hit.distance;
		atomic_fetch_add_explicit(&diagnostic.split_opaque_hits[min(alpha_ray_class, 3u)], 1u, memory_order_relaxed);
	}
	// The alpha-only TLAS is disjoint, so its normal alpha traversal cannot hide
	// an opaque hit and the nearest accepted hit is exact.
	atomic_fetch_add_explicit(&diagnostic.split_alpha_queries[min(alpha_ray_class, 3u)], 1u, memory_order_relaxed);
	const HybridIntersection alpha = hybrid_intersect(ray, alpha_scene, mask, ray_spread, alpha_ray_class, parameters, materials, geometry_records, material_textures, material_sampler, diagnostic, alpha_intersection_table, fast_intersector);
	if (alpha.type == raytracing::intersection_type::triangle) atomic_fetch_add_explicit(&diagnostic.split_alpha_hits[min(alpha_ray_class, 3u)], 1u, memory_order_relaxed);
	if (alpha.type != raytracing::intersection_type::triangle && opaque.type == raytracing::intersection_type::triangle) atomic_fetch_add_explicit(&diagnostic.split_alpha_rejections[min(alpha_ray_class, 3u)], 1u, memory_order_relaxed);
	if (opaque.type != raytracing::intersection_type::triangle) return alpha;
	if (alpha.type != raytracing::intersection_type::triangle) return opaque;
	return alpha.distance < opaque.distance ? alpha : opaque;
}

// Scalar KHR_materials_transmission is represented as a thin dielectric. We
// split the interface energy with Fresnel, retain the transmitted branch, and
// reconnect it through at most two panes. A total-internal-reflection event is
// deliberately a blocker here: the glossy reflection path owns that branch.
static bool thin_dielectric_refraction(float3 incident, float3 surface_normal, float ior, thread float3 &transmitted, thread float &fresnel) {
	const float3 normal = dot(incident, surface_normal) < 0.0f ? surface_normal : -surface_normal;
	const bool entering = dot(incident, surface_normal) < 0.0f;
	const float eta_i = entering ? 1.0f : max(ior, 1.0f);
	const float eta_t = entering ? max(ior, 1.0f) : 1.0f;
	const float eta = eta_i / eta_t;
	const float cosine_i = clamp(dot(-incident, normal), 0.0f, 1.0f);
	const float sine_t2 = eta * eta * max(0.0f, 1.0f - cosine_i * cosine_i);
	const float f0 = (eta_i - eta_t) / max(eta_i + eta_t, 0.00001f);
	const float f0_squared = f0 * f0;
	fresnel = clamp(f0_squared + (1.0f - f0_squared) * pow(1.0f - cosine_i, 5.0f), 0.0f, 1.0f);
	if (!isfinite(sine_t2) || sine_t2 >= 1.0f) return false;
	transmitted = normalize_or_fallback(eta * incident + (eta * cosine_i - sqrt(max(0.0f, 1.0f - sine_t2))) * normal, incident);
	return valid_direction(transmitted) && isfinite(fresnel);
}

static HybridIntersection hybrid_intersect_thin_reconnect(
		thread raytracing::ray &ray,
		raytracing::instance_acceleration_structure scene,
		raytracing::instance_acceleration_structure opaque_scene,
		raytracing::instance_acceleration_structure alpha_scene,
		uint mask,
		float ray_spread,
		uint alpha_ray_class,
		constant Parameters &parameters,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		device MaterialDiagnosticAtomic &diagnostic,
		raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &fast_intersector,
		thread float3 &throughput) {
	throughput = float3(1.0f);
	for (uint pane = 0u; pane < 2u; pane++) {
		const HybridIntersection hit = hybrid_intersect_split(ray, scene, opaque_scene, alpha_scene, mask, ray_spread, alpha_ray_class, parameters, materials, geometry_records, material_textures, material_sampler, diagnostic, alpha_intersection_table, fast_intersector);
		if (hit.type != raytracing::intersection_type::triangle) return hit;
		const MaterialRecord material = materials[hit.instance_id];
		if ((material.flags & 8u) == 0u) return hit;
		float3 geometric_normal = intersection_normal(geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, -ray.direction);
		float3 refracted;
		float fresnel = 1.0f;
		if (!thin_dielectric_refraction(ray.direction, geometric_normal, clamp(material.thin_transmission_ior.y, 1.0f, 4.0f), refracted, fresnel)) return hit;
		const float transmitted_energy = clamp(material.thin_transmission_ior.x, 0.0f, 1.0f) * (1.0f - fresnel);
		if (!(transmitted_energy > 0.0f) || !isfinite(transmitted_energy)) return hit;
		throughput *= transmitted_energy;
		const float remaining_distance = ray.max_distance - hit.distance;
		if (!(remaining_distance > 0.002f) || !all(isfinite(throughput))) return { raytracing::intersection_type::none, 0u, 0u, float2(0.0f), 0.0f };
		const float3 offset_normal = dot(refracted, geometric_normal) >= 0.0f ? geometric_normal : -geometric_normal;
		ray = { ray.origin + ray.direction * hit.distance + offset_normal * 0.002f, refracted, 0.001f, remaining_distance - 0.001f };
	}
	// Bounded transport never turns a stack of thin panes into an unbounded
	// traversal. The third pane is conservatively treated as opaque.
	return hybrid_intersect_split(ray, scene, opaque_scene, alpha_scene, mask, ray_spread, alpha_ray_class, parameters, materials, geometry_records, material_textures, material_sampler, diagnostic, alpha_intersection_table, fast_intersector);
}

// Direct-light connections must remain straight-line proposals. A thin pane
// therefore does not bend the target segment; it attenuates the selected
// visibility connection by its Fresnel transmission and reconnects along the
// original segment. This is intentionally separate from the refracted
// secondary transport helper above. It is called only after a ReSTIR source
// has been selected, never while evaluating candidate weights.
static float3 thin_visibility_transmittance(
		thread raytracing::ray &ray,
		raytracing::instance_acceleration_structure scene,
		raytracing::instance_acceleration_structure opaque_scene,
		raytracing::instance_acceleration_structure alpha_scene,
		uint mask,
		float ray_spread,
		constant Parameters &parameters,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		device MaterialDiagnosticAtomic &diagnostic,
		raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &fast_intersector) {
	float3 transmittance = float3(1.0f);
	for (uint pane = 0u; pane < 2u; pane++) {
		const HybridIntersection hit = hybrid_intersect_split(ray, scene, opaque_scene, alpha_scene, mask, ray_spread, ALPHA_RAY_CLASS_VISIBILITY, parameters, materials, geometry_records, material_textures, material_sampler, diagnostic, alpha_intersection_table, fast_intersector);
		if (hit.type != raytracing::intersection_type::triangle) return transmittance;
		const MaterialRecord material = materials[hit.instance_id];
		if ((material.flags & 8u) == 0u) return float3(0.0f);
		const float3 geometric_normal = intersection_normal(geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, -ray.direction);
		float3 unused_refracted;
		float fresnel = 1.0f;
		if (!thin_dielectric_refraction(ray.direction, geometric_normal, clamp(material.thin_transmission_ior.y, 1.0f, 4.0f), unused_refracted, fresnel)) return float3(0.0f);
		const float pane_transmittance = clamp(material.thin_transmission_ior.x, 0.0f, 1.0f) * (1.0f - fresnel);
		if (!(pane_transmittance > 0.0f) || !isfinite(pane_transmittance)) return float3(0.0f);
		transmittance *= pane_transmittance;
		const float remaining_distance = ray.max_distance - hit.distance;
		if (!(remaining_distance > 0.002f) || !all(isfinite(transmittance))) return float3(0.0f);
		// Keep the selected point/light endpoint fixed: refraction was evaluated
		// for Fresnel/TIR correctness, while this straight connection is the
		// bounded reconnection estimator for a thin surface.
		ray = { ray.origin + ray.direction * hit.distance + ray.direction * 0.002f, ray.direction, 0.001f, remaining_distance - 0.001f };
	}
	// A third pane is deliberately opaque: selected visibility must stay bounded.
	return float3(0.0f);
}

static uint hash_u32(uint value) {
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu; return value ^ (value >> 16);
}

// The stage probe deliberately records a bounded fixed-point luminance sum.
// It is only an A/B lineage signal: it cannot clamp or feed back into rendered
// radiance, and a 32-bit total remains safe at the validation resolutions.
static void stage_probe_add(constant Parameters &parameters, device atomic_uint &sum, float3 value) {
	if ((parameters.reconstruction_flags & 4u) == 0u || !all(isfinite(value))) return;
	const float luminance = max(dot(max(value, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
	atomic_fetch_add_explicit(&sum, uint(min(luminance, 32.0f) * 64.0f + 0.5f), memory_order_relaxed);
}

static void stage_probe_count(constant Parameters &parameters, device atomic_uint &sum, uint value) {
	if ((parameters.reconstruction_flags & 4u) == 0u) return;
	atomic_fetch_add_explicit(&sum, value, memory_order_relaxed);
}

static float random_float(thread uint &state) {
    state = hash_u32(state + 0x9e3779b9u);
    return float(state & 0x00ffffffu) / float(0x01000000u);
}

static float radical_inverse_vdc(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f;
}

// The uploaded STBN source is a small toroidal tile. Repeating it directly
// across a full render target makes its rank pattern visible in one-spp light
// estimates. Rotate coordinates and time per source tile, while keeping the
// underlying ranked volume and each pixel's temporal permutation unchanged.
static uint stbn_tile_scramble(uint2 pixel, uint dimension, constant Parameters &parameters) {
	const uint tile_x = pixel.x / parameters.sampling_tile_width;
	const uint tile_y = pixel.y / parameters.sampling_tile_height;
	return hash_u32(tile_x * 0x9e3779b9u ^ tile_y * 0x85ebca6bu ^ (dimension + 1u) * 0xc2b2ae35u);
}

// Owen-scrambled, progressive Hammersley dimensions. Consecutive frames visit
// disjoint points from the same bounded sequence, while the per-pixel rotation
// stays fixed. This gives temporal reconstruction new, low-discrepancy samples
// without the large screen-wide lighting shifts produced by a frame-random hash.
static float hammersley_dimension(uint frame_index, uint sample, uint sample_count, uint pixel_seed, uint dimension, constant Parameters &parameters, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (parameters.sampling_sequence_mode != 0u && parameters.sampling_tile_width != 0u && parameters.sampling_tile_height != 0u && parameters.sampling_tile_depth != 0u && parameters.sampling_tile_channels != 0u) {
		// STBN ranks are already an XY/Z-distributed scalar sequence. Keep that
		// scalar linear through the estimator instead of hashing it into white-noise
		// rotations. Independently generated channels plus dimension-specific XY/Z
		// transforms prevent paired estimators from sharing one scalar stream.
		const uint channel = dimension % parameters.sampling_tile_channels;
		const uint tile_scramble = stbn_tile_scramble(stbn_pixel, dimension, parameters);
		const uint2 tile_pixel = uint2((stbn_pixel.x + dimension * 7u + tile_scramble) % parameters.sampling_tile_width, (stbn_pixel.y + dimension * 11u + (tile_scramble >> 8u)) % parameters.sampling_tile_height);
		const uint slice = (frame_index + dimension * 13u + (tile_scramble >> 16u)) % parameters.sampling_tile_depth;
		const uint layer = channel * parameters.sampling_tile_depth + slice;
		const float stbn_scalar = (float(stbn_scalar_volume.read(tile_pixel, layer).x) + 0.5f) / 1024.0f;
		return sample_count <= 1u ? stbn_scalar : fract((float(sample) + 0.5f) / float(sample_count) + stbn_scalar);
	}
	const uint sequence_length = 256u;
	const uint sequence_index = (frame_index * max(sample_count, 1u) + sample) & (sequence_length - 1u);
	const uint scramble = hash_u32(pixel_seed ^ (0x9e3779b9u * (dimension + 1u)));
	const float rotation = float(scramble & 0x00ffffffu) / float(0x01000000u);
	if (dimension == 0u) {
		return fract((float(sequence_index) + 0.5f) / float(sequence_length) + rotation);
	}
	return fract(radical_inverse_vdc(sequence_index + dimension * 0x9e3779b9u) + rotation);
}

// Shader-local mapping of the engine's sampling ABI. Values are deliberately
// sparse so new work can reserve dimensions without perturbing existing replay.
enum FluxSampleDimension : uint {
	FLUX_SAMPLE_DIMENSION_DIRECT_CANDIDATE = 0u,
	FLUX_SAMPLE_DIMENSION_DIRECT_EMISSIVE_SELECT = 1u,
	FLUX_SAMPLE_DIMENSION_DIRECT_BARYCENTRIC_U = 2u,
	FLUX_SAMPLE_DIMENSION_DIRECT_BARYCENTRIC_V = 3u,
	FLUX_SAMPLE_DIMENSION_DIRECT_RESERVOIR = 4u,
	FLUX_SAMPLE_DIMENSION_DIRECT_REUSE = 5u,
	FLUX_SAMPLE_DIMENSION_REFLECTION_U = 6u,
	FLUX_SAMPLE_DIMENSION_REFLECTION_V = 7u,
	FLUX_SAMPLE_DIMENSION_PRIMARY_ENVIRONMENT = 8u,
	FLUX_SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT = 11u,
	FLUX_SAMPLE_DIMENSION_REFLECTION_ENVIRONMENT = 14u,
	FLUX_SAMPLE_DIMENSION_SOLAR_U = 17u,
	FLUX_SAMPLE_DIMENSION_PUNCTUAL_AREA_U = 20u,
	FLUX_SAMPLE_DIMENSION_PRIMARY_GI_U = 22u,
	FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_MIRROR = 24u,
	FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_SOURCE = 25u,
	FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_BARYCENTRIC = 26u,
};

static float3 sample_cone(float3 axis, float angular_radius, thread uint &state) {
    if (angular_radius <= 0.00001f) return axis;
    float cos_max = cos(angular_radius);
    float cos_theta = mix(cos_max, 1.0f, random_float(state));
    float sin_theta = sqrt(max(0.0f, 1.0f - cos_theta * cos_theta));
    float phi = random_float(state) * 6.28318530718f;
    float3 helper = abs(axis.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(helper, axis));
    float3 bitangent = cross(axis, tangent);
    return normalize(axis * cos_theta + tangent * (cos(phi) * sin_theta) + bitangent * (sin(phi) * sin_theta));
}

// A short-range world-space visibility estimate for low-frequency primary
// diffuse transport. The cosine-weighted hemisphere matches the diffuse
// transport measure. Full response through the near 75% and a smooth fade over
// the final 25% confine the term to the authored world-space horizon. This is
// contact visibility, not another GI bounce and not a replacement for exact
// light/environment visibility.
static float sample_diffuse_contact_visibility(
		float3 world_position,
		float3 world_normal,
		uint pixel_seed,
		uint sample_count,
		float maximum_distance,
		float strength,
		constant Parameters &parameters,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler,
		device MaterialDiagnosticAtomic &material_diagnostic,
		raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector,
		raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (sample_count == 0u || maximum_distance <= 0.0001f || strength <= 0.0001f) return 1.0f;
	float3 helper = abs(world_normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
	float3 tangent = normalize(cross(helper, world_normal));
	float3 bitangent = cross(world_normal, tangent);
	float occlusion = 0.0f;
	sample_count = clamp(sample_count, 2u, 4u);
	const uint azimuth_scramble = hash_u32(pixel_seed ^ 0xa511e9b3u);
	const float azimuth_rotation = float(azimuth_scramble & 0x00ffffffu) / float(0x01000000u);
	for (uint sample = 0u; sample < sample_count; sample++) {
		// Unlike the progressive transport sequence, this small fixed set must span
		// the whole cosine hemisphere every frame. Stratifying radius explicitly
		// prevents the 2--4 rays from clustering away from contact geometry. A stable
		// per-pixel azimuth rotation decorrelates neighbors without temporal shimmer.
		const float u = (float(sample) + 0.5f) / float(sample_count);
		const float v = fract(radical_inverse_vdc(sample) + azimuth_rotation);
		const float radius = sqrt(u);
		const float phi = v * 6.28318530718f;
		const float z = sqrt(max(0.0f, 1.0f - radius * radius));
		const float3 direction = normalize(tangent * (cos(phi) * radius) + bitangent * (sin(phi) * radius) + world_normal * z);
		raytracing::ray contact_ray = { world_position + world_normal * 0.003f, direction, 0.001f, maximum_distance };
		auto contact_hit = hybrid_intersect_split(contact_ray, scene, opaque_scene, alpha_scene, 0xff, 0.02f, ALPHA_RAY_CLASS_INDIRECT, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
		if (contact_hit.type == raytracing::intersection_type::triangle) {
			// Preserve full contact response through the near 75% of the authored
			// horizon, then fade smoothly to zero. This is a bounded world-space
			// distance kernel, not a screen-space blur or an unbounded darkening term.
			occlusion += 1.0f - smoothstep(maximum_distance * 0.75f, maximum_distance, contact_hit.distance);
		}
	}
	return clamp(1.0f - strength * occlusion / float(sample_count), 0.0f, 1.0f);
}

static float3 sample_ggx_reflection(float3 incident, float3 normal, float roughness, uint frame_index, uint pixel_seed, constant Parameters &parameters, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	const float alpha = max(roughness * roughness, 0.001f);
	const float u1 = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, 6u, parameters, stbn_scalar_volume, stbn_pixel);
	const float u2 = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, 7u, parameters, stbn_scalar_volume, stbn_pixel);
	const float3 helper = abs(normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
	const float3 tangent = normalize(cross(helper, normal));
	const float3 bitangent = cross(normal, tangent);
	const float3 view = normalize(-incident);
	const float3 local_view = float3(dot(view, tangent), dot(view, bitangent), max(dot(view, normal), 0.0001f));
	const float3 stretched_view = normalize(float3(alpha * local_view.x, alpha * local_view.y, local_view.z));
	const float lensq = dot(stretched_view.xy, stretched_view.xy);
	const float3 t1 = lensq > 0.0f ? float3(-stretched_view.y, stretched_view.x, 0.0f) / sqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
	const float3 t2 = cross(stretched_view, t1);
	const float radius = sqrt(u1);
	const float phi = 6.28318530718f * u2;
	const float disk_x = radius * cos(phi);
	const float raw_disk_y = radius * sin(phi);
	const float blend = 0.5f * (1.0f + stretched_view.z);
	const float disk_y = mix(sqrt(max(0.0f, 1.0f - disk_x * disk_x)), raw_disk_y, blend);
	const float3 visible_normal = disk_x * t1 + disk_y * t2 + sqrt(max(0.0f, 1.0f - disk_x * disk_x - disk_y * disk_y)) * stretched_view;
	const float3 local_half = normalize(float3(alpha * visible_normal.x, alpha * visible_normal.y, max(visible_normal.z, 0.0f)));
	const float3 half_vector = normalize(tangent * local_half.x + bitangent * local_half.y + normal * local_half.z);
	float3 reflected = normalize(reflect(incident, half_vector));
	return dot(reflected, normal) > 0.0f ? reflected : normalize(reflect(incident, normal));
}

static float smith_ggx_lambda(float normal_dot_direction, float alpha_squared) {
	float cos_squared = max(normal_dot_direction * normal_dot_direction, 0.000001f);
	float tan_squared = max((1.0f - cos_squared) / cos_squared, 0.0f);
	return 0.5f * (-1.0f + sqrt(1.0f + alpha_squared * tan_squared));
}

// The reflection direction is sampled from the GGX half-vector distribution.
// Convert that sample to a radiance estimator here; multiplying by Fresnel a
// second time after this function would incorrectly brighten or darken glossy
// transport, especially for the low-roughness validation sphere.
static float3 ggx_reflection_throughput(float3 incident, float3 normal, float3 reflected, float roughness, float3 f0) {
	float3 view = -incident;
	float normal_dot_view = max(dot(normal, view), 0.0001f);
	float normal_dot_light = max(dot(normal, reflected), 0.0001f);
	float3 half_vector = normalize(view + reflected);
	float view_dot_half = max(dot(view, half_vector), 0.0001f);
	float alpha = max(roughness * roughness, 0.001f);
	float alpha_squared = alpha * alpha;
	const float lambda_view = smith_ggx_lambda(normal_dot_view, alpha_squared);
	const float lambda_light = smith_ggx_lambda(normal_dot_light, alpha_squared);
	float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - view_dot_half, 5.0f);
	// Visible-normal GGX sampling cancels D and the grazing Jacobian. The
	// remaining Smith ratio is energy-derived and bounded by one, avoiding the
	// unbounded fireflies of ordinary NDF sampling without clipping radiance.
	return fresnel * (1.0f + lambda_view) / max(1.0f + lambda_view + lambda_light, 0.0001f);
}

// Cook-Torrance GGX response including the incident cosine. Inputs use the
// renderer convention where view_direction points from the camera to surface.
static float3 ggx_direct_response(float3 view_direction, float3 normal, float3 light_direction, float roughness, float3 f0) {
	const float3 view = normalize(-view_direction);
	const float normal_dot_view = max(dot(normal, view), 0.0f);
	const float normal_dot_light = max(dot(normal, light_direction), 0.0f);
	if (normal_dot_view <= 0.0f || normal_dot_light <= 0.0f) return 0.0f;
	const float3 half_vector = normalize(view + light_direction);
	const float normal_dot_half = max(dot(normal, half_vector), 0.0f);
	const float view_dot_half = max(dot(view, half_vector), 0.0f);
	const float alpha = max(roughness * roughness, 0.001f);
	const float alpha_squared = alpha * alpha;
	const float denominator = normal_dot_half * normal_dot_half * (alpha_squared - 1.0f) + 1.0f;
	const float distribution = alpha_squared / max(M_PI_F * denominator * denominator, 0.000001f);
	const float geometry = 1.0f / (1.0f + smith_ggx_lambda(normal_dot_view, alpha_squared) + smith_ggx_lambda(normal_dot_light, alpha_squared));
	const float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - view_dot_half, 5.0f);
	const float3 response = fresnel * distribution * geometry * normal_dot_light / max(4.0f * normal_dot_view * normal_dot_light, 0.000001f);
	return all(isfinite(response)) ? response : float3(0.0f);
}

static float power_heuristic(float pdf_a, float pdf_b) {
	if (!(pdf_a > 0.0f) || !isfinite(pdf_a)) return 0.0f;
	if (!(pdf_b > 0.0f) || !isfinite(pdf_b)) return 1.0f;
	const float a_squared = pdf_a * pdf_a;
	const float b_squared = pdf_b * pdf_b;
	return a_squared / max(a_squared + b_squared, 0.000000000001f);
}

// PDF of the visible-normal GGX reflection sampler above, expressed in solid
// angle at the shaded surface. Keeping this in the same parameterization as the
// area-light PDF is what makes the direct-light MIS weights finite at grazing
// angles without clipping the evaluated radiance.
static float ggx_vndf_reflection_pdf(float3 view_direction, float3 normal, float3 light_direction, float roughness) {
	const float3 view = normalize(-view_direction);
	const float normal_dot_view = max(dot(normal, view), 0.0f);
	const float normal_dot_light = max(dot(normal, light_direction), 0.0f);
	if (normal_dot_view <= 0.0f || normal_dot_light <= 0.0f) return 0.0f;
	const float3 half_vector = normalize(view + light_direction);
	const float normal_dot_half = max(dot(normal, half_vector), 0.0f);
	const float view_dot_half = max(dot(view, half_vector), 0.0f);
	if (normal_dot_half <= 0.0f || view_dot_half <= 0.0f) return 0.0f;
	const float alpha = max(roughness * roughness, 0.001f);
	const float alpha_squared = alpha * alpha;
	const float denominator = normal_dot_half * normal_dot_half * (alpha_squared - 1.0f) + 1.0f;
	const float distribution = alpha_squared / max(M_PI_F * denominator * denominator, 0.000001f);
	const float smith_view = 1.0f / (1.0f + smith_ggx_lambda(normal_dot_view, alpha_squared));
	const float pdf = distribution * smith_view / max(4.0f * normal_dot_view, 0.000001f);
	return isfinite(pdf) && pdf > 0.0f ? pdf : 0.0f;
}

static float emissive_solid_angle_pdf_for_hit(
		uint instance_id,
		uint primitive_id,
		float distance_squared,
		float3 light_direction,
		constant GeometryRecord *geometries,
		constant EmissiveTriangleRecord *triangles,
		uint triangle_count) {
	// The compact table contains selectable emissive triangles only. Its order is
	// intentionally independent of scene geometry order, so a BSDF hit must
	// locate the exact current instance/primitive record rather than reconstruct
	// an all-geometry prefix slot.
	uint triangle_index = 0xffffffffu;
	for (uint index = 0u; index < triangle_count; index++) {
		const EmissiveTriangleRecord candidate = triangles[index];
		if (candidate.instance_id == instance_id && candidate.primitive_id == primitive_id && candidate.selection_pdf > 0.0f && candidate.area > 0.0f) {
			triangle_index = index;
			break;
		}
	}
	if (triangle_index == 0xffffffffu) return 0.0f;
	const EmissiveTriangleRecord triangle = triangles[triangle_index];
	if (triangle.instance_id != instance_id || triangle.primitive_id != primitive_id || !(triangle.selection_pdf > 0.0f) || !(triangle.area > 0.0f)) return 0.0f;
	constant GeometryRecord &geometry = geometries[triangle.instance_id];
	const float3 p0 = world_vertex_position(geometry, triangle_vertex_index(geometry, triangle.primitive_id, 0u));
	const float3 p1 = world_vertex_position(geometry, triangle_vertex_index(geometry, triangle.primitive_id, 1u));
	const float3 p2 = world_vertex_position(geometry, triangle_vertex_index(geometry, triangle.primitive_id, 2u));
	const float3 emitter_normal = -normalize(cross(p1 - p0, p2 - p0));
	const float light_cosine = max(dot(emitter_normal, -light_direction), 0.0f);
	if (!(light_cosine > 0.0f)) return 0.0f;
	const float pdf = (triangle.selection_pdf / triangle.area) * distance_squared / light_cosine;
	return isfinite(pdf) && pdf > 0.0f ? pdf : 0.0f;
}

static float3 sample_emissive_lighting(
		float3 world_position,
		float3 world_normal,
		float3 diffuse_albedo,
		float3 view_direction,
		float roughness,
		float3 f0,
		uint sample_count,
		constant MaterialRecord *materials,
		constant GeometryRecord *geometry_records,
		constant EmissiveTriangleRecord *emissive_triangles,
		uint emissive_triangle_count,
	uint frame_index,
	uint state,
	float transport_max_distance,
	constant Parameters &parameters,
#if HYBRID_BINDLESS_MATERIALS
	constant MaterialTextureTable &material_textures,
#else
	array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler,
	device MaterialDiagnosticAtomic &material_diagnostic,
	raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
	thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector,
		raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (emissive_triangle_count == 0u || (all(diffuse_albedo <= float3(0.0001f)) && all(f0 <= float3(0.0001f)))) return 0.0f;
	float3 result = 0.0f;
	sample_count = max(sample_count, 1u);
	const uint pixel_seed = state;
	for (uint sample = 0u; sample < sample_count; sample++) {
		const float select_u = hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 0u, parameters, stbn_scalar_volume, stbn_pixel);
		uint low = 0u;
		uint high = emissive_triangle_count - 1u;
		while (low < high) {
			const uint middle = low + (high - low) / 2u;
			if (select_u <= emissive_triangles[middle].cdf) high = middle;
			else low = middle + 1u;
		}
		const EmissiveTriangleRecord emitter = emissive_triangles[low];
		if (!(emitter.selection_pdf > 0.0f) || !(emitter.area > 0.0f) || !isfinite(emitter.selection_pdf)) continue;
		constant GeometryRecord &geometry = geometry_records[emitter.instance_id];
		const uint primitive_id = emitter.primitive_id;
		float3 p0 = world_vertex_position(geometry, triangle_vertex_index(geometry, primitive_id, 0u));
		float3 p1 = world_vertex_position(geometry, triangle_vertex_index(geometry, primitive_id, 1u));
		float3 p2 = world_vertex_position(geometry, triangle_vertex_index(geometry, primitive_id, 2u));
		float3 area_vector = cross(p1 - p0, p2 - p0);
		float triangle_area = 0.5f * length(area_vector);
		if (triangle_area <= 0.000001f) continue;
		float sqrt_x = sqrt(hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 2u, parameters, stbn_scalar_volume, stbn_pixel));
		float bary_y = hammersley_dimension(frame_index, sample, sample_count, pixel_seed, 3u, parameters, stbn_scalar_volume, stbn_pixel);
		const float2 emitter_barycentric = float2(sqrt_x * (1.0f - bary_y), sqrt_x * bary_y);
		float3 light_position = p0 * (1.0f - emitter_barycentric.x - emitter_barycentric.y) + p1 * emitter_barycentric.x + p2 * emitter_barycentric.y;
		float3 to_light = light_position - world_position;
		float distance_squared = dot(to_light, to_light);
		float distance_to_light = sqrt(distance_squared);
		if (distance_to_light <= 0.02f) continue;
		if (transport_max_distance > 0.0f && distance_to_light > transport_max_distance) continue;
		float3 light_direction = to_light / distance_to_light;
		float surface_cosine = max(dot(world_normal, light_direction), 0.0f);
		// Godot's packed triangle winding is opposite the visible material normal
		// in this Metal geometry path. Recover the material-facing emitter normal,
		// then evaluate a one-sided cosine term. abs() here lit both sides of the
		// Cornell ceiling panel and produced the nonphysical wall/ceiling patches.
		float3 emitter_normal = -normalize(area_vector);
		float light_cosine = max(dot(emitter_normal, -light_direction), 0.0f);
		if (surface_cosine <= 0.0f || light_cosine <= 0.0f) continue;
		const MaterialRecord emitter_material = materials[emitter.instance_id];
		const MaterialSample emitter_sample = sample_material(emitter_material, geometry_records, emitter.instance_id, primitive_id, emitter_barycentric, distance_to_light, 0.001f, material_textures, material_sampler, parameters.material_texture_capacity);
		if ((emitter_material.flags & 1u) != 0u && sample_material_alpha(emitter_material, geometry_records, emitter.instance_id, primitive_id, emitter_barycentric, distance_to_light, 0.001f, material_textures, material_sampler, parameters.material_texture_capacity) < emitter_material.material_factors.z) continue;
		raytracing::ray visibility_ray = { world_position + world_normal * 0.003f, light_direction, 0.001f, distance_to_light - 0.01f };
		const float3 visibility_transmittance = thin_visibility_transmittance(visibility_ray, scene, opaque_scene, alpha_scene, 0xff, 0.001f, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
		if (all(visibility_transmittance <= float3(0.0f))) continue;
		// selection_pdf is probability mass for this exact triangle. Uniform
		// barycentrics have density 1 / area, so this is the exact inverse area
		// proposal that converts to the same solid-angle PDF used by MIS hits.
		float inverse_pdf = triangle_area / emitter.selection_pdf;
		float3 emitted_radiance = emitter_sample.emission;
		const float3 bsdf_response = diffuse_albedo * (1.0f / M_PI_F) * surface_cosine + ggx_direct_response(view_direction, world_normal, light_direction, roughness, f0);
		result += visibility_transmittance * bsdf_response * emitted_radiance * light_cosine * inverse_pdf / max(distance_squared, 0.0001f);
	}
	const float3 estimate = result / float(sample_count);
	return all(isfinite(estimate)) ? estimate : float3(0.0f);
}

// A deliberately narrow singular technique: sample a finite emissive triangle,
// reflect its sampled point and normal through one selected planar delta mirror,
// then connect the receiver to that virtual source. Ordinary NEE has zero
// density for this constrained hidden delta path, therefore its MIS weight is
// exactly one. Both real segments are traced below; nothing is cached.
static float3 sample_bidirectional_planar_caustic(
		float3 receiver_position, float3 receiver_normal, float3 receiver_diffuse,
		uint receiver_visibility_mask, uint frame_index, uint state,
		constant MaterialRecord *materials, constant GeometryRecord *geometry_records,
		constant EmissiveTriangleRecord *emissive_triangles, uint emissive_triangle_count,
		constant BidirectionalCausticMirrorTriangleRecord *mirrors, uint mirror_count,
		float transport_max_distance, constant Parameters &parameters,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic,
		raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
		thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector,
		raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if ((parameters.bidirectional_caustic_flags & 3u) != 3u || mirror_count == 0u || emissive_triangle_count == 0u || all(receiver_diffuse <= float3(0.0001f)) || receiver_visibility_mask == 0u) return 0.0f;
	atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_candidates, 1u, memory_order_relaxed);
	const float mirror_u = hammersley_dimension(frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_MIRROR, parameters, stbn_scalar_volume, stbn_pixel);
	const uint mirror_index = min(uint(mirror_u * float(mirror_count)), mirror_count - 1u);
	const BidirectionalCausticMirrorTriangleRecord mirror = mirrors[mirror_index];
	if (mirror.instance_id >= parameters.geometry_record_count || mirror.visibility_mask == 0u || (mirror.visibility_mask & receiver_visibility_mask) == 0u) {
		atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed);
		return 0.0f;
	}
	const float source_u = hammersley_dimension(frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_SOURCE, parameters, stbn_scalar_volume, stbn_pixel);
	uint low = 0u;
	uint high = emissive_triangle_count - 1u;
	while (low < high) { const uint middle = low + (high - low) / 2u; if (source_u <= emissive_triangles[middle].cdf) high = middle; else low = middle + 1u; }
	const EmissiveTriangleRecord source = emissive_triangles[low];
	if (source.instance_id >= parameters.geometry_record_count || !(source.selection_pdf > 0.0f) || !(source.area > 0.0f) || !isfinite(source.selection_pdf) || !isfinite(source.area) || (materials[source.instance_id].visibility_mask & mirror.visibility_mask & receiver_visibility_mask) == 0u) {
		atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_nonfinite_or_pdf_failures, 1u, memory_order_relaxed);
		return 0.0f;
	}
	constant GeometryRecord &mirror_geometry = geometry_records[mirror.instance_id];
	const uint m0i = triangle_vertex_index(mirror_geometry, mirror.primitive_id, 0u);
	const uint m1i = triangle_vertex_index(mirror_geometry, mirror.primitive_id, 1u);
	const uint m2i = triangle_vertex_index(mirror_geometry, mirror.primitive_id, 2u);
	const float3 m0 = world_vertex_position(mirror_geometry, m0i);
	const float3 m1 = world_vertex_position(mirror_geometry, m1i);
	const float3 m2 = world_vertex_position(mirror_geometry, m2i);
	const float3 mirror_normal = -normalize(cross(m1 - m0, m2 - m0));
	if (!valid_direction(mirror_normal)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_nonfinite_or_pdf_failures, 1u, memory_order_relaxed); return 0.0f; }
	constant GeometryRecord &source_geometry = geometry_records[source.instance_id];
	const float3 s0 = world_vertex_position(source_geometry, triangle_vertex_index(source_geometry, source.primitive_id, 0u));
	const float3 s1 = world_vertex_position(source_geometry, triangle_vertex_index(source_geometry, source.primitive_id, 1u));
	const float3 s2 = world_vertex_position(source_geometry, triangle_vertex_index(source_geometry, source.primitive_id, 2u));
	const float sqrt_u = sqrt(hammersley_dimension(frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_BARYCENTRIC, parameters, stbn_scalar_volume, stbn_pixel));
	const float bary_u = hammersley_dimension(frame_index, 1u, 2u, state, FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_BARYCENTRIC + 1u, parameters, stbn_scalar_volume, stbn_pixel);
	const float2 source_barycentric = float2(sqrt_u * (1.0f - bary_u), sqrt_u * bary_u);
	const float3 source_position = s0 * (1.0f - source_barycentric.x - source_barycentric.y) + s1 * source_barycentric.x + s2 * source_barycentric.y;
	const float3 source_normal = -normalize(cross(s1 - s0, s2 - s0));
	const float3 virtual_source = source_position - mirror_normal * (2.0f * dot(source_position - m0, mirror_normal));
	const float3 to_virtual = virtual_source - receiver_position;
	const float distance_squared = dot(to_virtual, to_virtual);
	const float denominator = dot(to_virtual, mirror_normal);
	if (!(distance_squared > 0.000001f) || !isfinite(distance_squared) || abs(denominator) <= 0.000001f || !isfinite(denominator)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_nonfinite_or_pdf_failures, 1u, memory_order_relaxed); return 0.0f; }
	const float mirror_t = dot(m0 - receiver_position, mirror_normal) / denominator;
	if (!(mirror_t > 0.0001f && mirror_t < 0.9999f) || !isfinite(mirror_t)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed); return 0.0f; }
	const float3 mirror_position = receiver_position + to_virtual * mirror_t;
	const float3 e0 = m1 - m0;
	const float3 e1 = m2 - m0;
	const float3 e2 = mirror_position - m0;
	const float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1), d20 = dot(e2, e0), d21 = dot(e2, e1);
	const float bary_denom = d00 * d11 - d01 * d01;
	if (abs(bary_denom) <= 0.0000001f || !isfinite(bary_denom)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_nonfinite_or_pdf_failures, 1u, memory_order_relaxed); return 0.0f; }
	const float bary_y = (d11 * d20 - d01 * d21) / bary_denom;
	const float bary_z = (d00 * d21 - d01 * d20) / bary_denom;
	const float bary_x = 1.0f - bary_y - bary_z;
	if (!(bary_x > 0.0001f && bary_y > 0.0001f && bary_z > 0.0001f) || !isfinite(bary_x) || !isfinite(bary_y) || !isfinite(bary_z)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed); return 0.0f; }
	const float3 receiver_direction = normalize(to_virtual);
	const float receiver_cosine = dot(receiver_normal, receiver_direction);
	const float3 virtual_source_normal = source_normal - mirror_normal * (2.0f * dot(source_normal, mirror_normal));
	const float source_cosine = dot(normalize(virtual_source_normal), -receiver_direction);
	const float receiver_to_mirror_distance = length(mirror_position - receiver_position);
	const float mirror_to_source_distance = length(source_position - mirror_position);
	if (!(receiver_cosine > 0.00001f && source_cosine > 0.00001f && receiver_to_mirror_distance > 0.01f && mirror_to_source_distance > 0.01f) || (transport_max_distance > 0.0f && (receiver_to_mirror_distance + mirror_to_source_distance) > transport_max_distance)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed); return 0.0f; }
	// Fresh alpha-aware visibility. The segment end offsets prevent the selected
	// mirror/source from self-occluding, while every other current scene primitive
	// remains a blocker.
	raytracing::ray first_visibility = { receiver_position + receiver_normal * (dot(receiver_direction, receiver_normal) >= 0.0f ? 0.003f : -0.003f), receiver_direction, 0.001f, receiver_to_mirror_distance - 0.006f };
	atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_visibility_rays, 1u, memory_order_relaxed);
	if (hybrid_intersect_split(first_visibility, scene, opaque_scene, alpha_scene, mirror.visibility_mask & receiver_visibility_mask & 0xffu, 0.001f, ALPHA_RAY_CLASS_VISIBILITY, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector).type == raytracing::intersection_type::triangle) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed); return 0.0f; }
	const float3 mirror_to_source_direction = normalize(source_position - mirror_position);
	raytracing::ray second_visibility = { mirror_position + mirror_normal * (dot(mirror_to_source_direction, mirror_normal) >= 0.0f ? 0.003f : -0.003f), mirror_to_source_direction, 0.001f, mirror_to_source_distance - 0.006f };
	atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_visibility_rays, 1u, memory_order_relaxed);
	if (hybrid_intersect_split(second_visibility, scene, opaque_scene, alpha_scene, mirror.visibility_mask & materials[source.instance_id].visibility_mask & 0xffu, 0.001f, ALPHA_RAY_CLASS_VISIBILITY, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector).type == raytracing::intersection_type::triangle) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed); return 0.0f; }
	const MaterialRecord source_material = materials[source.instance_id];
	const MaterialRecord mirror_material = materials[mirror.instance_id];
	const MaterialSample source_sample = sample_material(source_material, geometry_records, source.instance_id, source.primitive_id, source_barycentric, mirror_to_source_distance, 0.001f, material_textures, material_sampler, parameters.material_texture_capacity);
	const MaterialSample mirror_sample = sample_material(mirror_material, geometry_records, mirror.instance_id, mirror.primitive_id, float2(bary_y, bary_z), receiver_to_mirror_distance, 0.001f, material_textures, material_sampler, parameters.material_texture_capacity);
	if (mirror_material.generation_low != mirror.material_generation_low || mirror_material.generation_high != mirror.material_generation_high || mirror_sample.metallic < 0.999f || mirror_sample.roughness > parameters.bidirectional_caustic_delta_roughness_threshold || (mirror_material.flags & 1u) != 0u || !all(isfinite(source_sample.emission)) || !all(isfinite(mirror_sample.albedo))) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_rejections, 1u, memory_order_relaxed); return 0.0f; }
	const float source_area_pdf = source.selection_pdf / source.area;
	const float mirror_selection_mass = 1.0f / float(mirror_count);
	const float proposal_pdf = source_area_pdf * mirror_selection_mass;
	if (!(proposal_pdf > 0.0f) || !isfinite(proposal_pdf)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_nonfinite_or_pdf_failures, 1u, memory_order_relaxed); return 0.0f; }
	const float3 delta_fresnel = clamp(mirror_sample.albedo, 0.0f, 1.0f);
	const float3 contribution = receiver_diffuse * (1.0f / M_PI_F) * source_sample.emission * delta_fresnel * receiver_cosine * source_cosine / (distance_squared * proposal_pdf);
	if (!all(isfinite(contribution)) || any(contribution < 0.0f)) { atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_nonfinite_or_pdf_failures, 1u, memory_order_relaxed); return 0.0f; }
	atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_valid, 1u, memory_order_relaxed);
	if (any(contribution > 0.0f)) atomic_fetch_add_explicit(&material_diagnostic.bidirectional_caustic_contributed, 1u, memory_order_relaxed);
	return contribution;
}

// Godot's authored inverse-distance/range contract for a bounded omni source.
static float omni_attenuation(float distance, float range, float decay) {
	if (range <= 0.0001f || distance >= range) return 0.0f;
	float normalized_distance = distance / range;
	normalized_distance *= normalized_distance;
	normalized_distance *= normalized_distance;
	float range_fade = max(1.0f - normalized_distance, 0.0f);
	range_fade *= range_fade;
	return range_fade * pow(max(distance, 0.0001f), -decay);
}

static float3 sample_punctual_lighting(
		float3 world_position,
		float3 world_normal,
		float3 diffuse_albedo,
		float3 view_direction,
		float roughness,
		float3 f0,
		uint receiver_visibility_mask,
		uint sign_filter, // 0 = all, 1 = positive only, 2 = negative only.
		bool indirect_transport,
		bool trace_visibility,
	constant PunctualLightRecord *punctual_lights,
	uint punctual_light_count,
	uint frame_index,
	thread uint &state,
	float transport_max_distance,
	constant Parameters &parameters,
	constant MaterialRecord *materials,
	constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
	constant MaterialTextureTable &material_textures,
#else
	array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler,
	device MaterialDiagnosticAtomic &material_diagnostic,
	raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table,
	thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector,
		raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (punctual_light_count == 0u || (all(diffuse_albedo <= float3(0.0001f)) && all(f0 <= float3(0.0001f)))) return 0.0f;
	float3 result = 0.0f;
	for (uint light_index = 0u; light_index < punctual_light_count; light_index++) {
		constant PunctualLightRecord &light = punctual_lights[light_index];
		const bool negative = (light.flags & 2u) != 0u;
		if ((sign_filter == 1u && negative) || (sign_filter == 2u && !negative)) continue;
		if (light.type > 3u || (light.cull_mask & receiver_visibility_mask) == 0u) continue;
		if (!indirect_transport) atomic_fetch_add_explicit(&material_diagnostic.primary_analytic_selected, 1u, memory_order_relaxed);
		uint visibility_mask = light.shadow_caster_mask & 0xffu;
		float3 light_direction = float3(0.0f);
		float distance_to_light = 0.0f;
		float attenuation = 1.0f;
		if (light.type == 3u) {
			uint directional_state = hash_u32(state ^ frame_index ^ light.source_identity);
			light_direction = sample_cone(normalize(light.direction_spot_outer.xyz), max(light.position_range.w, 0.0f), directional_state);
			distance_to_light = transport_max_distance > 0.0f ? transport_max_distance : 100000.0f;
		} else {
			float3 light_position = light.position_range.xyz;
			float area_weight = 1.0f;
			if (light.type == 2u) {
				// Uniformly sample the authored rectangular emitter. The record stores
				// world-space half axes, so the PDF is 1 / (4 |u x v|).
				const float u = hammersley_dimension(frame_index, light_index, max(punctual_light_count, 1u), state, 20u, parameters, stbn_scalar_volume, stbn_pixel) * 2.0f - 1.0f;
				const float v = hammersley_dimension(frame_index, light_index, max(punctual_light_count, 1u), state, 21u, parameters, stbn_scalar_volume, stbn_pixel) * 2.0f - 1.0f;
				const float3 area_u = light.area_u_spot_attenuation.xyz;
				const float3 area_v = light.area_v.xyz;
				const float area = 4.0f * length(cross(area_u, area_v));
				if (!(area > 0.000001f) || !isfinite(area)) continue;
				light_position += area_u * u + area_v * v;
				area_weight = area;
			}
			const float3 to_light = light_position - world_position;
			const float distance_squared = dot(to_light, to_light);
			distance_to_light = sqrt(distance_squared);
			if (distance_to_light <= 0.02f) continue;
			if (transport_max_distance > 0.0f && distance_to_light > transport_max_distance) continue;
			// Finite area sampling already converts area measure to solid angle with
			// its geometric distance-squared term below; applying Omni's inverse
			// distance decay as well would double-count falloff.
			attenuation = omni_attenuation(distance_to_light, light.position_range.w, light.type == 2u ? 0.0f : light.radiance_attenuation.w);
			if (attenuation <= 0.0f) continue;
			light_direction = to_light / distance_to_light;
			if (light.type == 1u) {
				const float outer = clamp(light.direction_spot_outer.w, -1.0f, 1.0f);
				const float spot_cosine = max(dot(normalize(light.direction_spot_outer.xyz), -light_direction), outer);
				const float spot_rim = max(0.0001f, (1.0f - spot_cosine) / max(1.0f - outer, 0.0001f));
				attenuation *= max(1.0f - pow(spot_rim, max(light.area_u_spot_attenuation.w, 0.0001f)), 0.0f);
			} else if (light.type == 2u) {
				const float3 area_normal = normalize(light.direction_spot_outer.xyz); // Authored local -Z.
				const float emitter_cosine = max(dot(area_normal, -light_direction), 0.0f);
				if (emitter_cosine <= 0.0f) continue;
				attenuation *= emitter_cosine * area_weight / max(distance_squared, 0.0001f);
			}
		}
		float surface_cosine = max(dot(world_normal, light_direction), 0.0f);
		if (surface_cosine <= 0.0f) continue;
		float3 visibility = float3(1.0f);
		if (trace_visibility && (light.flags & 1u) != 0u && visibility_mask != 0u && light.shadow_opacity > 0.0f) {
			raytracing::ray visibility_ray = { world_position + world_normal * 0.003f, light_direction, 0.001f, distance_to_light - 0.01f };
			if (!indirect_transport) atomic_fetch_add_explicit(&material_diagnostic.primary_analytic_visibility_tests, 1u, memory_order_relaxed);
			const float visibility_spread = light.type == 3u ? max(light.position_range.w, 0.00025f) : 0.001f;
			const float3 thin_visibility = thin_visibility_transmittance(visibility_ray, scene, opaque_scene, alpha_scene, visibility_mask, visibility_spread, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
			// Preserve Godot's authored shadow-opacity interpolation for opaque
			// blockers while allowing a fully opaque shadow to become a finite
			// Fresnel-transmitted shadow through supported thin panes.
			visibility = mix(float3(1.0f - clamp(light.shadow_opacity, 0.0f, 1.0f)), thin_visibility, clamp(light.shadow_opacity, 0.0f, 1.0f));
		}
		const float3 bsdf_response = diffuse_albedo * (1.0f / M_PI_F) * surface_cosine + ggx_direct_response(view_direction, world_normal, light_direction, roughness, f0) * light.specular_amount;
		const float indirect_scale = indirect_transport ? max(light.indirect_energy, 0.0f) : 1.0f;
		const float3 contribution = bsdf_response * light.radiance_attenuation.rgb * indirect_scale * attenuation * visibility;
		if (!indirect_transport && any(abs(contribution) > float3(0.000001f))) atomic_fetch_add_explicit(&material_diagnostic.primary_analytic_contributed, 1u, memory_order_relaxed);
		result += contribution;
	}
	return all(isfinite(result)) ? result : float3(0.0f);
}

static uint hash_u32(uint value);

struct DirectLightReservoir {
	uint source_type; // 0 = emissive, 1 = punctual.
	uint source_index; // Emissive primitive ID, or punctual light index.
	uint2 source_identity; // Exact stable instance identity for emissive.
	uint triangle_record;
	float2 barycentric;
	float proposal_pdf;
	float target;
	float weight_sum;
	uint candidate_count;
	uint age;
	bool valid;
};

// Cache ABI: signed world keys and the entire committed transport revision
// tuple live with every cell. Visibility is intentionally absent: a reused
// finite-light sample is always evaluated at the current receiver below.
struct RegirHeader {
	int4 center_and_cell_size; // xyz world key, w meters.
	int4 previous_center_and_flags; // xyz prior key, w enabled.
	uint4 dimensions_and_blocks; // xyz cells, w staging block count.
	ulong4 revisions;
};

struct RegirCell {
	int4 key_valid; // xyz exact world key, w validity.
	uint4 source;
	float4 sample;
	float4 metadata;
	ulong4 revisions;
};

struct RegirCandidate {
	uint cell_index;
	uint valid;
	uint2 padding;
	uint4 source;
	float4 sample;
	float4 metadata;
};

// ABI-v3 mirror of ReusablePathSampleGpuRecord. This intentionally carries
// neither a pixel nor cached visibility/radiance ownership. Reconnection
// always resolves the current triangle/material and current visibility.
struct ReusablePathSampleRecord {
	ulong secondary_geometry_instance_id;
	ulong secondary_material_id;
	uint secondary_surface_id;
	uint secondary_primitive_id;
	ulong source_primary_geometry_instance_id;
	ulong source_primary_material_id;
	uint source_primary_surface_id;
	uint source_primary_primitive_id;
	uint abi_version;
	uint flags;
	ulong secondary_geometry_revision;
	ulong secondary_material_revision;
	ulong secondary_residency_revision;
	ulong source_primary_geometry_revision;
	ulong source_primary_material_revision;
	ulong source_primary_residency_revision;
	ulong lighting_revision;
	ulong environment_revision;
	ulong source_primary_mask;
	ulong secondary_mask;
	ulong capture_frame;
	float4 secondary_world_position;
	float4 secondary_geometric_normal;
	float4 secondary_shading_normal;
	float4 source_primary_world_position;
	float4 source_primary_geometric_normal;
	float4 source_primary_shading_normal;
	float4 throughput;
	float4 incident_radiance;
	float4 outgoing_radiance;
	float source_primary_proposal_solid_angle_pdf;
	float target;
	float normalization;
	uint age;
	float secondary_barycentric_u;
	float secondary_barycentric_v;
	uint padding;
};

struct ReusablePathHeader {
	int4 center_and_cell_size;
	int4 previous_center_and_flags;
	uint4 dimensions_and_flags;
	// Deliberately excludes light/environment: they are evaluated freshly for
	// every selected proposal, while geometry and residency remain fatal.
	ulong2 geometry_and_residency_revisions;
	ulong2 padding0;
	ulong frame;
	ulong3 padding;
};

static uint reusable_path_cell_count(constant ReusablePathHeader &header) {
	return header.dimensions_and_flags.x * header.dimensions_and_flags.y * header.dimensions_and_flags.z;
}

static int3 reusable_path_key_for_index(uint index, constant ReusablePathHeader &header) {
	const uint dx = header.dimensions_and_flags.x;
	const uint dy = header.dimensions_and_flags.y;
	const int3 local = int3(int(index % dx), int((index / dx) % dy), int(index / (dx * dy)));
	return header.center_and_cell_size.xyz + local - int3(int(dx / 2u), int(dy / 2u), int(header.dimensions_and_flags.z / 2u));
}

static bool reusable_path_index_for_key(int3 key, constant ReusablePathHeader &header, thread uint &r_index) {
	const int3 local = key - header.center_and_cell_size.xyz + int3(int(header.dimensions_and_flags.x / 2u), int(header.dimensions_and_flags.y / 2u), int(header.dimensions_and_flags.z / 2u));
	if (any(local < int3(0)) || any(local >= int3(header.dimensions_and_flags.xyz))) return false;
	r_index = uint(local.x) + uint(local.y) * header.dimensions_and_flags.x + uint(local.z) * header.dimensions_and_flags.x * header.dimensions_and_flags.y;
	return true;
}

// Stable identities survive per-frame record packing. Resolve the current
// record before replaying a path payload so geometry/material generations and
// exact barycentrics are checked against this frame's scene.
static bool reusable_path_resolve_current_geometry(ulong stable_identity, constant GeometryRecord *geometry_records, uint geometry_record_count, thread uint &r_geometry_index) {
	for (uint index = 0u; index < geometry_record_count; index++) {
		const GeometryRecord geometry = geometry_records[index];
		const ulong current_identity = ulong(geometry.instance_identity_low) | (ulong(geometry.instance_identity_high) << 32u);
		if (current_identity == stable_identity) {
			r_geometry_index = index;
			return true;
		}
	}
	return false;
}

kernel void reusable_path_clear_staging(device ReusablePathSampleRecord *staging [[buffer(0)]], device atomic_uint *claims [[buffer(1)]], uint index [[thread_position_in_grid]]) {
	staging[index] = {};
	atomic_store_explicit(&claims[index], 0xffffffffu, memory_order_relaxed);
}

// The only persistent-cache write runs after tracing. It starts with an exact
// world-keyed copy from immutable previous storage, then deterministically
// replaces that cell with its single trace-owned staged record when eligible.
kernel void reusable_path_reduce(constant ReusablePathHeader &header [[buffer(0)]], device const ReusablePathSampleRecord *previous [[buffer(1)]], device const ReusablePathSampleRecord *staging [[buffer(2)]], device ReusablePathSampleRecord *next [[buffer(3)]], device MaterialDiagnosticAtomic &material_diagnostic [[buffer(4)]], uint index [[thread_position_in_grid]]) {
	if (index >= reusable_path_cell_count(header)) return;
	ReusablePathSampleRecord output = {};
	const int3 key = reusable_path_key_for_index(index, header);
	if (header.previous_center_and_flags.w != 0) {
		const int3 old_local = key - header.previous_center_and_flags.xyz + int3(int(header.dimensions_and_flags.x / 2u), int(header.dimensions_and_flags.y / 2u), int(header.dimensions_and_flags.z / 2u));
		if (all(old_local >= int3(0)) && all(old_local < int3(header.dimensions_and_flags.xyz))) {
			const uint old_index = uint(old_local.x) + uint(old_local.y) * header.dimensions_and_flags.x + uint(old_local.z) * header.dimensions_and_flags.x * header.dimensions_and_flags.y;
			const ReusablePathSampleRecord old = previous[old_index];
			if (old.abi_version == 3u && (old.flags & 1u) != 0u) {
				// The exact-key test below avoids carrying a record into a different
				// world cell after scroll/recenter; no camera state participates.
				const int3 old_key = int3(floor(old.source_primary_world_position.xyz / max(float(header.center_and_cell_size.w), 0.25f)));
				if (all(old_key == key) && all(isfinite(old.source_primary_geometric_normal.xyz))) output = old;
			}
		}
	}
	const ReusablePathSampleRecord candidate = staging[index];
	if (candidate.abi_version == 3u && (candidate.flags & 1u) != 0u) output = candidate;
	next[index] = output;
	if (output.abi_version == 3u && (output.flags & 1u) != 0u) atomic_fetch_add_explicit(&material_diagnostic.reusable_path_occupied, 1u, memory_order_relaxed);
	atomic_fetch_add_explicit(&material_diagnostic.reusable_path_updates, 1u, memory_order_relaxed);
}

static uint regir_cell_count(constant RegirHeader &header) {
	return header.dimensions_and_blocks.x * header.dimensions_and_blocks.y * header.dimensions_and_blocks.z;
}

static int3 regir_key_for_index(uint index, constant RegirHeader &header) {
	const uint dx = header.dimensions_and_blocks.x;
	const uint dy = header.dimensions_and_blocks.y;
	const int3 local = int3(int(index % dx), int((index / dx) % dy), int(index / (dx * dy)));
	return header.center_and_cell_size.xyz + local - int3(int(dx / 2u), int(dy / 2u), int(header.dimensions_and_blocks.z / 2u));
}

static bool regir_index_for_key(int3 key, constant RegirHeader &header, thread uint &r_index) {
	const int3 local = key - header.center_and_cell_size.xyz + int3(int(header.dimensions_and_blocks.x / 2u), int(header.dimensions_and_blocks.y / 2u), int(header.dimensions_and_blocks.z / 2u));
	if (any(local < int3(0)) || any(local >= int3(header.dimensions_and_blocks.xyz))) return false;
	r_index = uint(local.x) + uint(local.y) * header.dimensions_and_blocks.x + uint(local.z) * header.dimensions_and_blocks.x * header.dimensions_and_blocks.y;
	return true;
}

kernel void regir_scroll(constant RegirHeader &header [[buffer(0)]], device const RegirCell *previous [[buffer(1)]], device RegirCell *next [[buffer(2)]], uint index [[thread_position_in_grid]]) {
	if (index >= regir_cell_count(header)) return;
	RegirCell cell = {};
	const int3 key = regir_key_for_index(index, header);
	cell.key_valid = int4(key, 0);
	cell.revisions = header.revisions;
	uint old_index = 0u;
	const int3 old_local = key - header.previous_center_and_flags.xyz + int3(int(header.dimensions_and_blocks.x / 2u), int(header.dimensions_and_blocks.y / 2u), int(header.dimensions_and_blocks.z / 2u));
	if (header.previous_center_and_flags.w != 0 && all(old_local >= int3(0)) && all(old_local < int3(header.dimensions_and_blocks.xyz))) {
		old_index = uint(old_local.x) + uint(old_local.y) * header.dimensions_and_blocks.x + uint(old_local.z) * header.dimensions_and_blocks.x * header.dimensions_and_blocks.y;
		const RegirCell prior = previous[old_index];
		if (prior.key_valid.w != 0 && all(prior.key_valid.xyz == key) && all(prior.revisions == header.revisions)) cell = prior;
	}
	next[index] = cell;
}

kernel void regir_classify(constant RegirHeader &header [[buffer(0)]], texture2d<float, access::read> primary_world_position [[texture(0)]], texture2d<uint, access::read> reservoir [[texture(1)]], texture2d<float, access::read> reservoir_sample [[texture(2)]], texture2d<float, access::read> reservoir_metadata [[texture(3)]], device RegirCandidate *staging [[buffer(1)]], uint2 pixel [[thread_position_in_grid]], uint2 local_position [[thread_position_in_threadgroup]], uint2 group_position [[threadgroup_position_in_grid]]) {
	threadgroup RegirCandidate candidates[64];
	const uint lane = local_position.y * 8u + local_position.x;
	RegirCandidate candidate = {};
	if (pixel.x < primary_world_position.get_width() && pixel.y < primary_world_position.get_height()) {
		const float4 position = primary_world_position.read(pixel);
		const float4 metadata = reservoir_metadata.read(pixel);
		if (position.w > 0.5f && all(isfinite(position.xyz)) && metadata.w > 0.5f && metadata.x > 0.0f && metadata.y > 0.0f) {
			const float cell_size = max(float(header.center_and_cell_size.w), 0.25f);
			const int3 key = int3(floor(position.xyz / cell_size));
			uint cell_index = 0u;
			if (regir_index_for_key(key, header, cell_index)) {
				candidate.cell_index = cell_index;
				candidate.valid = 1u;
				candidate.source = reservoir.read(pixel);
				candidate.sample = reservoir_sample.read(pixel);
				candidate.metadata = metadata;
			}
		}
	}
	candidates[lane] = candidate;
	threadgroup_barrier(mem_flags::mem_threadgroup);
	if (lane != 0u) return;
	const uint block = group_position.y * ((primary_world_position.get_width() + 7u) / 8u) + group_position.x;
	const uint cell_count = regir_cell_count(header);
	for (uint cell = 0u; cell < cell_count; cell++) {
		RegirCandidate selected = {};
		for (uint i = 0u; i < 64u; i++) {
			const RegirCandidate value = candidates[i];
			if (value.valid == 0u || value.cell_index != cell) continue;
			// Stable tie break by lane makes this stage deterministic without
			// atomics or trace-grid writes.
			if (selected.valid == 0u || value.metadata.y > selected.metadata.y) selected = value;
		}
		staging[block * cell_count + cell] = selected;
	}
}

kernel void regir_reduce(constant RegirHeader &header [[buffer(0)]], device const RegirCandidate *staging [[buffer(1)]], device RegirCell *cells [[buffer(2)]], uint cell_index [[thread_position_in_grid]]) {
	const uint cell_count = regir_cell_count(header);
	if (cell_index >= cell_count) return;
	RegirCell selected = cells[cell_index];
	float selected_weight = selected.key_valid.w != 0 ? selected.metadata.y : -1.0f;
	for (uint block = 0u; block < header.dimensions_and_blocks.w; block++) {
		const RegirCandidate candidate = staging[block * cell_count + cell_index];
		if (candidate.valid == 0u || !all(isfinite(candidate.metadata)) || !(candidate.metadata.x > 0.0f) || !(candidate.metadata.y > 0.0f) || !(candidate.sample.w > 0.0f)) continue;
		if (selected.key_valid.w == 0 || candidate.metadata.y > selected_weight) {
			selected.key_valid = int4(regir_key_for_index(cell_index, header), 1);
			selected.source = candidate.source;
			selected.sample = candidate.sample;
			selected.metadata = candidate.metadata;
			selected.revisions = header.revisions;
			selected_weight = candidate.metadata.y;
		}
	}
	cells[cell_index] = selected;
}

static float direct_luminance(float3 value) { return max(dot(max(value, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)), 0.0f); }

// A camera-centered, bounded ReGIR-style local proposal. The hashed grid cell
// chooses a fixed 32-light window; its PDF is mixed with a uniform global
// proposal, preserving support and an explicit non-zero PDF for every light.
static uint regir_window_begin(float3 world_position, float3 camera_position, uint light_count) {
	const int3 cell = int3(floor((world_position - camera_position) / 4.0f));
	const uint cell_hash = hash_u32(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u);
	return light_count > 0u ? cell_hash % light_count : 0u;
}

static float regir_local_pdf(float3 world_position, float3 camera_position, uint selected, constant PunctualLightRecord *lights, uint light_count) {
	const uint count = min(light_count, 32u);
	if (count == 0u) return 0.0f;
	const uint begin = regir_window_begin(world_position, camera_position, light_count);
	float total = 0.0f;
	float selected_weight = 0.0f;
	for (uint i = 0u; i < count; i++) {
		const uint index = (begin + i) % light_count;
		const float3 offset = lights[index].position_range.xyz - world_position;
		const float weight = max(dot(lights[index].radiance_attenuation.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f) / max(dot(offset, offset), 1.0f);
		total += weight;
		if (index == selected) selected_weight = weight;
	}
	return total > 0.0f ? selected_weight / total : 0.0f;
}

static uint regir_sample_local(float3 world_position, float3 camera_position, float random, constant PunctualLightRecord *lights, uint light_count) {
	const uint count = min(light_count, 32u);
	const uint begin = regir_window_begin(world_position, camera_position, light_count);
	float total = 0.0f;
	for (uint i = 0u; i < count; i++) {
		const float3 offset = lights[(begin + i) % light_count].position_range.xyz - world_position;
		total += max(dot(lights[(begin + i) % light_count].radiance_attenuation.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f) / max(dot(offset, offset), 1.0f);
	}
	if (!(total > 0.0f)) return begin;
	float target = random * total;
	for (uint i = 0u; i < count; i++) {
		const uint index = (begin + i) % light_count;
		const float3 offset = lights[index].position_range.xyz - world_position;
		target -= max(dot(lights[index].radiance_attenuation.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f) / max(dot(offset, offset), 1.0f);
		if (target <= 0.0f) return index;
	}
	return (begin + count - 1u) % light_count;
}

static uint find_emissive_triangle(DirectLightReservoir reservoir, constant EmissiveTriangleRecord *triangles, uint triangle_count) {
	if (reservoir.triangle_record < triangle_count) {
		const EmissiveTriangleRecord candidate = triangles[reservoir.triangle_record];
		if (candidate.primitive_id == reservoir.source_index && candidate.instance_identity_low == reservoir.source_identity.x && candidate.instance_identity_high == reservoir.source_identity.y && candidate.weight > 0.0f) return reservoir.triangle_record;
	}
	// The previous table can be ordered differently after instance changes. Scan
	// only the fixed bounded table to remap the persistent source tuple.
	for (uint index = 0u; index < triangle_count; index++) {
		const EmissiveTriangleRecord candidate = triangles[index];
		if (candidate.primitive_id == reservoir.source_index && candidate.instance_identity_low == reservoir.source_identity.x && candidate.instance_identity_high == reservoir.source_identity.y && candidate.weight > 0.0f) return index;
	}
	return 0xffffffffu;
}

// Punctual light order is an upload detail. A direct reservoir keeps the full
// renderer stable id and treats its array index only as a cache. This mirrors
// emissive-triangle remapping above and prevents a light-list reorder from
// turning a valid reservoir into a different light.
static uint find_punctual_light(DirectLightReservoir reservoir, constant PunctualLightRecord *lights, uint light_count) {
	if (reservoir.source_index < light_count && all(lights[reservoir.source_index].stable_identity == reservoir.source_identity)) return reservoir.source_index;
	for (uint index = 0u; index < light_count; index++) {
		if (all(lights[index].stable_identity == reservoir.source_identity)) return index;
	}
	return 0xffffffffu;
}

static float3 evaluate_emissive_triangle(DirectLightReservoir reservoir, float3 position, float3 normal, float3 diffuse, float3 view_direction, float roughness, float3 f0, bool trace_visibility, constant MaterialRecord *materials, constant GeometryRecord *geometries, constant EmissiveTriangleRecord *triangles, uint triangle_count, float max_distance, constant Parameters &parameters,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic, raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table, thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector, raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	const uint triangle_index = find_emissive_triangle(reservoir, triangles, triangle_count);
	if (triangle_index == 0xffffffffu) return 0.0f;
	const EmissiveTriangleRecord triangle = triangles[triangle_index];
	constant GeometryRecord &geometry = geometries[triangle.instance_id];
	const float3 p0 = world_vertex_position(geometry, triangle_vertex_index(geometry, triangle.primitive_id, 0u));
	const float3 p1 = world_vertex_position(geometry, triangle_vertex_index(geometry, triangle.primitive_id, 1u));
	const float3 p2 = world_vertex_position(geometry, triangle_vertex_index(geometry, triangle.primitive_id, 2u));
	const float b1 = clamp(reservoir.barycentric.x, 0.0f, 1.0f);
	const float b2 = clamp(reservoir.barycentric.y, 0.0f, 1.0f - b1);
	const float3 light_position = p0 * (1.0f - b1 - b2) + p1 * b1 + p2 * b2;
	const float3 to_light = light_position - position;
	const float distance_squared = dot(to_light, to_light);
	const float distance = sqrt(distance_squared);
	if (distance <= 0.02f || (max_distance > 0.0f && distance > max_distance)) return 0.0f;
	const float3 light_direction = to_light / distance;
	const float surface_cosine = max(dot(normal, light_direction), 0.0f);
	const float3 emitter_normal = -normalize(cross(p1 - p0, p2 - p0));
	const float light_cosine = max(dot(emitter_normal, -light_direction), 0.0f);
	if (surface_cosine <= 0.0f || light_cosine <= 0.0f) return 0.0f;
	const MaterialRecord emitter_material = materials[triangle.instance_id];
	const float2 emitter_barycentric = float2(b1, b2);
	const MaterialSample emitter_sample = sample_material(emitter_material, geometries, triangle.instance_id, triangle.primitive_id, emitter_barycentric, distance, 0.001f, material_textures, material_sampler, parameters.material_texture_capacity);
	if ((emitter_material.flags & 1u) != 0u && sample_material_alpha(emitter_material, geometries, triangle.instance_id, triangle.primitive_id, emitter_barycentric, distance, 0.001f, material_textures, material_sampler, parameters.material_texture_capacity) < emitter_material.material_factors.z) return 0.0f;
	float3 selected_visibility_transmittance = float3(1.0f);
	if (trace_visibility) {
		raytracing::ray visibility_ray = { position + normal * 0.003f, light_direction, 0.001f, distance - 0.01f };
		selected_visibility_transmittance = thin_visibility_transmittance(visibility_ray, scene, opaque_scene, alpha_scene, 0xff, 0.001f, parameters, materials, geometries, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
		if (all(selected_visibility_transmittance <= float3(0.0f))) return 0.0f;
	}
	const float area_pdf = reservoir.proposal_pdf;
	const float light_pdf = area_pdf * distance_squared / light_cosine;
	const uint transport_flags = uint(parameters.ao_distance_strength_roughness_flags.w);
	const bool diffuse_bsdf_active = (transport_flags & 4u) != 0u;
	const bool specular_bsdf_active = (transport_flags & 1u) != 0u && roughness <= parameters.ao_distance_strength_roughness_flags.z;
	const float diffuse_pdf = diffuse_bsdf_active ? surface_cosine * (1.0f / M_PI_F) : 0.0f;
	const float specular_pdf = specular_bsdf_active ? ggx_vndf_reflection_pdf(view_direction, normal, light_direction, roughness) : 0.0f;
	const float diffuse_weight = diffuse_bsdf_active ? power_heuristic(light_pdf, diffuse_pdf) : 1.0f;
	const float specular_weight = specular_bsdf_active ? power_heuristic(light_pdf, specular_pdf) : 1.0f;
	const float3 bsdf_response = diffuse * (1.0f / M_PI_F) * surface_cosine * diffuse_weight + ggx_direct_response(view_direction, normal, light_direction, roughness, f0) * specular_weight;
	return selected_visibility_transmittance * bsdf_response * emitter_sample.emission * light_cosine / max(distance_squared, 0.0001f);
}

static float3 evaluate_reservoir_source(DirectLightReservoir reservoir, float3 position, float3 normal, float3 diffuse, float3 view_direction, float roughness, float3 f0, uint receiver_visibility_mask, bool trace_visibility, constant MaterialRecord *materials, constant GeometryRecord *geometries, constant EmissiveTriangleRecord *triangles, uint triangle_count, constant PunctualLightRecord *lights, uint light_count, uint frame_index, thread uint &state, float max_distance, constant Parameters &parameters,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic, raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table, thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector, raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (!reservoir.valid) return 0.0f;
	if (trace_visibility) atomic_fetch_add_explicit(&material_diagnostic.direct_selected_visibility, 1u, memory_order_relaxed);
	else atomic_fetch_add_explicit(&material_diagnostic.direct_candidate_evaluations, 1u, memory_order_relaxed);
	if (reservoir.source_type == 0u) {
		return evaluate_emissive_triangle(reservoir, position, normal, diffuse, view_direction, roughness, f0, trace_visibility, materials, geometries, triangles, triangle_count, max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, stbn_pixel);
	}
	if (reservoir.source_type == 1u) {
		const uint light_index = find_punctual_light(reservoir, lights, light_count);
		if (light_index == 0xffffffffu) return 0.0f;
		// triangle_record is a persistent area-light sample seed for this source.
		// It is intentionally evaluated with a fixed frame index so temporal/grid
		// reuse preserves the selected area point rather than resampling it.
		uint key_state = reservoir.triangle_record;
		return sample_punctual_lighting(position, normal, diffuse, view_direction, roughness, f0, receiver_visibility_mask, 0u, false, trace_visibility, lights + light_index, 1u, 0u, key_state, max_distance, parameters, materials, geometries, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, stbn_pixel);
	}
	return 0.0f;
}


static void reservoir_merge(thread DirectLightReservoir &reservoir, DirectLightReservoir candidate, float3 candidate_value, float random, bool reprojected, device MaterialDiagnosticAtomic &diagnostic) {
	if (!candidate.valid) return;
	if (!all(isfinite(candidate_value))) {
		atomic_fetch_add_explicit(&diagnostic.nonfinite_lobe_samples, 1u, memory_order_relaxed);
		return;
	}
	const float target = direct_luminance(candidate_value);
	if (!(candidate.proposal_pdf > 0.0f) || !isfinite(candidate.proposal_pdf)) {
		atomic_fetch_add_explicit(&diagnostic.invalid_pdf_samples, 1u, memory_order_relaxed);
		return;
	}
	if (!(target > 0.0f)) return;
	const float weight = reprojected && candidate.target > 0.0f ? candidate.weight_sum * target / candidate.target : target / candidate.proposal_pdf;
	if (!(weight > 0.0f) || !isfinite(weight)) {
		atomic_fetch_add_explicit(&diagnostic.rejected_energy_samples, 1u, memory_order_relaxed);
		return;
	}
	const uint candidate_count = reprojected ? max(candidate.candidate_count, 1u) : 1u;
	const uint uncapped_count = reservoir.candidate_count + candidate_count;
	const float cap_scale = uncapped_count > 64u ? 64.0f / float(uncapped_count) : 1.0f;
	const float total = (reservoir.weight_sum + weight) * cap_scale;
	if (!(total > 0.0f) || !isfinite(total)) {
		atomic_fetch_add_explicit(&diagnostic.rejected_energy_samples, 1u, memory_order_relaxed);
		return;
	}
	const float selected_weight = weight * cap_scale;
	if (random * total < selected_weight) {
		candidate.target = target;
		candidate.weight_sum = total;
		candidate.candidate_count = min(uncapped_count, 64u);
		candidate.age = reprojected ? min(candidate.age + 1u, 30u) : 0u;
		candidate.valid = true;
		reservoir = candidate;
	} else {
		reservoir.weight_sum = total;
		reservoir.candidate_count = min(uncapped_count, 64u);
	}
}

static float2 oct_encode(float3 direction) {
	direction /= max(abs(direction.x) + abs(direction.y) + abs(direction.z), 0.000001f);
	float2 oct = direction.xy;
	if (direction.z < 0.0f) oct = (1.0f - abs(oct.yx)) * select(float2(-1.0f), float2(1.0f), oct >= 0.0f);
	return oct * 0.5f + 0.5f;
}

static float3 oct_decode(float2 uv) {
	float2 oct = uv * 2.0f - 1.0f;
	float3 value = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
	float fold = max(-value.z, 0.0f);
	value.xy += fold * select(float2(1.0f), float2(-1.0f), value.xy >= 0.0f);
	return normalize(value);
}

static float oct_jacobian(float2 uv, float border) {
	float scale = max(1.0f - 2.0f * border, 0.000001f);
	float2 oct = (uv - border) / scale * 2.0f - 1.0f;
	float3 value = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
	float fold = max(-value.z, 0.0f);
	value.xy += fold * select(float2(1.0f), float2(-1.0f), value.xy >= 0.0f);
	float length_value = length(value);
	return length_value > 0.000001f ? 4.0f / (scale * scale * length_value * length_value * length_value) : 0.0f;
}

static float3 environment_lookup(float3 world_direction, constant Parameters &parameters, texture2d<float, access::sample> radiance, sampler environment_sampler) {
	if (parameters.environment_info.w < 0.5f) return float3(0.002f);
	if (parameters.environment_info.w < 1.5f) return 0.0f;
	float3 local = normalize((parameters.radiance_from_world * float4(world_direction, 0.0f)).xyz);
	float2 uv = oct_encode(local) * parameters.environment_info.y + parameters.environment_info.x;
	return max(radiance.sample(environment_sampler, uv, level(0.0f)).rgb, 0.0f);
}

struct EnvironmentSample {
	float3 direction;
	float3 radiance;
	float pdf;
	bool valid;
};

static EnvironmentSample sample_environment(uint frame_index, uint pixel_seed, uint dimension, constant Parameters &parameters, texture2d<float, access::sample> radiance, texture2d<float, access::read> importance, sampler environment_sampler, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	EnvironmentSample result = { 0.0f, 0.0f, 0.0f, false };
	if (parameters.environment_info.w < 1.5f) return result;
	const uint mip_count = uint(parameters.environment_info.z);
	const float total = importance.read(uint2(0), mip_count - 1u).r;
	if (!(total > 0.0f) || !isfinite(total)) return result;
	uint2 coordinate = uint2(0);
	float target = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, dimension, parameters, stbn_scalar_volume, stbn_pixel);
	for (int mip = int(mip_count) - 2; mip >= 0; mip--) {
		const uint2 size = max(parameters.environment_importance_dimensions >> uint(mip), uint2(1));
		float child_weights[4];
		float child_total = 0.0f;
		for (uint child = 0u; child < 4u; child++) {
			uint2 child_coordinate = coordinate * 2u + uint2(child & 1u, child >> 1u);
			child_weights[child] = all(child_coordinate < size) ? max(importance.read(child_coordinate, uint(mip)).r, 0.0f) : 0.0f;
			child_total += child_weights[child];
		}
		if (!(child_total > 0.0f) || !isfinite(child_total)) return result;
		float select_value = target * child_total;
		uint selected = 3u;
		float accumulated = 0.0f;
		float prefix = 0.0f;
		for (uint child = 0u; child < 4u; child++) { accumulated += child_weights[child]; if (select_value < accumulated) { selected = child; break; } prefix += child_weights[child]; }
		target = child_weights[selected] > 0.0f ? clamp((select_value - prefix) / child_weights[selected], 0.0f, 0.99999994f) : 0.0f;
		coordinate = coordinate * 2u + uint2(selected & 1u, selected >> 1u);
	}
	if (any(coordinate >= parameters.environment_dimensions)) return result;
	const float weight = importance.read(coordinate, 0).r;
	float2 jitter = float2(hammersley_dimension(frame_index, 0u, 1u, pixel_seed, dimension + 1u, parameters, stbn_scalar_volume, stbn_pixel), hammersley_dimension(frame_index, 0u, 1u, pixel_seed, dimension + 2u, parameters, stbn_scalar_volume, stbn_pixel));
	float2 uv = (float2(coordinate) + jitter) / float2(parameters.environment_dimensions);
	const float solid_angle = oct_jacobian(uv, parameters.environment_info.x) / float(parameters.environment_dimensions.x * parameters.environment_dimensions.y);
	if (!(weight > 0.0f) || !(solid_angle > 0.0f) || !isfinite(weight)) return result;
	const float2 oct_uv = (uv - parameters.environment_info.x) / parameters.environment_info.y;
	result.direction = normalize((parameters.world_from_radiance * float4(oct_decode(oct_uv), 0.0f)).xyz);
	result.radiance = max(radiance.sample(environment_sampler, uv, level(0.0f)).rgb, 0.0f);
	result.pdf = weight / total / solid_angle;
	result.valid = isfinite(result.pdf) && result.pdf > 0.0f;
	return result;
}

static float environment_pdf(float3 world_direction, constant Parameters &parameters, texture2d<float, access::read> importance) {
	if (parameters.environment_info.w < 1.5f) return 0.0f;
	float total = importance.read(uint2(0), uint(parameters.environment_info.z) - 1u).r;
	if (!(total > 0.0f) || !isfinite(total)) return 0.0f;
	float3 local = normalize((parameters.radiance_from_world * float4(world_direction, 0.0f)).xyz);
	float2 uv = oct_encode(local) * parameters.environment_info.y + parameters.environment_info.x;
	uint2 coordinate = min(uint2(uv * float2(parameters.environment_dimensions)), parameters.environment_dimensions - 1u);
	float solid_angle = oct_jacobian(uv, parameters.environment_info.x) / float(parameters.environment_dimensions.x * parameters.environment_dimensions.y);
	float weight = importance.read(coordinate, 0).r;
	return weight > 0.0f && solid_angle > 0.0f ? weight / total / solid_angle : 0.0f;
}

static float3 sample_environment_lighting(float3 world_position, float3 world_normal, float3 diffuse_albedo, float3 view_direction, float roughness, float3 f0, uint frame_index, uint pixel_seed, uint dimension, bool primary_mis, constant Parameters &parameters, constant MaterialRecord *materials, constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic, raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table, texture2d<float, access::sample> radiance, texture2d<float, access::read> importance, sampler environment_sampler, thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector, raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (all(diffuse_albedo <= float3(0.0001f)) && all(f0 <= float3(0.0001f))) return 0.0f;
	EnvironmentSample sample = sample_environment(frame_index, pixel_seed, dimension, parameters, radiance, importance, environment_sampler, stbn_scalar_volume, stbn_pixel);
	if (!sample.valid) return 0.0f;
	float cosine = max(dot(world_normal, sample.direction), 0.0f);
	if (cosine <= 0.0f) return 0.0f;
	raytracing::ray visibility = { world_position + world_normal * 0.003f, sample.direction, 0.001f, parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 100000.0f };
	const float3 visibility_transmittance = thin_visibility_transmittance(visibility, scene, opaque_scene, alpha_scene, 0xff, 0.001f, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
	if (all(visibility_transmittance <= float3(0.0f))) return 0.0f;
	float mis_weight = 1.0f;
	if (primary_mis) {
		const float bsdf_pdf = cosine * (1.0f / M_PI_F);
		mis_weight = sample.pdf / max(sample.pdf + bsdf_pdf, 0.000001f);
	}
	const float3 bsdf_response = diffuse_albedo * (1.0f / M_PI_F) * cosine + ggx_direct_response(view_direction, world_normal, sample.direction, roughness, f0);
	return visibility_transmittance * bsdf_response * sample.radiance * mis_weight / sample.pdf;
}

static float portal_pdf(float3 world_position, float3 direction, constant PortalRecord &portal) {
	const float3 normal_unscaled = cross(portal.axis_u.xyz, portal.axis_v.xyz);
	const float area = 4.0f * length(normal_unscaled);
	if (!(area > 0.000001f)) return 0.0f;
	const float3 normal = normal_unscaled / (area * 0.25f);
	const float denominator = dot(normal, direction);
	if (abs(denominator) <= 0.000001f) return 0.0f;
	const float distance = dot(normal, portal.center_weight.xyz - world_position) / denominator;
	if (!(distance > 0.001f)) return 0.0f;
	const float3 offset = world_position + direction * distance - portal.center_weight.xyz;
	const float uu = dot(portal.axis_u.xyz, portal.axis_u.xyz);
	const float uv = dot(portal.axis_u.xyz, portal.axis_v.xyz);
	const float vv = dot(portal.axis_v.xyz, portal.axis_v.xyz);
	const float determinant = uu * vv - uv * uv;
	if (!(determinant > 0.000001f)) return 0.0f;
	const float u = (dot(offset, portal.axis_u.xyz) * vv - dot(offset, portal.axis_v.xyz) * uv) / determinant;
	const float v = (dot(offset, portal.axis_v.xyz) * uu - dot(offset, portal.axis_u.xyz) * uv) / determinant;
	return abs(u) <= 1.0f && abs(v) <= 1.0f ? distance * distance / max(area * abs(dot(normal, -direction)), 0.000001f) : 0.0f;
}

// Environment importance and rectangular openings form a true mixture: the
// environment PDF is retained in every portal direction and every portal PDF
// is retained for an environment-selected direction. Portals are proposals,
// not occluders or an alternate environment source.
static float3 sample_environment_portal_mixture(float3 world_position, float3 world_normal, float3 diffuse_albedo, float3 view_direction, float roughness, float3 f0, uint frame_index, uint pixel_seed, uint dimension, bool primary_mis, constant Parameters &parameters, constant MaterialRecord *materials, constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic, raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table, constant PortalRecord *portals, texture2d<float, access::sample> radiance, texture2d<float, access::read> importance, sampler environment_sampler, thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector, raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (parameters.portal_count == 0u) return sample_environment_lighting(world_position, world_normal, diffuse_albedo, view_direction, roughness, f0, frame_index, pixel_seed, dimension, primary_mis, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, radiance, importance, environment_sampler, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, stbn_pixel);
	float total_weight = 1.0f;
	for (uint i = 0u; i < parameters.portal_count; i++) total_weight += max(portals[i].center_weight.w, 0.0f);
	const float choose = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, dimension, parameters, stbn_scalar_volume, stbn_pixel);
	float3 direction = 0.0f;
	float base_pdf = 0.0f;
	float3 environment_radiance = 0.0f;
	if (choose < 1.0f / total_weight) {
		EnvironmentSample sample = sample_environment(frame_index, pixel_seed, dimension + 1u, parameters, radiance, importance, environment_sampler, stbn_scalar_volume, stbn_pixel);
		if (!sample.valid) return 0.0f;
		direction = sample.direction;
		base_pdf = sample.pdf / total_weight;
		environment_radiance = sample.radiance;
	} else {
		float target = choose * total_weight - 1.0f;
		uint selected = parameters.portal_count - 1u;
		float prefix = 0.0f;
		for (uint i = 0u; i < parameters.portal_count; i++) { prefix += max(portals[i].center_weight.w, 0.0f); if (target <= prefix) { selected = i; break; } }
		const float u = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, dimension + 1u, parameters, stbn_scalar_volume, stbn_pixel) * 2.0f - 1.0f;
		const float v = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, dimension + 2u, parameters, stbn_scalar_volume, stbn_pixel) * 2.0f - 1.0f;
		const float3 point = portals[selected].center_weight.xyz + portals[selected].axis_u.xyz * u + portals[selected].axis_v.xyz * v;
		direction = normalize(point - world_position);
		base_pdf = max(portals[selected].center_weight.w, 0.0f) / total_weight * portal_pdf(world_position, direction, portals[selected]);
		environment_radiance = environment_lookup(direction, parameters, radiance, environment_sampler);
	}
	float mixture_pdf = base_pdf;
	const float environment_pdf_value = environment_pdf(direction, parameters, importance);
	mixture_pdf = environment_pdf_value / total_weight;
	for (uint i = 0u; i < parameters.portal_count; i++) mixture_pdf += max(portals[i].center_weight.w, 0.0f) / total_weight * portal_pdf(world_position, direction, portals[i]);
	const float cosine = max(dot(world_normal, direction), 0.0f);
	if (!(mixture_pdf > 0.0f) || cosine <= 0.0f) return 0.0f;
	raytracing::ray visibility = { world_position + world_normal * 0.003f, direction, 0.001f, parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 100000.0f };
	const float3 visibility_transmittance = thin_visibility_transmittance(visibility, scene, opaque_scene, alpha_scene, 0xff, 0.001f, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
	if (all(visibility_transmittance <= float3(0.0f))) return 0.0f;
	const float bsdf_pdf = cosine * (1.0f / M_PI_F);
	const float mis = primary_mis ? mixture_pdf / max(mixture_pdf + bsdf_pdf, 0.000001f) : 1.0f;
	const float3 bsdf_response = diffuse_albedo * (1.0f / M_PI_F) * cosine + ggx_direct_response(view_direction, world_normal, direction, roughness, f0);
	return visibility_transmittance * bsdf_response * environment_radiance * mis / mixture_pdf;
}

// A finite, uniform-solid-angle solar lobe. Its perpendicular irradiance is
// pre-attenuated by the source-direction cloud scalar on the CPU, so the
// visibility estimator deliberately has no cloud multiplier of its own.
static float3 sample_solar_lobe_lighting(float3 world_position, float3 world_normal, float3 diffuse_albedo, float3 view_direction, float roughness, float3 f0, uint frame_index, uint pixel_seed, constant Parameters &parameters, constant MaterialRecord *materials, constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
	sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic, raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table, thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector, raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene, texture2d_array<ushort, access::read> stbn_scalar_volume, uint2 stbn_pixel) {
	if (parameters.solar_perpendicular_irradiance_enabled.w < 0.5f || (all(diffuse_albedo <= float3(0.0001f)) && all(f0 <= float3(0.0001f)))) return 0.0f;
	const float radius = parameters.solar_current_direction_radius.w;
	if (!(radius > 0.0f) || !isfinite(radius)) return 0.0f;
	const float u = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, 17u, parameters, stbn_scalar_volume, stbn_pixel);
	const float v = hammersley_dimension(frame_index, 0u, 1u, pixel_seed, 18u, parameters, stbn_scalar_volume, stbn_pixel);
	const float cos_min = cos(radius);
	const float cosine_from_center = mix(cos_min, 1.0f, u);
	const float sine_from_center = sqrt(max(0.0f, 1.0f - cosine_from_center * cosine_from_center));
	const float3 center = normalize(parameters.solar_current_direction_radius.xyz);
	const float3 helper = abs(center.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
	const float3 tangent = normalize(cross(helper, center));
	const float3 bitangent = cross(center, tangent);
	const float phi = 6.28318530718f * v;
	const float3 direction = normalize(center * cosine_from_center + tangent * (cos(phi) * sine_from_center) + bitangent * (sin(phi) * sine_from_center));
	const float surface_cosine = max(dot(world_normal, direction), 0.0f);
	if (surface_cosine <= 0.0f) return 0.0f;
	raytracing::ray visibility = { world_position + world_normal * 0.003f, direction, 0.001f, 100000.0f };
	const float3 visibility_transmittance = thin_visibility_transmittance(visibility, scene, opaque_scene, alpha_scene, 0xff, 0.001f, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
	if (all(visibility_transmittance <= float3(0.0f))) return 0.0f;
	const float solid_angle = 6.28318530718f * max(1.0f - cos_min, 0.00000001f);
	const float pdf = 1.0f / solid_angle;
	const float sine_radius = sin(radius);
	const float3 radiance = max(parameters.solar_perpendicular_irradiance_enabled.xyz, 0.0f) / (M_PI_F * max(sine_radius * sine_radius, 0.00000001f));
	const float3 bsdf_response = diffuse_albedo * (1.0f / M_PI_F) * surface_cosine + ggx_direct_response(view_direction, world_normal, direction, roughness, f0);
	return visibility_transmittance * bsdf_response * radiance / pdf;
}

// Shared primary/secondary directional estimator. Visibility is evaluated only
// when the authored light has shadows, and opacity blends the blocked result.
static float3 sample_directional_secondary_lighting(float3 world_position, float3 world_normal, float3 diffuse_albedo, float3 view_direction, float roughness, float3 f0, uint receiver_mask, uint pixel_seed, constant Parameters &parameters, constant MaterialRecord *materials, constant GeometryRecord *geometry_records,
#if HYBRID_BINDLESS_MATERIALS
		constant MaterialTextureTable &material_textures,
#else
		array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures,
#endif
		sampler material_sampler, device MaterialDiagnosticAtomic &material_diagnostic, raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table, thread raytracing::intersector<raytracing::instancing, raytracing::triangle_data> &intersector, raytracing::instance_acceleration_structure scene, raytracing::instance_acceleration_structure opaque_scene, raytracing::instance_acceleration_structure alpha_scene) {
	if (parameters.directional_light_radiance_enabled.w < 0.5f || (receiver_mask & parameters.directional_light_cull_mask) == 0u || (all(diffuse_albedo <= float3(0.0001f)) && all(f0 <= float3(0.0001f)))) {
		return 0.0f;
	}
	uint state = pixel_seed;
	const float3 direction = sample_cone(normalize(parameters.light_direction_and_reflection_strength.xyz), parameters.directional_light_angular_radius, state);
	const float cosine = max(dot(world_normal, direction), 0.0f);
	if (cosine <= 0.0f) {
		return 0.0f;
	}
	const uint ray_mask = parameters.directional_shadow_caster_mask & 0xffu;
	float3 visibility_weight = float3(1.0f);
	if ((parameters.directional_flags & 1u) != 0u && ray_mask != 0u && parameters.directional_shadow_opacity > 0.0f) {
		raytracing::ray visibility = { world_position + world_normal * 0.003f, direction, 0.001f, parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 100000.0f };
		const float3 thin_visibility = thin_visibility_transmittance(visibility, scene, opaque_scene, alpha_scene, ray_mask, max(parameters.directional_light_angular_radius, 0.00025f), parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
		visibility_weight = mix(float3(1.0f - clamp(parameters.directional_shadow_opacity, 0.0f, 1.0f)), thin_visibility, clamp(parameters.directional_shadow_opacity, 0.0f, 1.0f));
	}
	const float3 bsdf_response = diffuse_albedo * (1.0f / M_PI_F) * cosine + ggx_direct_response(view_direction, world_normal, direction, roughness, f0) * parameters.directional_specular_amount;
	return bsdf_response * parameters.directional_light_radiance_enabled.xyz * visibility_weight;
}

kernel void environment_importance_build(constant Parameters &parameters [[buffer(0)]], device EnvironmentDiagnosticAtomic &diagnostic [[buffer(1)]], texture2d<float, access::sample> radiance [[texture(0)]], texture2d<float, access::write> importance [[texture(1)]], sampler radiance_sampler [[sampler(0)]], uint2 pixel [[thread_position_in_grid]]) {
	if (any(pixel >= parameters.environment_importance_dimensions)) return;
	if (any(pixel >= parameters.environment_dimensions)) { importance.write(float4(0.0f), pixel, 0); return; }
	float2 uv = (float2(pixel) + 0.5f) / float2(parameters.environment_dimensions);
	bool interior = all(uv >= parameters.environment_info.x) && all(uv <= 1.0f - parameters.environment_info.x);
	float3 rgb = radiance.sample(radiance_sampler, uv, level(0.0f)).rgb;
	bool finite_rgb = all(isfinite(rgb));
	if (!finite_rgb) atomic_fetch_add_explicit(&diagnostic.nonfinite_texel_count, 1u, memory_order_relaxed);
	float luminance = finite_rgb && interior ? dot(max(rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)) : 0.0f;
	const float texel_count = float(parameters.environment_dimensions.x * parameters.environment_dimensions.y);
	const float proposal_floor = interior && finite_rgb ? 0.000001f / texel_count : 0.0f;
	float weight = isfinite(luminance) ? max(luminance * oct_jacobian(uv, parameters.environment_info.x) / texel_count, proposal_floor) : 0.0f;
	if (interior && finite_rgb && isfinite(luminance)) atomic_fetch_max_explicit(&diagnostic.finite_peak_luminance_bits, as_type<uint>(max(luminance, 0.0f)), memory_order_relaxed);
	if (isfinite(weight)) atomic_fetch_max_explicit(&diagnostic.maximum_texel_weight_bits, as_type<uint>(max(weight, 0.0f)), memory_order_relaxed);
	importance.write(float4(max(weight, 0.0f)), pixel, 0);
}

kernel void environment_importance_reduce(constant Parameters &parameters [[buffer(0)]], texture2d<float, access::read_write> importance [[texture(0)]], uint2 pixel [[thread_position_in_grid]]) {
	const uint destination_mip = parameters.frame_index;
	const uint2 destination_size = max(parameters.environment_importance_dimensions >> destination_mip, uint2(1));
	if (any(pixel >= destination_size)) return;
	const uint source_mip = destination_mip - 1u;
	const uint2 source_size = max(parameters.environment_importance_dimensions >> source_mip, uint2(1));
	float sum = 0.0f;
	for (uint y = 0u; y < 2u; y++) for (uint x = 0u; x < 2u; x++) { const uint2 child = pixel * 2u + uint2(x, y); if (all(child < source_size)) sum += max(importance.read(child, source_mip).r, 0.0f); }
	importance.write(float4(sum), pixel, destination_mip);
}

kernel void environment_importance_diagnostic(constant Parameters &parameters [[buffer(0)]], device EnvironmentDiagnosticAtomic &diagnostic [[buffer(1)]], texture2d<float, access::sample> radiance [[texture(0)]], texture2d<float, access::read> importance [[texture(1)]], sampler radiance_sampler [[sampler(0)]], uint2 pixel [[thread_position_in_grid]]) {
	if (any(pixel >= parameters.environment_dimensions)) return;
	float2 uv = (float2(pixel) + 0.5f) / float2(parameters.environment_dimensions);
	bool interior = all(uv >= parameters.environment_info.x) && all(uv <= 1.0f - parameters.environment_info.x);
	float3 rgb = radiance.sample(radiance_sampler, uv, level(0.0f)).rgb;
	if (interior && all(isfinite(rgb))) {
		float luminance = dot(max(rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
		uint peak_bits = atomic_load_explicit(&diagnostic.finite_peak_luminance_bits, memory_order_relaxed);
		if (isfinite(luminance) && as_type<uint>(max(luminance, 0.0f)) == peak_bits) {
			atomic_fetch_max_explicit(&diagnostic.finite_peak_red_bits, as_type<uint>(max(rgb.r, 0.0f)), memory_order_relaxed);
			atomic_fetch_max_explicit(&diagnostic.finite_peak_green_bits, as_type<uint>(max(rgb.g, 0.0f)), memory_order_relaxed);
			atomic_fetch_max_explicit(&diagnostic.finite_peak_blue_bits, as_type<uint>(max(rgb.b, 0.0f)), memory_order_relaxed);
		}
	}
	if (all(pixel == uint2(0))) {
		float total = importance.read(uint2(0), uint(parameters.environment_info.z) - 1u).r;
		atomic_store_explicit(&diagnostic.total_importance_weight_bits, as_type<uint>(isfinite(total) ? max(total, 0.0f) : 0.0f), memory_order_relaxed);
	}
}

kernel void trace_hybrid_shadow(
    raytracing::instance_acceleration_structure scene [[buffer(0)]],
    constant Parameters &parameters [[buffer(1)]],
	constant MaterialRecord *materials [[buffer(2)]],
	constant GeometryRecord *geometry_records [[buffer(3)]],
#if HYBRID_BINDLESS_MATERIALS
	constant MaterialTextureTable &material_textures [[buffer(8)]],
#else
	array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures [[texture(12)]],
#endif
	device MaterialDiagnosticAtomic &material_diagnostic [[buffer(9)]],
	raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table [[buffer(10)]],
	raytracing::instance_acceleration_structure opaque_scene [[buffer(17)]],
	raytracing::instance_acceleration_structure alpha_scene [[buffer(22)]],
    texture2d<float, access::read> depth_texture [[texture(0)]],
    texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
    texture2d<float, access::write> effect_texture [[texture(2)]],
	sampler material_sampler [[sampler(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float depth = depth_texture.read(pixel).r;
    if (depth <= 0.000001f) {
        effect_texture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
        return;
    }
    float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
	// Forward+ applies its TAA offset to the raster projection. Convert the
	// jittered depth sample back to the unjittered projection used below.
	float2 ndc = uv * 2.0f - 1.0f;
    float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
    float3 view_position = view_h.xyz / view_h.w;
    float3 world_position = (parameters.world_from_view * float4(view_position, 1.0f)).xyz;
    float3 view_normal = normalize(normal_roughness_texture.read(pixel).xyz * 2.0f - 1.0f);
    float3 world_normal = normalize((parameters.world_from_view * float4(view_normal, 0.0f)).xyz);
    float3 light_direction = normalize(parameters.light_direction_and_reflection_strength.xyz);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
    uint state = hash_u32(pixel.x + pixel.y * parameters.dimensions.x);
	uint flags = uint(parameters.ao_distance_strength_roughness_flags.w);
	const bool fresh_ray_oracle = (parameters.experimental_feature_flags & 0x80000000u) != 0u;
	float occlusion = 1.0f;
	if ((flags & 2u) != 0u) {
		float angle = random_float(state) * 6.28318530718f;
		float3 helper = abs(world_normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
		float3 tangent = normalize(cross(helper, world_normal));
		float3 bitangent = cross(world_normal, tangent);
		float3 ao_direction = normalize(world_normal + 0.65f * (cos(angle) * tangent + sin(angle) * bitangent));
		raytracing::ray ao_ray = { world_position + world_normal * 0.002f, ao_direction, 0.001f, parameters.ao_distance_strength_roughness_flags.x };
		auto blocker = hybrid_intersect_split(ao_ray, scene, opaque_scene, alpha_scene, 0xff, 0.02f, ALPHA_RAY_CLASS_INDIRECT, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
		if (blocker.type == raytracing::intersection_type::triangle) {
			float proximity = 1.0f - clamp(blocker.distance / parameters.ao_distance_strength_roughness_flags.x, 0.0f, 1.0f);
			occlusion = 1.0f - proximity * parameters.ao_distance_strength_roughness_flags.y;
		}
	}
    float visibility = 0.0f;
    uint sample_count = max(parameters.shadow_sample_count, 1u);
	const uint ray_mask = parameters.directional_shadow_caster_mask & 0xffu;
	for (uint sample = 0; sample < sample_count; sample++) {
        float3 direction = sample_cone(light_direction, parameters.directional_light_angular_radius, state);
		raytracing::ray ray = { world_position + world_normal * 0.003f, direction, 0.001f, parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 100000.0f };
		const float3 transmittance = thin_visibility_transmittance(ray, scene, scene, scene, ray_mask, max(parameters.directional_light_angular_radius, 0.00025f), parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
        visibility += dot(transmittance, float3(0.2126f, 0.7152f, 0.0722f));
    }
    effect_texture.write(float4(occlusion, 0.0f, 0.0f, visibility / float(sample_count)), pixel);
}

static void clear_trace_outputs(
		uint2 pixel,
		texture2d<float, access::write> effect_texture,
		texture2d<float, access::write> guide_normal,
		texture2d<float, access::write> guide_diffuse,
		texture2d<float, access::write> guide_specular,
		texture2d<float, access::write> guide_roughness,
		texture2d<float, access::write> guide_denoise_strength,
		texture2d<float, access::write> guide_reactive,
		texture2d<float, access::write> guide_specular_distance,
		texture2d<float, access::write> guide_transparency,
		texture2d<uint, access::write> reservoir_output,
		texture2d<float, access::write> reservoir_surface_output,
		texture2d<float, access::write> reservoir_metadata_output,
		texture2d<float, access::write> reservoir_sample_output,
		texture2d<uint, access::write> reservoir_primary_identity_output,
		texture2d<float, access::write> primary_shading_output,
		texture2d<float, access::write> primary_world_position_output,
		texture2d<float, access::write> split_diffuse,
		texture2d<float, access::write> split_specular,
		texture2d<float, access::write> diffuse_history_output,
		texture2d<float, access::write> specular_history_output,
		texture2d<float, access::write> diffuse_moments_output,
		texture2d<float, access::write> specular_moments_output) {
	effect_texture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
	guide_normal.write(float4(0.0f), pixel);
	guide_diffuse.write(float4(0.0f), pixel);
	guide_specular.write(float4(0.0f), pixel);
	guide_roughness.write(float4(1.0f), pixel);
	guide_denoise_strength.write(float4(1.0f), pixel);
	guide_reactive.write(float4(0.0f), pixel);
	guide_specular_distance.write(float4(0.0f), pixel);
	guide_transparency.write(float4(0.0f), pixel);
	reservoir_output.write(uint4(0u), pixel);
	reservoir_surface_output.write(float4(0.0f), pixel);
	reservoir_metadata_output.write(float4(0.0f), pixel);
	reservoir_sample_output.write(float4(0.0f), pixel);
	reservoir_primary_identity_output.write(uint4(0u), pixel);
	primary_shading_output.write(float4(0.0f), pixel);
	primary_world_position_output.write(float4(0.0f), pixel);
	split_diffuse.write(float4(0.0f), pixel);
	split_specular.write(float4(0.0f), pixel);
	diffuse_history_output.write(float4(0.0f), pixel);
	specular_history_output.write(float4(0.0f), pixel);
	diffuse_moments_output.write(float4(0.0f), pixel);
	specular_moments_output.write(float4(0.0f), pixel);
}

// Full-screen classification owns background/invalid-primary clearing and
// emits each active pixel into exactly one exclusive queue. The entry retains
// all applicable need bits so combined transport executes once in trace_hybrid.
kernel void trace_classify(
		constant Parameters &parameters [[buffer(0)]],
		device MaterialDiagnosticAtomic &material_diagnostic [[buffer(1)]],
		device TraceCompactEntry *trace_queues [[buffer(2)]],
		device atomic_uint *trace_queue_counts [[buffer(3)]],
		depth2d<float, access::read> depth_texture [[texture(0)]],
		texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
		texture2d<float, access::read> primary_material_texture [[texture(2)]],
		texture2d<float, access::read> primary_geometry_texture [[texture(3)]],
		texture2d<uint, access::read> primary_flags_texture [[texture(4)]],
		texture2d<float, access::write> effect_texture [[texture(5)]],
		texture2d<float, access::write> guide_normal [[texture(6)]],
		texture2d<float, access::write> guide_diffuse [[texture(7)]],
		texture2d<float, access::write> guide_specular [[texture(8)]],
		texture2d<float, access::write> guide_roughness [[texture(9)]],
		texture2d<float, access::write> guide_denoise_strength [[texture(10)]],
		texture2d<float, access::write> guide_reactive [[texture(11)]],
		texture2d<float, access::write> guide_specular_distance [[texture(12)]],
		texture2d<float, access::write> guide_transparency [[texture(13)]],
		texture2d<uint, access::write> reservoir_output [[texture(14)]],
		texture2d<float, access::write> reservoir_surface_output [[texture(15)]],
		texture2d<float, access::write> reservoir_metadata_output [[texture(16)]],
		texture2d<float, access::write> reservoir_sample_output [[texture(17)]],
		texture2d<uint, access::write> reservoir_primary_identity_output [[texture(18)]],
		texture2d<float, access::write> primary_world_position_output [[texture(19)]],
		texture2d<float, access::write> split_diffuse [[texture(20)]],
		texture2d<float, access::write> split_specular [[texture(21)]],
		texture2d<float, access::write> diffuse_history_output [[texture(22)]],
		texture2d<float, access::write> specular_history_output [[texture(23)]],
		texture2d<float, access::write> diffuse_moments_output [[texture(24)]],
		texture2d<float, access::write> specular_moments_output [[texture(25)]],
		texture2d<float, access::read> diffuse_moments_input [[texture(26)]],
		texture2d<float, access::read> specular_moments_input [[texture(27)]],
		texture2d<float, access::read> velocity_texture [[texture(28)]],
		texture2d<float, access::write> primary_shading_output [[texture(29)]],
		texture2d<uint, access::read> primary_identity_texture [[texture(30)]],
		texture2d<float, access::read> reservoir_surface_input [[texture(31)]],
		texture2d<uint, access::read> reservoir_primary_identity_input [[texture(32)]],
		texture2d<float, access::read> primary_shading_input [[texture(33)]],
		texture2d<float, access::read> diffuse_transport_sample_input [[texture(34)]],
		texture2d<float, access::read> specular_transport_sample_input [[texture(35)]],
		uint2 pixel [[thread_position_in_grid]]) {
	if (any(pixel >= parameters.dimensions)) return;
	const float depth = depth_texture.read(pixel);
	if (!(depth > 0.0f) || !isfinite(depth)) {
		atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_inactive_pixels, 1u, memory_order_relaxed);
		clear_trace_outputs(pixel, effect_texture, guide_normal, guide_diffuse, guide_specular, guide_roughness, guide_denoise_strength, guide_reactive, guide_specular_distance, guide_transparency, reservoir_output, reservoir_surface_output, reservoir_metadata_output, reservoir_sample_output, reservoir_primary_identity_output, primary_shading_output, primary_world_position_output, split_diffuse, split_specular, diffuse_history_output, specular_history_output, diffuse_moments_output, specular_moments_output);
		return;
	}

	uint need_mask = TRACE_NEED_DIRECT;
	const bool raster_primary_available = parameters.raster_primary_surface != 0u;
	if (raster_primary_available) {
		const uint surface_flags = primary_flags_texture.read(pixel).x;
		const float4 raster_material = primary_material_texture.read(pixel);
		const float4 raster_geometry = primary_geometry_texture.read(pixel);
		const bool primary_valid = (surface_flags & 0x0fu) == 1u && (surface_flags & (1u << 4u)) != 0u && all(isfinite(raster_material)) && all(isfinite(raster_geometry)) && dot(raster_geometry.xyz, raster_geometry.xyz) >= 0.25f;
		if (!primary_valid) {
			// Classification owns this invalid raster-primary decision on the
			// compact path, so preserve the diagnostic that trace_hybrid used to
			// emit without allowing the invalid pixel to enter a queue.
			atomic_fetch_add_explicit(&material_diagnostic.primary_invalid_pixels, 1u, memory_order_relaxed);
			atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_inactive_pixels, 1u, memory_order_relaxed);
			clear_trace_outputs(pixel, effect_texture, guide_normal, guide_diffuse, guide_specular, guide_roughness, guide_denoise_strength, guide_reactive, guide_specular_distance, guide_transparency, reservoir_output, reservoir_surface_output, reservoir_metadata_output, reservoir_sample_output, reservoir_primary_identity_output, primary_shading_output, primary_world_position_output, split_diffuse, split_specular, diffuse_history_output, specular_history_output, diffuse_moments_output, specular_moments_output);
			return;
		}
		const float3 diffuse = max(raster_material.rgb * (1.0f - raster_material.a), 0.0f);
		const float3 f0 = mix(float3(0.08f * clamp(raster_geometry.a, 0.0f, 1.0f)), raster_material.rgb, raster_material.a);
		const float encoded_roughness = normal_roughness_texture.read(pixel).w;
		const float roughness = encoded_roughness > 0.5f ? (1.0f - encoded_roughness) * (255.0f / 127.0f) : encoded_roughness * (255.0f / 127.0f);
		// A stationary, geometry/residency-valid pixel can skip a fresh secondary
		// ray after its *transport metadata* converges. This state is independent
		// of image reconstruction: MetalFX consumes raw ray radiance, while Flux
		// retains only validated per-pixel variance/confidence for scheduling.
		// Camera movement, cuts, lighting revisions, disocclusion, material/scene
		// changes, and alpha surfaces clear the history-valid bit on the CPU, so
		// this never replaces exact work for an invalid history.
		const float2 velocity = velocity_texture.read(pixel).xy;
		const bool stationary = all(isfinite(velocity)) && max(abs(velocity.x), abs(velocity.y)) <= 0.00025f;
		const bool secondary_transport_metadata_valid = (parameters.reconstruction_flags & 8u) != 0u;
		// Validate the current raster-primary surface against the previous exact ray
		// primary before a raw secondary sample may replace a fresh ray. This is
		// deliberately independent of MetalFX's image history.
		const uint4 raster_identity = primary_identity_texture.read(pixel);
		const uint2 current_identity = uint2(raster_identity.x | (raster_identity.y << 16u), raster_identity.z | (raster_identity.w << 16u));
		const float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
		bool sample_reprojection_valid = secondary_transport_metadata_valid;
		uint2 previous_pixel = pixel;
		if (sample_reprojection_valid && all(isfinite(velocity))) {
			const float2 previous_uv = uv + velocity;
			sample_reprojection_valid = all(previous_uv >= 0.0f) && all(previous_uv < 1.0f);
			if (sample_reprojection_valid) previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
		} else if (sample_reprojection_valid) {
			const float2 ndc = uv * 2.0f - 1.0f;
			const float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
			const float3 world_position = (parameters.world_from_view * float4(view_h.xyz / view_h.w, 1.0f)).xyz;
			const float4 previous_clip = parameters.prev_clip_from_world * float4(world_position, 1.0f);
			sample_reprojection_valid = previous_clip.w > 0.0f;
			if (sample_reprojection_valid) {
				const float2 previous_uv = float2(previous_clip.x / previous_clip.w * 0.5f + 0.5f, 0.5f - previous_clip.y / previous_clip.w * 0.5f);
				sample_reprojection_valid = all(previous_uv >= 0.0f) && all(previous_uv < 1.0f);
				if (sample_reprojection_valid) previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
			}
		}
		const float2 ndc = uv * 2.0f - 1.0f;
		const float4 current_view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
		const float3 current_world_position = (parameters.world_from_view * float4(current_view_h.xyz / current_view_h.w, 1.0f)).xyz;
		const float current_primary_distance = length(current_world_position - parameters.world_from_view[3].xyz);
		const float3 current_world_normal = normalize(normal_roughness_texture.read(pixel).xyz * 2.0f - 1.0f);
		const float4 prior_surface = reservoir_surface_input.read(previous_pixel);
		const uint2 prior_identity = reservoir_primary_identity_input.read(previous_pixel).xy;
		const float4 prior_shading = primary_shading_input.read(previous_pixel);
		const float3 current_diffuse = max(raster_material.rgb * (1.0f - raster_material.a), 0.0f);
		const float current_shading_roughness = clamp(roughness, 0.0f, 1.0f);
		const float prior_shading_roughness = clamp(prior_shading.w, 0.0f, 1.0f);
		const bool glossy = min(current_shading_roughness, prior_shading_roughness) < 0.20f;
		const float albedo_delta = max(max(abs(current_diffuse.x - prior_shading.x), abs(current_diffuse.y - prior_shading.y)), abs(current_diffuse.z - prior_shading.z));
		const bool sample_surface_compatible = sample_reprojection_valid && any(current_identity != uint2(0u)) && all(current_identity == prior_identity) && prior_surface.w != 0.0f &&
				abs(current_primary_distance - prior_surface.z) / max(current_primary_distance, 0.001f) < 0.02f &&
				dot(current_world_normal, oct_decode(prior_surface.xy)) > (glossy ? 0.95f : 0.8f) &&
				albedo_delta <= (glossy ? 0.06f : 0.14f) && abs(current_shading_roughness - prior_shading_roughness) <= (glossy ? 0.025f : 0.10f);
		const float4 diffuse_moments = diffuse_moments_input.read(pixel);
		const float4 specular_moments = specular_moments_input.read(pixel);
		const float4 prior_diffuse_transport = diffuse_transport_sample_input.read(previous_pixel);
		const float4 prior_specular_transport = specular_transport_sample_input.read(previous_pixel);
		const bool diffuse_converged = sample_surface_compatible && stationary && diffuse_moments.w > 0.5f && isfinite(diffuse_moments.z) && diffuse_moments.z <= 0.00075f && prior_diffuse_transport.a > 0.5f && all(isfinite(prior_diffuse_transport.rgb));
		// Reflections preserve a raw, exact secondary sample instead of any image
		// history.  A stable receiver may replay that immutable sample, but a
		// staggered refresh keeps dynamic lighting and very glossy transport from
		// becoming indefinitely stale.  Surface/reprojection compatibility above
		// forces an immediate ray on a cut, disocclusion, or material change.
		const uint reflection_refresh_period = glossy ? 4u : 8u;
		const uint reflection_refresh_phase = hash_u32(pixel.x + pixel.y * parameters.dimensions.x) % reflection_refresh_period;
		const bool reflection_refresh_due = parameters.frame_index % reflection_refresh_period == reflection_refresh_phase;
		const bool stable_specular_transport = specular_moments.w >= 0.75f && isfinite(specular_moments.z) && specular_moments.z <= 0.0015f && prior_specular_transport.a > 0.5f && all(isfinite(prior_specular_transport.rgb));
		const bool specular_converged = sample_surface_compatible && stationary && stable_specular_transport && !reflection_refresh_due;
		if ((uint(parameters.ao_distance_strength_roughness_flags.w) & 1u) != 0u && roughness <= parameters.ao_distance_strength_roughness_flags.z && any(f0 > float3(0.0001f))) {
			if (!sample_reprojection_valid) atomic_fetch_add_explicit(&material_diagnostic.reflection_validation_reprojection_rejections, 1u, memory_order_relaxed);
			else if (!sample_surface_compatible) atomic_fetch_add_explicit(&material_diagnostic.reflection_validation_surface_rejections, 1u, memory_order_relaxed);
			else if (!stationary) atomic_fetch_add_explicit(&material_diagnostic.reflection_validation_motion_rejections, 1u, memory_order_relaxed);
			if (reflection_refresh_due) atomic_fetch_add_explicit(&material_diagnostic.reflection_validation_refreshes, 1u, memory_order_relaxed);
		}
		if ((uint(parameters.ao_distance_strength_roughness_flags.w) & 4u) != 0u && any(diffuse > float3(0.0001f))) {
			if (diffuse_converged && (surface_flags & (1u << 7u)) == 0u) {
				atomic_fetch_add_explicit(&material_diagnostic.gi_converged_skips, 1u, memory_order_relaxed);
			} else {
				need_mask |= TRACE_NEED_GI;
			}
		}
		if ((uint(parameters.ao_distance_strength_roughness_flags.w) & 1u) != 0u && roughness <= parameters.ao_distance_strength_roughness_flags.z && any(f0 > float3(0.0001f))) {
			if (specular_converged && (surface_flags & (1u << 7u)) == 0u) {
				atomic_fetch_add_explicit(&material_diagnostic.reflection_converged_skips, 1u, memory_order_relaxed);
			} else {
				need_mask |= TRACE_NEED_REFLECTION;
			}
		}
		if ((surface_flags & (1u << 7u)) != 0u) need_mask |= TRACE_NEED_EXACT_ALPHA;
		if ((parameters.bidirectional_caustic_flags & 1u) != 0u && parameters.bidirectional_caustic_mirror_count > 0u && parameters.bidirectional_caustic_source_count > 0u && any(diffuse > float3(0.0001f))) need_mask |= TRACE_NEED_COMPLEX;
	} else {
		// Without PrimarySurfaceV1, preserve conservative support for every
		// globally enabled optional estimator. The trace kernel still owns exact
		// primary validation and alpha intersection semantics.
		if ((uint(parameters.ao_distance_strength_roughness_flags.w) & 4u) != 0u) need_mask |= TRACE_NEED_GI;
		if ((uint(parameters.ao_distance_strength_roughness_flags.w) & 1u) != 0u) need_mask |= TRACE_NEED_REFLECTION;
		if ((parameters.bidirectional_caustic_flags & 1u) != 0u && parameters.bidirectional_caustic_mirror_count > 0u && parameters.bidirectional_caustic_source_count > 0u) need_mask |= TRACE_NEED_COMPLEX;
	}

	if ((need_mask & TRACE_NEED_DIRECT) != 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_need_direct, 1u, memory_order_relaxed);
	atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_active_pixels, 1u, memory_order_relaxed);
	if ((need_mask & TRACE_NEED_GI) != 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_need_gi, 1u, memory_order_relaxed);
	if ((need_mask & TRACE_NEED_REFLECTION) != 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_need_reflection, 1u, memory_order_relaxed);
	if ((need_mask & TRACE_NEED_EXACT_ALPHA) != 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_need_exact_alpha, 1u, memory_order_relaxed);
	if ((need_mask & TRACE_NEED_COMPLEX) != 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_need_complex, 1u, memory_order_relaxed);
	const uint queue_index = (need_mask & TRACE_NEED_COMPLEX) != 0u ? 4u : (need_mask & TRACE_NEED_EXACT_ALPHA) != 0u ? 3u : (need_mask & TRACE_NEED_REFLECTION) != 0u ? 2u : (need_mask & TRACE_NEED_GI) != 0u ? 1u : 0u;
	const uint slot = atomic_fetch_add_explicit(&trace_queue_counts[queue_index], 1u, memory_order_relaxed);
	if (slot >= parameters.dimensions.x * parameters.dimensions.y) return;
	trace_queues[queue_index * (parameters.dimensions.x * parameters.dimensions.y) + slot] = { pixel.y * parameters.dimensions.x + pixel.x, need_mask };
	if (queue_index == 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_enqueued_direct, 1u, memory_order_relaxed);
	else if (queue_index == 1u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_enqueued_gi, 1u, memory_order_relaxed);
	else if (queue_index == 2u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_enqueued_reflection, 1u, memory_order_relaxed);
	else if (queue_index == 3u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_enqueued_exact_alpha, 1u, memory_order_relaxed);
	else atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_enqueued_complex, 1u, memory_order_relaxed);
}

kernel void trace_indirect_finalize(
		device atomic_uint *trace_queue_counts [[buffer(0)]],
		device TraceIndirectArguments *trace_indirect_arguments [[buffer(1)]],
		device MaterialDiagnosticAtomic &material_diagnostic [[buffer(2)]],
		uint queue_index [[thread_position_in_grid]]) {
	if (queue_index >= 5u) return;
	const uint count = atomic_load_explicit(&trace_queue_counts[queue_index], memory_order_relaxed);
	trace_indirect_arguments[queue_index].threadgroupsPerGrid[0] = (count + 63u) / 64u;
	trace_indirect_arguments[queue_index].threadgroupsPerGrid[1] = 1u;
	trace_indirect_arguments[queue_index].threadgroupsPerGrid[2] = 1u;
	if (queue_index == 0u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_direct, count, memory_order_relaxed);
	else if (queue_index == 1u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_gi, count, memory_order_relaxed);
	else if (queue_index == 2u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_reflection, count, memory_order_relaxed);
	else if (queue_index == 3u) atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_exact_alpha, count, memory_order_relaxed);
	else atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_complex, count, memory_order_relaxed);
}

kernel void trace_hybrid(
    raytracing::instance_acceleration_structure scene [[buffer(0)]],
    constant Parameters &parameters [[buffer(1)]],
    constant MaterialRecord *materials [[buffer(2)]],
    constant GeometryRecord *geometry_records [[buffer(3)]],
	constant EmissiveRecord *emissives [[buffer(4)]],
	constant PunctualLightRecord *punctual_lights [[buffer(5)]],
	constant PortalRecord *portals [[buffer(6)]],
	constant EmissiveTriangleRecord *emissive_triangles [[buffer(7)]],
#if HYBRID_BINDLESS_MATERIALS
	constant MaterialTextureTable &material_textures [[buffer(8)]],
#endif
	device MaterialDiagnosticAtomic &material_diagnostic [[buffer(9)]],
	raytracing::intersection_function_table<raytracing::instancing, raytracing::triangle_data> alpha_intersection_table [[buffer(10)]],
	device const RegirCell *regir_cells [[buffer(11)]],
	constant RegirHeader &regir_header [[buffer(12)]],
	device const ReusablePathSampleRecord *reusable_path_previous [[buffer(13)]],
	constant ReusablePathHeader &reusable_path_header [[buffer(14)]],
	device ReusablePathSampleRecord *reusable_path_staging [[buffer(15)]],
	constant BidirectionalCausticMirrorTriangleRecord *bidirectional_caustic_mirrors [[buffer(16)]],
	raytracing::instance_acceleration_structure opaque_scene [[buffer(17)]],
	raytracing::instance_acceleration_structure alpha_scene [[buffer(22)]],
    depth2d<float, access::read> depth_texture [[texture(0)]],
    texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
    texture2d<float, access::read> color_texture [[texture(2)]],
    texture2d<float, access::write> effect_texture [[texture(3)]],
	texture2d<float, access::write> guide_normal [[texture(4)]],
	texture2d<float, access::write> guide_diffuse [[texture(5)]],
	texture2d<float, access::write> guide_specular [[texture(6)]],
	texture2d<float, access::write> guide_roughness [[texture(7)]],
	texture2d<float, access::write> guide_denoise_strength [[texture(8)]],
	texture2d<float, access::write> guide_reactive [[texture(9)]],
	texture2d<float, access::write> guide_specular_distance [[texture(10)]],
	texture2d<float, access::write> guide_transparency [[texture(11)]],
#if !HYBRID_BINDLESS_MATERIALS
	array<texture2d<float, access::sample>, HYBRID_FALLBACK_MATERIAL_TEXTURES> material_textures [[texture(12)]],
#endif
	texture2d<float, access::sample> environment_radiance [[texture(28)]],
	texture2d<float, access::read> environment_importance [[texture(29)]],
	texture2d<float, access::sample> full_environment_radiance [[texture(30)]],
	texture2d<uint, access::read> reservoir_input [[texture(31)]],
	texture2d<uint, access::write> reservoir_output [[texture(32)]],
	texture2d<float, access::read> reservoir_surface_input [[texture(33)]],
	texture2d<float, access::write> reservoir_surface_output [[texture(34)]],
	texture2d<float, access::write> split_diffuse [[texture(36)]],
	texture2d<float, access::write> split_specular [[texture(37)]],
	texture2d<float, access::read> diffuse_history_input [[texture(38)]],
	texture2d<float, access::write> diffuse_history_output [[texture(39)]],
	texture2d<float, access::read> specular_history_input [[texture(40)]],
	texture2d<float, access::write> specular_history_output [[texture(41)]],
	texture2d<float, access::read> diffuse_moments_input [[texture(42)]],
	texture2d<float, access::write> diffuse_moments_output [[texture(43)]],
	texture2d<float, access::read> specular_moments_input [[texture(44)]],
	texture2d<float, access::write> specular_moments_output [[texture(45)]],
	texture2d<float, access::read> reservoir_metadata_input [[texture(46)]],
	texture2d<float, access::write> reservoir_metadata_output [[texture(47)]],
	texture2d<float, access::read> reservoir_sample_input [[texture(48)]],
	texture2d<float, access::write> reservoir_sample_output [[texture(49)]],
	texture2d<float, access::read> primary_material_texture [[texture(50)]],
	texture2d<uint, access::read> primary_identity_texture [[texture(51)]],
	texture2d<uint, access::read> reservoir_primary_identity_input [[texture(52)]],
	texture2d<uint, access::write> reservoir_primary_identity_output [[texture(53)]],
	texture2d<float, access::read> velocity_texture [[texture(54)]],
	texture2d<float, access::read> primary_geometry_texture [[texture(55)]],
	texture2d<uint, access::read> primary_flags_texture [[texture(56)]],
	texture2d_array<ushort, access::read> stbn_scalar_volume [[texture(57)]],
	texture2d<float, access::write> primary_world_position_output [[texture(58)]],
	texture2d<float, access::write> primary_shading_output [[texture(59)]],
	texture2d<float, access::read> diffuse_transport_sample_input [[texture(60)]],
	texture2d<float, access::read> specular_transport_sample_input [[texture(61)]],
	texture2d<float, access::write> diffuse_transport_sample_output [[texture(62)]],
	texture2d<float, access::write> specular_transport_sample_output [[texture(63)]],
	texture2d<float, access::read> primary_shading_input [[texture(66)]],
	sampler material_sampler [[sampler(0)]],
	sampler environment_sampler [[sampler(1)]],
	device const TraceCompactEntry *trace_queues [[buffer(18)]],
	constant TraceCompactContext &trace_context [[buffer(19)]],
	device const atomic_uint *trace_queue_counts [[buffer(20)]],
	device atomic_uint *reusable_path_staging_claims [[buffer(21)]],
	    uint3 thread_position_in_grid [[thread_position_in_grid]]) {
	uint2 pixel = uint2(thread_position_in_grid.xy);
	uint need_mask = TRACE_NEED_DIRECT | TRACE_NEED_GI | TRACE_NEED_REFLECTION | TRACE_NEED_EXACT_ALPHA | TRACE_NEED_COMPLEX;
	if (trace_context.enabled != 0u) {
		if (trace_context.queue_index >= 5u || trace_context.queue_capacity == 0u) return;
		const uint compact_index = thread_position_in_grid.x;
		const uint logical_count = atomic_load_explicit(&trace_queue_counts[trace_context.queue_index], memory_order_relaxed);
		if (compact_index >= logical_count) return;
		const uint entry_index = trace_context.queue_index * trace_context.queue_capacity + compact_index;
		const TraceCompactEntry entry = trace_queues[entry_index];
		const uint pixel_count = parameters.dimensions.x * parameters.dimensions.y;
		if (entry.pixel_index >= pixel_count) return;
		pixel = uint2(entry.pixel_index % parameters.dimensions.x, entry.pixel_index / parameters.dimensions.x);
		need_mask = entry.need_mask;
	} else if (any(pixel >= parameters.dimensions)) {
		return;
	}
    float depth = depth_texture.read(pixel);
    if (depth <= 0.0f) {
        effect_texture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
		guide_normal.write(float4(0.0f), pixel);
		guide_diffuse.write(float4(0.0f), pixel);
		guide_specular.write(float4(0.0f), pixel);
		guide_roughness.write(float4(1.0f), pixel);
		guide_denoise_strength.write(float4(1.0f), pixel);
		guide_reactive.write(float4(0.0f), pixel);
		guide_specular_distance.write(float4(0.0f), pixel);
		guide_transparency.write(float4(0.0f), pixel);
		reservoir_output.write(uint4(0u), pixel);
		reservoir_surface_output.write(float4(0.0f), pixel);
		reservoir_metadata_output.write(float4(0.0f), pixel);
		reservoir_sample_output.write(float4(0.0f), pixel);
		reservoir_primary_identity_output.write(uint4(0u), pixel);
		primary_shading_output.write(float4(0.0f), pixel);
		primary_world_position_output.write(float4(0.0f), pixel);
		split_diffuse.write(float4(0.0f), pixel);
		split_specular.write(float4(0.0f), pixel);
		diffuse_history_output.write(float4(0.0f), pixel);
		specular_history_output.write(float4(0.0f), pixel);
		diffuse_moments_output.write(float4(0.0f), pixel);
		specular_moments_output.write(float4(0.0f), pixel);
        return;
    }
    float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
    float2 ndc = uv * 2.0f - 1.0f;
    float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
    float3 view_position = view_h.xyz / view_h.w;
    float3 world_position = (parameters.world_from_view * float4(view_position, 1.0f)).xyz;
    float4 normal_roughness = normal_roughness_texture.read(pixel);
    float3 view_normal = normalize(normal_roughness.xyz * 2.0f - 1.0f);
    float3 world_normal = normalize((parameters.world_from_view * float4(view_normal, 0.0f)).xyz);
	float3 geometric_normal = world_normal;
    float roughness = normal_roughness.w;
    roughness = roughness > 0.5f ? (1.0f - roughness) * (255.0f / 127.0f) : roughness * (255.0f / 127.0f);
    float3 view_direction = normalize(world_position - parameters.world_from_view[3].xyz);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
	// The first-frame hybrid path is intentionally screen-space deterministic.
	// A changing hash here makes one-sample area-light and GGX estimates form
	// large travelling lobes that a temporal scaler cannot classify as motion.
	// Per-pixel scrambling still decorrelates neighboring rays; camera motion is
	// handled by the primary depth/motion guides rather than a random frame seed.
	uint state = hash_u32(pixel.x + pixel.y * parameters.dimensions.x);
	float3 primary_diffuse = 1.0f;
	float3 primary_f0 = 0.04f;
	float3 primary_emission = 0.0f;
	float primary_ambient_occlusion = 1.0f;
	uint primary_instance_id = 0xffffffffu;
	uint2 primary_surface_identity = uint2(0u);
	uint primary_receiver_mask = 0xffffffffu;
	float3 camera_position = parameters.world_from_view[3].xyz;
	float3 primary_direction = world_position - camera_position;
	float primary_distance = length(primary_direction);
	if (parameters.raster_primary_surface != 0u) {
		const uint surface_flags = primary_flags_texture.read(pixel).x;
		const bool primary_valid = (surface_flags & 0x0fu) == 1u && (surface_flags & (1u << 4u)) != 0u && isfinite(depth);
		const float4 raster_material = primary_material_texture.read(pixel);
		const float4 raster_emission_ao = color_texture.read(pixel);
		const float4 raster_geometry = primary_geometry_texture.read(pixel);
		if (!primary_valid || !all(isfinite(raster_material)) || !all(isfinite(raster_emission_ao)) || !all(isfinite(raster_geometry)) || dot(raster_geometry.xyz, raster_geometry.xyz) < 0.25f || dot(world_normal, world_normal) < 0.25f) {
			atomic_fetch_add_explicit(&material_diagnostic.primary_invalid_pixels, 1u, memory_order_relaxed);
			effect_texture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixel);
			primary_shading_output.write(float4(0.0f), pixel);
			primary_world_position_output.write(float4(0.0f), pixel);
			return;
		}
		atomic_fetch_add_explicit(&material_diagnostic.primary_valid_pixels, 1u, memory_order_relaxed);
		world_normal = normalize(normal_roughness.xyz * 2.0f - 1.0f);
		geometric_normal = normalize(raster_geometry.xyz);
		primary_diffuse = raster_material.rgb * (1.0f - raster_material.a);
		primary_f0 = mix(float3(0.08f * clamp(raster_geometry.a, 0.0f, 1.0f)), raster_material.rgb, raster_material.a);
		primary_emission = raster_emission_ao.rgb;
		primary_ambient_occlusion = clamp(raster_emission_ao.a, 0.0f, 1.0f);
		primary_receiver_mask = surface_flags >> 12u;
		const uint4 raster_identity = primary_identity_texture.read(pixel);
		primary_surface_identity = uint2(raster_identity.x | (raster_identity.y << 16u), raster_identity.z | (raster_identity.w << 16u));
	} else if (primary_distance > 0.001f) {
		// This validates the camera/raster direction and material identity; it is
		// not a secondary transport segment. Measured raster-depth reconstruction
		// can substantially underreach the matching Metal AS hit, so D must not
		// cap this ray or it can select no primary material at all.
		raytracing::ray primary_ray = { camera_position, primary_direction / primary_distance, 0.001f, 100000.0f };
		const float primary_ray_spread = max(1.0f / float(max(parameters.dimensions.x, parameters.dimensions.y)), 0.00025f);
		auto primary_hit = hybrid_intersect_split(primary_ray, scene, opaque_scene, alpha_scene, 0xff, primary_ray_spread, ALPHA_RAY_CLASS_PRIMARY, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
		if (primary_hit.type == raytracing::intersection_type::triangle) {
			primary_instance_id = primary_hit.instance_id;
			primary_surface_identity = uint2(geometry_records[primary_hit.instance_id].instance_identity_low, geometry_records[primary_hit.instance_id].instance_identity_high);
			MaterialRecord primary_material = materials[primary_hit.instance_id];
			primary_receiver_mask = primary_material.visibility_mask;
			const MaterialSample primary_sample = sample_material(primary_material, geometry_records, primary_hit.instance_id, primary_hit.primitive_id, primary_hit.triangle_barycentric_coord, primary_hit.distance, primary_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
			primary_diffuse = primary_sample.albedo * (1.0f - primary_sample.metallic);
				// Keep authored StandardMaterial3D specular in the primary
				// dielectric F0. Secondary hits already use this same 0.08*specular
				// mapping; the old constant here made primary panes matte even when
				// their authored specular was raised.
				primary_f0 = mix(float3(0.08f * clamp(primary_material.specular, 0.0f, 1.0f)), primary_sample.albedo, primary_sample.metallic);
			primary_emission = primary_sample.emission;
			primary_ambient_occlusion = primary_sample.ambient_occlusion;
			world_position = primary_ray.origin + primary_ray.direction * primary_hit.distance;
			world_normal = intersection_normal(geometry_records, primary_hit.instance_id, primary_hit.primitive_id, primary_hit.triangle_barycentric_coord, -primary_ray.direction);
			if (dot(world_normal, -primary_ray.direction) < 0.0f) world_normal = -world_normal;
			world_normal = material_shading_normal(primary_material, geometry_records, primary_hit.instance_id, primary_hit.primitive_id, primary_hit.triangle_barycentric_coord, world_normal, primary_hit.distance, primary_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
			roughness = primary_sample.roughness;
		}
	}
	uint flags = uint(parameters.ao_distance_strength_roughness_flags.w);
	const bool fresh_ray_oracle = (parameters.experimental_feature_flags & 0x80000000u) != 0u;
	const float4 old_diffuse_moments = diffuse_moments_input.read(pixel);
	const float old_variance = max(old_diffuse_moments.y - old_diffuse_moments.x * old_diffuse_moments.x, 0.0f);
	const uint adaptive_direct_samples = clamp(parameters.adaptive_min_samples + uint(old_variance > parameters.adaptive_variance_reference), parameters.adaptive_min_samples, max(parameters.adaptive_max_samples, parameters.adaptive_min_samples));
	DirectLightReservoir reservoir = { 0u, 0u, uint2(0u), 0xffffffffu, float2(0.0f), 0.0f, 0.0f, 0.0f, 0u, 0u, false };
	const bool have_emissive = parameters.emissive_count > 0u && parameters.emissive_triangle_count > 0u;
	const bool have_punctual = parameters.punctual_light_count > 0u;
	// Restore the c2e89bba6c emissive DI estimator: one bounded current reservoir
	// followed by one temporal and one spatial reuse candidate. Metal owns every
	// cast-shadow visibility query; Flux raster supplies only the primary surface.
	if (!fresh_ray_oracle && have_emissive) {
		for (uint candidate_index = 0u; candidate_index < adaptive_direct_samples; candidate_index++) {
			const float select_u = hammersley_dimension(parameters.frame_index, candidate_index, adaptive_direct_samples, state, FLUX_SAMPLE_DIMENSION_DIRECT_EMISSIVE_SELECT, parameters, stbn_scalar_volume, pixel);
			uint low = 0u, high = parameters.emissive_triangle_count - 1u;
			while (low < high) { const uint middle = low + (high - low) / 2u; if (select_u <= emissive_triangles[middle].cdf) high = middle; else low = middle + 1u; }
			const EmissiveTriangleRecord selected = emissive_triangles[low];
			if (!(selected.weight > 0.0f) || !(selected.area > 0.0f) || !(selected.selection_pdf > 0.0f)) continue;
			const float sqrt_x = sqrt(hammersley_dimension(parameters.frame_index, candidate_index, adaptive_direct_samples, state, FLUX_SAMPLE_DIMENSION_DIRECT_BARYCENTRIC_U, parameters, stbn_scalar_volume, pixel));
			const float bary_y = hammersley_dimension(parameters.frame_index, candidate_index, adaptive_direct_samples, state, FLUX_SAMPLE_DIMENSION_DIRECT_BARYCENTRIC_V, parameters, stbn_scalar_volume, pixel);
			DirectLightReservoir candidate = { 0u, selected.primitive_id, uint2(selected.instance_identity_low, selected.instance_identity_high), low, float2(sqrt_x * (1.0f - bary_y), sqrt_x * bary_y), selected.selection_pdf / max(selected.area, 0.000001f), 0.0f, 0.0f, 1u, 0u, true };
			const float3 value = evaluate_reservoir_source(candidate, world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, primary_receiver_mask, true, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, punctual_lights, parameters.punctual_light_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
			reservoir_merge(reservoir, candidate, value, hammersley_dimension(parameters.frame_index, candidate_index, adaptive_direct_samples, state, FLUX_SAMPLE_DIMENSION_DIRECT_RESERVOIR, parameters, stbn_scalar_volume, pixel), false, material_diagnostic);
		}
	}
	// Finite analytic sources share the same reservoir contract as emissive
	// triangles. Sample a true local/global mixture: the local proposal improves
	// city-light selection while the uniform branch preserves support for every
	// currently admitted light. The stored full stable id survives table reorder;
	// a selected area point is reconstructed from its persistent seed above.
	if (!fresh_ray_oracle && (parameters.experimental_feature_flags & 8u) != 0u && have_punctual) {
		const float technique = hammersley_dimension(parameters.frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_DIRECT_RESERVOIR + 2u, parameters, stbn_scalar_volume, pixel);
		const float select_random = hammersley_dimension(parameters.frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_DIRECT_RESERVOIR + 3u, parameters, stbn_scalar_volume, pixel);
		const uint global_index = min(uint(select_random * float(parameters.punctual_light_count)), parameters.punctual_light_count - 1u);
		const uint local_index = regir_sample_local(world_position, camera_position, select_random, punctual_lights, parameters.punctual_light_count);
		const uint selected_index = technique < 0.5f ? local_index : global_index;
		const PunctualLightRecord selected = punctual_lights[selected_index];
		const float local_pdf = regir_local_pdf(world_position, camera_position, selected_index, punctual_lights, parameters.punctual_light_count);
		float proposal_pdf = 0.5f * local_pdf + 0.5f / float(parameters.punctual_light_count);
		if (selected.type == 2u) {
			const float area = 4.0f * length(cross(selected.area_u_spot_attenuation.xyz, selected.area_v.xyz));
			if (!(area > 0.000001f) || !isfinite(area)) proposal_pdf = 0.0f;
			else proposal_pdf /= area;
		}
		const uint sample_seed = hash_u32(state ^ selected.stable_identity.x ^ selected.stable_identity.y ^ parameters.frame_index);
		DirectLightReservoir candidate = { 1u, selected_index, selected.stable_identity, sample_seed, float2(0.0f), proposal_pdf, 0.0f, 0.0f, 1u, 0u, proposal_pdf > 0.0f && isfinite(proposal_pdf) };
		const float3 value = evaluate_reservoir_source(candidate, world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, primary_receiver_mask, true, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, punctual_lights, parameters.punctual_light_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
		reservoir_merge(reservoir, candidate, value, hammersley_dimension(parameters.frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_DIRECT_REUSE + 1u, parameters, stbn_scalar_volume, pixel), false, material_diagnostic);
	}
	float4 previous_clip = parameters.prev_clip_from_world * float4(world_position, 1.0f);
	uint2 previous_pixel = pixel;
	if (previous_clip.w > 0.0f) { const float2 previous_uv = float2(previous_clip.x / previous_clip.w * 0.5f + 0.5f, 0.5f - previous_clip.y / previous_clip.w * 0.5f); if (all(previous_uv >= 0.0f) && all(previous_uv < 1.0f)) previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u); }
	const uint2 spatial_pixel = min(previous_pixel + uint2(1u, hash_u32(pixel.x ^ pixel.y) & 1u), parameters.dimensions - 1u);
	if (!fresh_ray_oracle && (parameters.experimental_feature_flags & 1u) != 0u) for (uint reuse = 0u; reuse < 2u; reuse++) {
		const uint2 source_pixel = reuse == 0u ? previous_pixel : spatial_pixel;
		const float4 source_surface = reservoir_surface_input.read(source_pixel);
		const uint4 source_data = reservoir_input.read(source_pixel);
		const float4 source_sample = reservoir_sample_input.read(source_pixel);
		const float4 source_meta = reservoir_metadata_input.read(source_pixel);
		const uint2 source_primary_identity = reservoir_primary_identity_input.read(source_pixel).xy;
		const float normal_threshold = reuse == 0u ? 0.8f : 0.9f;
		const bool valid = parameters.history_valid != 0u && (reuse != 0u || previous_clip.w > 0.0f) && source_meta.w > 0.5f && any(source_primary_identity != uint2(0u)) && all(source_primary_identity == primary_surface_identity) && abs(source_surface.z - primary_distance) / max(primary_distance, 0.001f) < 0.02f && dot(oct_decode(source_surface.xy), world_normal) > normal_threshold;
		if (!valid) continue;
		if (reuse == 0u) atomic_fetch_add_explicit(&material_diagnostic.direct_temporal_reuse, 1u, memory_order_relaxed);
		else atomic_fetch_add_explicit(&material_diagnostic.direct_spatial_reuse, 1u, memory_order_relaxed);
		DirectLightReservoir candidate = { source_data.x, source_data.y, source_data.zw, uint(source_sample.x), source_sample.yz, source_sample.w, source_meta.x, source_meta.y, uint(source_meta.z), uint(source_meta.w) - 1u, true };
		const float3 value = evaluate_reservoir_source(candidate, world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, primary_receiver_mask, true, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, punctual_lights, parameters.punctual_light_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
		reservoir_merge(reservoir, candidate, value, hammersley_dimension(parameters.frame_index, reuse, 2u, state, FLUX_SAMPLE_DIMENSION_DIRECT_REUSE, parameters, stbn_scalar_volume, pixel), true, material_diagnostic);
	}
	// World-space ReGIR is a third proposal only. The cell is keyed from the
	// exact ray primary position, checks the full scene revision tuple, remaps
	// its stable finite-light source, and runs current receiver visibility via
	// evaluate_reservoir_source. A failed query simply leaves fresh DI intact.
	if (!fresh_ray_oracle && (parameters.experimental_feature_flags & 2u) != 0u && regir_header.previous_center_and_flags.w != 0 && all(isfinite(world_position))) {
		const float cell_size = max(float(regir_header.center_and_cell_size.w), 0.25f);
		uint cell_index = 0u;
		if (regir_index_for_key(int3(floor(world_position / cell_size)), regir_header, cell_index)) {
			const RegirCell cell = regir_cells[cell_index];
			if (cell.key_valid.w != 0 && all(cell.key_valid.xyz == int3(floor(world_position / cell_size))) && all(cell.revisions == regir_header.revisions) && cell.metadata.x > 0.0f && cell.metadata.y > 0.0f && cell.sample.w > 0.0f && all(isfinite(cell.metadata)) && all(isfinite(cell.sample))) {
				DirectLightReservoir candidate = { cell.source.x, cell.source.y, cell.source.zw, uint(cell.sample.x), cell.sample.yz, cell.sample.w, cell.metadata.x, cell.metadata.y, uint(cell.metadata.z), uint(cell.metadata.w) - 1u, true };
				const float3 value = evaluate_reservoir_source(candidate, world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, primary_receiver_mask, true, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, punctual_lights, parameters.punctual_light_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
				reservoir_merge(reservoir, candidate, value, hammersley_dimension(parameters.frame_index, 0u, 1u, state, FLUX_SAMPLE_DIMENSION_DIRECT_REUSE + 2u, parameters, stbn_scalar_volume, pixel), true, material_diagnostic);
			}
		}
	}
	// Reservoir records are proposals only. Every selected proposal is evaluated
	// at the current receiver with a current destination-visibility ray; no
	// cached direct-visibility radiance is ever replayed as final lighting.
	const float3 selected_direct = reservoir.valid && reservoir.target > 0.0f ? evaluate_reservoir_source(reservoir, world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, primary_receiver_mask, true, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, punctual_lights, parameters.punctual_light_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) : float3(0.0f);
	const float3 fresh_emissive_direct = fresh_ray_oracle && have_emissive ? sample_emissive_lighting(world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, adaptive_direct_samples, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) : float3(0.0f);
	const float3 emissive_direct = fresh_ray_oracle ? fresh_emissive_direct : (reservoir.valid && reservoir.target > 0.0f ? selected_direct * reservoir.weight_sum / max(float(reservoir.candidate_count) * reservoir.target, 0.000001f) : 0.0f);
	const float3 bidirectional_caustic_direct = (need_mask & TRACE_NEED_COMPLEX) != 0u ? sample_bidirectional_planar_caustic(world_position, world_normal, primary_diffuse, primary_receiver_mask, parameters.frame_index, state, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, bidirectional_caustic_mirrors, parameters.bidirectional_caustic_mirror_count, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) : 0.0f;
	// When unified finite-light reservoirs are active, positive punctual lights
	// are already represented by the selected reservoir source. Keep the exact
	// loop only for authored negative lights, which cannot form a positive RIS
	// target. This removes the previous duplicate direct-light evaluation.
	const uint analytic_sign_filter = !fresh_ray_oracle && (parameters.experimental_feature_flags & 8u) != 0u ? 2u : 0u;
	const float3 analytic_direct = have_punctual ? sample_punctual_lighting(world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, primary_receiver_mask, analytic_sign_filter, false, true, punctual_lights, parameters.punctual_light_count, parameters.frame_index, state, parameters.transport_max_distance, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) : float3(0.0f);
	// Environment NEE uses the reserved Hammersley dimensions 8..10. This is
	// separate from emissive 0..3, GI BRDF 4..5, and GGX 6..7. When GI is
	// active, its cosine-weighted miss estimator samples the same direct Sky
	// path, so the two estimators use complementary balance weights. With GI
	// disabled there is no paired BSDF proposal and NEE retains its full weight.
	const bool primary_environment_bsdf_proposal = (flags & 4u) != 0u;
	float3 environment_direct = parameters.environment_info.w > 2.5f ? 0.0f : sample_environment_portal_mixture(world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, parameters.frame_index, state, FLUX_SAMPLE_DIMENSION_PRIMARY_ENVIRONMENT, primary_environment_bsdf_proposal, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, portals, environment_radiance, environment_importance, environment_sampler, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
	float3 reflection = 0.0f;
	float reflection_weight = 0.0f;
	float specular_hit_distance = 0.0f;
	// MetalFX's geometry/material guides describe the stable primary surface.
	// Specular hit distance carries the secondary-hit distance without making
	// the guide set flicker as stochastic reflection rays select different hits.
	float3 reflection_guide_normal = world_normal;
	float3 reflection_guide_diffuse = primary_diffuse;
	float3 reflection_guide_f0 = primary_f0;
	float reflection_guide_roughness = roughness;
	float reflection_guide_cosine = max(dot(-view_direction, world_normal), 0.0f);
	    if ((need_mask & TRACE_NEED_REFLECTION) != 0u && (flags & 1u) != 0u && roughness <= parameters.ao_distance_strength_roughness_flags.z) {
		uint reflection_state = state;
		float3 reflected = sample_ggx_reflection(view_direction, world_normal, roughness, parameters.frame_index, reflection_state, parameters, stbn_scalar_volume, pixel);
		raytracing::ray ray = { world_position + geometric_normal * (dot(reflected, geometric_normal) >= 0.0f ? 0.002f : -0.002f), reflected, 0.001f, parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 10000.0f };
		atomic_fetch_add_explicit(&material_diagnostic.reflection_rays, 1u, memory_order_relaxed);
		const float reflected_ray_spread = max(1.0f / float(max(parameters.dimensions.x, parameters.dimensions.y)) + roughness * roughness * 0.25f, 0.00025f);
		float3 reflection_transmission = float3(1.0f);
		auto hit = hybrid_intersect_thin_reconnect(ray, scene, opaque_scene, alpha_scene, 0xff, reflected_ray_spread, ALPHA_RAY_CLASS_REFLECTION, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, reflection_transmission);
		if (hit.type == raytracing::intersection_type::triangle) {
			specular_hit_distance = hit.distance;
			MaterialRecord material = materials[hit.instance_id];
			float3 hit_normal = intersection_normal(geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, -ray.direction);
			if (dot(hit_normal, -ray.direction) < 0.0f) hit_normal = -hit_normal;
			float3 hit_position = ray.origin + ray.direction * hit.distance;
			const MaterialSample hit_sample = sample_material(material, geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, primary_distance + hit.distance, reflected_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
			hit_normal = material_shading_normal(material, geometry_records, hit.instance_id, hit.primitive_id, hit.triangle_barycentric_coord, hit_normal, primary_distance + hit.distance, reflected_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
			float3 hit_diffuse = hit_sample.albedo * (1.0f - hit_sample.metallic);
			float3 hit_f0 = mix(float3(0.08f * clamp(material.specular, 0.0f, 1.0f)), hit_sample.albedo, hit_sample.metallic);
			const float emitter_light_pdf = emissive_solid_angle_pdf_for_hit(hit.instance_id, hit.primitive_id, hit.distance * hit.distance, ray.direction, geometry_records, emissive_triangles, parameters.emissive_triangle_count);
			const float reflection_pdf = ggx_vndf_reflection_pdf(view_direction, world_normal, ray.direction, roughness);
			const float emitter_bsdf_weight = emitter_light_pdf > 0.0f ? power_heuristic(reflection_pdf, emitter_light_pdf) : 1.0f;
			reflection = hit_sample.emission * emitter_bsdf_weight + sample_emissive_lighting(hit_position, hit_normal, hit_diffuse, ray.direction, hit_sample.roughness, hit_f0, 1u, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, parameters.frame_index, reflection_state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
			reflection += sample_punctual_lighting(hit_position, hit_normal, hit_diffuse, ray.direction, hit_sample.roughness, hit_f0, material.visibility_mask, 0u, true, true, punctual_lights, parameters.punctual_light_count, parameters.frame_index, reflection_state, parameters.transport_max_distance, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
			reflection += sample_solar_lobe_lighting(hit_position, hit_normal, hit_diffuse, ray.direction, hit_sample.roughness, hit_f0, parameters.frame_index, reflection_state, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
			// Reflection-hit environment NEE owns dimensions 14..16; primary uses
			// 8..10 and diffuse-secondary transport uses 11..13.
			reflection += sample_environment_lighting(hit_position, hit_normal, hit_diffuse, ray.direction, hit_sample.roughness, hit_f0, parameters.frame_index, reflection_state, FLUX_SAMPLE_DIMENSION_REFLECTION_ENVIRONMENT, false, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, environment_radiance, environment_importance, environment_sampler, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
		} else if (parameters.environment_info.w <= 2.5f) {
			atomic_fetch_add_explicit(&material_diagnostic.reflection_environment_misses, 1u, memory_order_relaxed);
			const bool use_full_delta_miss = parameters.solar_perpendicular_irradiance_enabled.w > 0.5f && roughness <= 0.001f;
			if (use_full_delta_miss) {
				reflection = environment_lookup(reflected, parameters, full_environment_radiance, environment_sampler);
			} else {
				reflection = environment_lookup(reflected, parameters, environment_radiance, environment_sampler);
			}
			if (any(reflection > float3(0.0001f))) {
				atomic_fetch_add_explicit(&material_diagnostic.reflection_environment_contributions, 1u, memory_order_relaxed);
			}
		}
		reflection *= reflection_transmission * ggx_reflection_throughput(view_direction, world_normal, reflected, roughness, primary_f0);
		reflection_weight = parameters.light_direction_and_reflection_strength.w;
    }
	float3 environment_bsdf_direct = 0.0f;
	float3 indirect = 0.0f;
	float3 restir_gi_indirect = 0.0f;
	float3 restir_gi_fresh_incoming = 0.0f;
	float restir_gi_fresh_pdf = 0.0f;
	bool restir_gi_fresh_valid = false;
	if ((need_mask & TRACE_NEED_GI) != 0u && (flags & 4u) != 0u) {
		float3 helper = abs(world_normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
		float3 tangent = normalize(cross(helper, world_normal));
		float3 bitangent = cross(world_normal, tangent);
		// Reusable-path records are not a final-radiance cache. Their endpoint/PDF
		// contract is incomplete, so this backend keeps the entire consume/update
		// path fail-closed and always retains fresh GI samples.
		const bool reusable_path_transport_enabled = false;
		if (reusable_path_transport_enabled && (parameters.experimental_feature_flags & 4u) != 0u && reusable_path_header.previous_center_and_flags.w != 0u && any(primary_surface_identity != uint2(0u))) {
			const float path_cell_size = max(float(reusable_path_header.center_and_cell_size.w), 0.25f);
			uint candidate_index = 0u;
			if (reusable_path_index_for_key(int3(floor(world_position / path_cell_size)), reusable_path_header, candidate_index)) {
				atomic_fetch_add_explicit(&material_diagnostic.reusable_path_queries, 1u, memory_order_relaxed);
				const ReusablePathSampleRecord candidate = reusable_path_previous[candidate_index];
				const ulong geometry_revision = (ulong(parameters.admitted_geometry_generation.y) << 32u) | ulong(parameters.admitted_geometry_generation.x);
				const ulong residency_revision = (ulong(parameters.visibility_residency_generation.y) << 32u) | ulong(parameters.visibility_residency_generation.x);
				const ulong lighting_revision = (ulong(parameters.light_distribution_generation.y) << 32u) | ulong(parameters.light_distribution_generation.x);
				const ulong environment_revision = (ulong(parameters.environment_generation.y) << 32u) | ulong(parameters.environment_generation.x);
				const ulong primary_identity = ulong(primary_surface_identity.x) | (ulong(primary_surface_identity.y) << 32u);
				uint secondary_geometry_index = 0u;
				const bool record_geometry_valid = candidate.abi_version == 3u && (candidate.flags & 1u) != 0u && candidate.source_primary_geometry_instance_id == primary_identity && candidate.source_primary_geometry_revision == geometry_revision && candidate.secondary_geometry_revision == geometry_revision && candidate.source_primary_residency_revision == residency_revision && candidate.secondary_residency_revision == residency_revision && candidate.age <= 8u && reusable_path_header.frame >= candidate.capture_frame && reusable_path_header.frame - candidate.capture_frame <= 8u && all(isfinite(candidate.secondary_world_position.xyz)) && all(isfinite(candidate.secondary_geometric_normal.xyz)) && all(isfinite(candidate.incident_radiance.xyz)) && distance(candidate.source_primary_world_position.xyz, world_position) <= 2.0f && dot(normalize(candidate.source_primary_geometric_normal.xyz), geometric_normal) >= 0.95f && reusable_path_resolve_current_geometry(candidate.secondary_geometry_instance_id, geometry_records, parameters.geometry_record_count, secondary_geometry_index);
				if (!record_geometry_valid || candidate.lighting_revision != lighting_revision || candidate.environment_revision != environment_revision) {
					if (record_geometry_valid && candidate.lighting_revision != lighting_revision) atomic_fetch_add_explicit(&material_diagnostic.reusable_path_lighting_reevaluations, 1u, memory_order_relaxed);
					if (record_geometry_valid && candidate.environment_revision != environment_revision) atomic_fetch_add_explicit(&material_diagnostic.reusable_path_environment_reevaluations, 1u, memory_order_relaxed);
					atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
				} else {
					const MaterialRecord secondary_material = materials[secondary_geometry_index];
					const ulong secondary_material_revision = (ulong(secondary_material.generation_high) << 32u) | ulong(secondary_material.generation_low);
					const float3 reconnect_origin = world_position + geometric_normal * 0.003f;
					const float3 biased_to_secondary = candidate.secondary_world_position.xyz - reconnect_origin;
					const float secondary_distance = length(biased_to_secondary);
					if (secondary_distance <= 0.004f || candidate.secondary_material_revision != secondary_material_revision || (secondary_material.flags & 1u) != 0u) {
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
					} else {
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_valid_candidates, 1u, memory_order_relaxed);
						const float3 reconnect_direction = biased_to_secondary / secondary_distance;
						// Stop just before the known secondary point. The record itself is
						// separately revalidated, so visibility means no earlier blocker;
						// asking the ray to hit its endpoint was sensitive to origin bias.
						raytracing::ray reconnect = { reconnect_origin, reconnect_direction, 0.001f, max(secondary_distance - 0.004f, 0.001f) };
						const HybridIntersection reconnect_hit = hybrid_intersect_split(reconnect, scene, opaque_scene, alpha_scene, uint(candidate.secondary_mask) & 0xffu, 0.001f, ALPHA_RAY_CLASS_VISIBILITY, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_reconnection_visibility, 1u, memory_order_relaxed);
						if (reconnect_hit.type == raytracing::intersection_type::none) {
							const float receiver_bsdf = max(dot(world_normal, reconnect_direction), 0.0f) * (1.0f / M_PI_F);
							const float3 reused_target = candidate.incident_radiance.xyz * primary_diffuse * receiver_bsdf;
							if (all(isfinite(reused_target)) && any(reused_target > float3(0.0f))) {
								restir_gi_indirect = reused_target;
								atomic_fetch_add_explicit(&material_diagnostic.reusable_path_accepted, 1u, memory_order_relaxed);
								atomic_fetch_add_explicit(&material_diagnostic.reusable_path_selected, 1u, memory_order_relaxed);
								atomic_fetch_add_explicit(&material_diagnostic.reusable_path_reused_candidates, 1u, memory_order_relaxed);
								atomic_fetch_add_explicit(&material_diagnostic.restir_gi_reused_candidates, 1u, memory_order_relaxed);
								atomic_fetch_add_explicit(&material_diagnostic.restir_gi_selected_reuse, 1u, memory_order_relaxed);
							} else {
								atomic_fetch_add_explicit(&material_diagnostic.reusable_path_zero_target, 1u, memory_order_relaxed);
								atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							}
						} else {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_endpoint_blocked, 1u, memory_order_relaxed);
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
						}
					}
				}
			}
		}
		// Real-time world-path reuse is an estimator replacement, not an additive
		// feature. Execute one fresh diffuse proposal and let the current-measure
		// reservoir merge a bounded cache proposal below. Disabling reusable paths
		// retains the configured multi-sample progressive/reference estimator.
		const bool realtime_path_reuse = reusable_path_transport_enabled && (parameters.experimental_feature_flags & 4u) != 0u;
		uint gi_state = hash_u32(pixel.x + pixel.y * parameters.dimensions.x);
		uint sample_count = max(parameters.gi_sample_count, 1u);
		const uint gi_normalization_samples = max(sample_count, 1u);
		for (uint sample = 0u; sample < sample_count; sample++) {
			const uint gi_dimension = realtime_path_reuse ? FLUX_SAMPLE_DIMENSION_PRIMARY_GI_U : 4u;
			const float u = hammersley_dimension(parameters.frame_index, sample, sample_count, gi_state, gi_dimension, parameters, stbn_scalar_volume, pixel);
			const float v = hammersley_dimension(parameters.frame_index, sample, sample_count, gi_state, gi_dimension + 1u, parameters, stbn_scalar_volume, pixel);
			float phi = v * 6.28318530718f;
			float radius = sqrt(u);
			float z = sqrt(max(0.0f, 1.0f - radius * radius));
			float3 direction = normalize(tangent * (cos(phi) * radius) + bitangent * (sin(phi) * radius) + world_normal * z);
			raytracing::ray gi_ray = { world_position + geometric_normal * (dot(direction, geometric_normal) >= 0.0f ? 0.003f : -0.003f), direction, 0.001f, parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 100000.0f };
			atomic_fetch_add_explicit(&material_diagnostic.gi_fresh_rays, 1u, memory_order_relaxed);
			const float gi_ray_spread = max(1.0f / float(max(parameters.dimensions.x, parameters.dimensions.y)) + 0.15f, 0.00025f);
			// Thin transmission needs the exact mixed scene so it can reconnect
			// through alpha-tested panes without silently treating them as opaque.
			float3 gi_transmission = float3(1.0f);
			HybridIntersection gi_hit = hybrid_intersect_thin_reconnect(gi_ray, scene, opaque_scene, alpha_scene, 0xffu, gi_ray_spread, ALPHA_RAY_CLASS_INDIRECT, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, gi_transmission);
			float3 incoming = 0.0f;
			float3 reusable_secondary_position = 0.0f;
			float3 reusable_secondary_geometric_normal = 0.0f;
			float3 reusable_secondary_shading_normal = 0.0f;
			bool reusable_secondary_opaque = false;
			if (gi_hit.type == raytracing::intersection_type::triangle) {
				MaterialRecord material = materials[gi_hit.instance_id];
				float3 hit_normal = intersection_normal(geometry_records, gi_hit.instance_id, gi_hit.primitive_id, gi_hit.triangle_barycentric_coord, -gi_ray.direction);
				if (dot(hit_normal, -gi_ray.direction) < 0.0f) hit_normal = -hit_normal;
				float3 hit_position = gi_ray.origin + gi_ray.direction * gi_hit.distance;
				const MaterialSample hit_sample = sample_material(material, geometry_records, gi_hit.instance_id, gi_hit.primitive_id, gi_hit.triangle_barycentric_coord, primary_distance + gi_hit.distance, gi_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
				hit_normal = material_shading_normal(material, geometry_records, gi_hit.instance_id, gi_hit.primitive_id, gi_hit.triangle_barycentric_coord, hit_normal, primary_distance + gi_hit.distance, gi_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
				reusable_secondary_position = hit_position;
				reusable_secondary_geometric_normal = normalize(intersection_normal(geometry_records, gi_hit.instance_id, gi_hit.primitive_id, gi_hit.triangle_barycentric_coord, -gi_ray.direction));
				reusable_secondary_shading_normal = hit_normal;
				reusable_secondary_opaque = (material.flags & 1u) == 0u && (material.flags & 8u) == 0u;
				float3 hit_diffuse = hit_sample.albedo * (1.0f - hit_sample.metallic);
				float3 hit_f0 = mix(float3(0.08f * clamp(material.specular, 0.0f, 1.0f)), hit_sample.albedo, hit_sample.metallic);
				const float3 secondary_direct_nee =
						sample_emissive_lighting(hit_position, hit_normal, hit_diffuse, gi_ray.direction, hit_sample.roughness, hit_f0, 1u, materials, geometry_records, emissive_triangles, parameters.emissive_triangle_count, parameters.frame_index, gi_state, parameters.transport_max_distance, parameters, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) +
						sample_punctual_lighting(hit_position, hit_normal, hit_diffuse, gi_ray.direction, hit_sample.roughness, hit_f0, material.visibility_mask, 0u, true, true, punctual_lights, parameters.punctual_light_count, parameters.frame_index, gi_state, parameters.transport_max_distance, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) +
						sample_solar_lobe_lighting(hit_position, hit_normal, hit_diffuse, gi_ray.direction, hit_sample.roughness, hit_f0, parameters.frame_index, gi_state, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel) +
						sample_environment_lighting(hit_position, hit_normal, hit_diffuse, gi_ray.direction, hit_sample.roughness, hit_f0, parameters.frame_index, gi_state, FLUX_SAMPLE_DIMENSION_SECONDARY_ENVIRONMENT, false, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, environment_radiance, environment_importance, environment_sampler, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
				const float emitter_light_pdf = emissive_solid_angle_pdf_for_hit(gi_hit.instance_id, gi_hit.primitive_id, gi_hit.distance * gi_hit.distance, gi_ray.direction, geometry_records, emissive_triangles, parameters.emissive_triangle_count);
				const float diffuse_pdf = max(dot(world_normal, direction), 0.0f) * (1.0f / M_PI_F);
				const float emitter_bsdf_weight = emitter_light_pdf > 0.0f ? power_heuristic(diffuse_pdf, emitter_light_pdf) : 1.0f;
				const float3 deeper_diffuse_indirect = hit_sample.emission * emitter_bsdf_weight;
				incoming = (secondary_direct_nee + deeper_diffuse_indirect) * gi_transmission;
			} else if (parameters.environment_info.w <= 2.5f) {
				const float bsdf_pdf = max(dot(world_normal, direction), 0.0f) * (1.0f / M_PI_F);
				const float env_pdf = environment_pdf(direction, parameters, environment_importance);
				environment_bsdf_direct += environment_lookup(direction, parameters, environment_radiance, environment_sampler) * (bsdf_pdf / max(bsdf_pdf + env_pdf, 0.000001f));
			}
			if (gi_hit.type == raytracing::intersection_type::triangle) {
				const float fresh_pdf = max(dot(world_normal, direction), 0.0f) * (1.0f / M_PI_F);
				if (realtime_path_reuse && sample == 0u && fresh_pdf > 0.0f && all(isfinite(incoming))) {
					restir_gi_fresh_incoming = incoming;
					restir_gi_fresh_pdf = fresh_pdf;
					restir_gi_fresh_valid = true;
					atomic_fetch_add_explicit(&material_diagnostic.restir_gi_current_candidates, 1u, memory_order_relaxed);
				} else {
					indirect += incoming;
				}
			}
			// A fresh, opaque secondary hit may populate one deterministic staging
			// slot. This is not a trace-time cache update: the post-trace reduction
			// owns persistent storage, and alpha records are never staged because an
			// exact alpha reconnection payload is not yet available.
			if (reusable_path_transport_enabled && (parameters.experimental_feature_flags & 4u) != 0u && sample == 0u && reusable_path_header.dimensions_and_flags.w != 0u && gi_hit.type == raytracing::intersection_type::triangle) {
				const MaterialRecord secondary_material = materials[gi_hit.instance_id];
				const float source_pdf = max(dot(world_normal, direction), 0.0f) * (1.0f / M_PI_F);
				const float3 staging_incident_radiance = max(incoming, float3(0.0f));
				const float staging_incident_luminance = dot(staging_incident_radiance, float3(0.2126f, 0.7152f, 0.0722f));
				// Claim only a usable transport proposal. A zero contribution must not
				// permanently consume this frame's first-producer slot for the cell.
				const bool staging_radiance_eligible = all(isfinite(incoming)) && staging_incident_luminance > 0.0f;
				const int3 cell_key = int3(floor(world_position / max(float(reusable_path_header.center_and_cell_size.w), 0.25f)));
				uint cell_index = 0u;
				uint source_primary_geometry_index = 0u;
				const ulong source_primary_identity = ulong(primary_surface_identity.x) | (ulong(primary_surface_identity.y) << 32u);
				const bool source_primary_resolved = reusable_path_resolve_current_geometry(source_primary_identity, geometry_records, parameters.geometry_record_count, source_primary_geometry_index);
				if (reusable_secondary_opaque && source_primary_resolved && source_pdf > 0.0f && staging_radiance_eligible && all(isfinite(world_position)) && all(isfinite(reusable_secondary_position)) && reusable_path_index_for_key(cell_key, reusable_path_header, cell_index)) {
					// The old hash-to-screen owner was usually not a visible pixel, so
					// world cells never populated. Claim the first eligible visible
					// producer once; the immutable previous/next reduction remains the
					// sole persistent-cache writer.
					bool claimed_staging_cell = false;
					for (uint attempt = 0u; attempt < 2u && !claimed_staging_cell; attempt++) {
						uint expected_claim = 0xffffffffu;
						claimed_staging_cell = atomic_compare_exchange_weak_explicit(&reusable_path_staging_claims[cell_index], &expected_claim, pixel.y * parameters.dimensions.x + pixel.x, memory_order_relaxed, memory_order_relaxed);
						if (expected_claim != 0xffffffffu) break;
					}
					if (claimed_staging_cell) {
					const MaterialRecord source_primary_material = materials[source_primary_geometry_index];
					ReusablePathSampleRecord record = {};
					record.secondary_geometry_instance_id = ulong(geometry_records[gi_hit.instance_id].instance_identity_low) | (ulong(geometry_records[gi_hit.instance_id].instance_identity_high) << 32u);
					record.secondary_material_id = ulong(gi_hit.instance_id) + 1u;
					record.secondary_surface_id = gi_hit.instance_id;
					record.secondary_primitive_id = gi_hit.primitive_id;
					record.source_primary_geometry_instance_id = ulong(primary_surface_identity.x) | (ulong(primary_surface_identity.y) << 32u);
					record.source_primary_material_id = ulong(source_primary_geometry_index) + 1u;
					record.abi_version = 3u;
					record.flags = 1u;
					record.secondary_geometry_revision = (ulong(parameters.admitted_geometry_generation.y) << 32u) | ulong(parameters.admitted_geometry_generation.x);
					record.secondary_material_revision = (ulong(secondary_material.generation_high) << 32u) | ulong(secondary_material.generation_low);
					record.secondary_residency_revision = (ulong(parameters.visibility_residency_generation.y) << 32u) | ulong(parameters.visibility_residency_generation.x);
					record.source_primary_geometry_revision = record.secondary_geometry_revision;
					record.source_primary_material_revision = (ulong(source_primary_material.generation_high) << 32u) | ulong(source_primary_material.generation_low);
					record.source_primary_residency_revision = record.secondary_residency_revision;
					record.lighting_revision = (ulong(parameters.light_distribution_generation.y) << 32u) | ulong(parameters.light_distribution_generation.x);
					record.environment_revision = (ulong(parameters.environment_generation.y) << 32u) | ulong(parameters.environment_generation.x);
					record.source_primary_mask = ulong(source_primary_material.visibility_mask);
					record.secondary_mask = ulong(secondary_material.visibility_mask);
					record.capture_frame = reusable_path_header.frame;
					record.secondary_world_position = float4(reusable_secondary_position, 0.0f);
					record.secondary_geometric_normal = float4(reusable_secondary_geometric_normal, 0.0f);
					record.secondary_shading_normal = float4(reusable_secondary_shading_normal, 0.0f);
					record.source_primary_world_position = float4(world_position, 0.0f);
					record.source_primary_geometric_normal = float4(geometric_normal, 0.0f);
					record.source_primary_shading_normal = float4(world_normal, 0.0f);
					record.throughput = float4(primary_diffuse, 1.0f);
					record.incident_radiance = float4(staging_incident_radiance, 1.0f);
					record.outgoing_radiance = record.incident_radiance;
					record.source_primary_proposal_solid_angle_pdf = source_pdf;
					record.target = 1.0f;
					record.normalization = 1.0f;
					record.secondary_barycentric_u = gi_hit.triangle_barycentric_coord.x;
					record.secondary_barycentric_v = gi_hit.triangle_barycentric_coord.y;
					reusable_path_staging[cell_index] = record;
					atomic_fetch_add_explicit(&material_diagnostic.reusable_path_staged, 1u, memory_order_relaxed);
					}
				}
			}
		}
			// One current fresh candidate plus at most one current-resolved world-cache
			// candidate. Both weights are M*p_hat/q in the current primary solid-angle
			// measure; the selected vector target is normalized by W/(M*p_hat).
			if (realtime_path_reuse && restir_gi_fresh_valid) {
				float3 selected_target = restir_gi_fresh_incoming * primary_diffuse * restir_gi_fresh_pdf;
				float selected_target_scalar = max(dot(selected_target, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
				float weight_sum = selected_target_scalar / restir_gi_fresh_pdf;
				uint represented_m = 1u;
				bool selected_reused = false;
				uint selected_reconnect_geometry = 0xffffffffu;
				uint selected_reconnect_primitive = 0xffffffffu;
				uint selected_reconnect_mask = 0u;
				float3 selected_reconnect_direction = 0.0f;
				float selected_reconnect_distance = 0.0f;
				if (reusable_path_transport_enabled && (parameters.experimental_feature_flags & 4u) != 0u && reusable_path_header.previous_center_and_flags.w != 0u && any(primary_surface_identity != uint2(0u))) {
					const float path_cell_size = max(float(reusable_path_header.center_and_cell_size.w), 0.25f);
					const int3 base_key = int3(floor(world_position / path_cell_size));
					bool considered_reused = false;
					// Query the receiver cell and one deterministic face neighbor. The
					// former 3x3x3 scan performed up to 27 cache probes per pixel even
					// though the estimator merges at most one reused proposal.
					for (uint reuse_query = 0u; reuse_query < 2u && !considered_reused; reuse_query++) {
						const uint neighbor_selector = hash_u32(gi_state ^ parameters.frame_index) % 6u;
						const int neighbor_sign = (neighbor_selector & 1u) != 0u ? 1 : -1;
						const uint neighbor_axis = neighbor_selector >> 1u;
						const int3 neighbor_offset = reuse_query == 0u ? int3(0) : int3(neighbor_axis == 0u ? neighbor_sign : 0, neighbor_axis == 1u ? neighbor_sign : 0, neighbor_axis == 2u ? neighbor_sign : 0);
						uint candidate_index = 0u;
						if (!reusable_path_index_for_key(base_key + neighbor_offset, reusable_path_header, candidate_index)) continue;
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_queries, 1u, memory_order_relaxed);
						const ReusablePathSampleRecord candidate = reusable_path_previous[candidate_index];
						const ulong geometry_revision = (ulong(parameters.admitted_geometry_generation.y) << 32u) | ulong(parameters.admitted_geometry_generation.x);
						const ulong residency_revision = (ulong(parameters.visibility_residency_generation.y) << 32u) | ulong(parameters.visibility_residency_generation.x);
						const ulong lighting_revision = (ulong(parameters.light_distribution_generation.y) << 32u) | ulong(parameters.light_distribution_generation.x);
						const ulong environment_revision = (ulong(parameters.environment_generation.y) << 32u) | ulong(parameters.environment_generation.x);
						const ulong source_identity = ulong(primary_surface_identity.x) | (ulong(primary_surface_identity.y) << 32u);
						const bool barycentric_valid = candidate.secondary_barycentric_u >= 0.0f && candidate.secondary_barycentric_v >= 0.0f && candidate.secondary_barycentric_u + candidate.secondary_barycentric_v <= 1.0f;
						if (candidate.abi_version != 3u || (candidate.flags & 1u) == 0u || candidate.source_primary_geometry_instance_id != source_identity || candidate.source_primary_geometry_revision != geometry_revision || candidate.secondary_geometry_revision != geometry_revision || candidate.source_primary_residency_revision != residency_revision || candidate.secondary_residency_revision != residency_revision || candidate.age > 8u || reusable_path_header.frame < candidate.capture_frame || reusable_path_header.frame - candidate.capture_frame > 8u || !all(isfinite(candidate.source_primary_world_position.xyz)) || !all(isfinite(candidate.secondary_world_position.xyz)) || !all(isfinite(candidate.secondary_geometric_normal.xyz)) || !barycentric_valid || !(candidate.source_primary_proposal_solid_angle_pdf > 0.0f) || distance(candidate.source_primary_world_position.xyz, world_position) > 2.0f || dot(normalize(candidate.source_primary_geometric_normal.xyz), geometric_normal) < 0.95f) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_invalid_record, 1u, memory_order_relaxed);
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						// A light/environment mismatch is proposal-only: current emission,
						// NEE, and visibility below are mandatory, so it cannot authorize
						// a stale contribution or invalidate stable path geometry.
						const bool lighting_changed = candidate.lighting_revision != lighting_revision;
						const bool environment_changed = candidate.environment_revision != environment_revision;
						if (lighting_changed) atomic_fetch_add_explicit(&material_diagnostic.reusable_path_lighting_reevaluations, 1u, memory_order_relaxed);
						if (environment_changed) atomic_fetch_add_explicit(&material_diagnostic.reusable_path_environment_reevaluations, 1u, memory_order_relaxed);
						// Never consume a stale radiance payload. The one fresh path remains
						// active and its deterministic world-cell owner stages the record
						// under current light/environment revisions for the next frame.
						// Geometry stays resident and is not invalidated by this mismatch.
						if (lighting_changed || environment_changed || !all(isfinite(candidate.incident_radiance.xyz))) continue;
						uint current_primary_geometry_index = 0u;
						if (!reusable_path_resolve_current_geometry(candidate.source_primary_geometry_instance_id, geometry_records, parameters.geometry_record_count, current_primary_geometry_index)) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						const MaterialRecord current_primary_material = materials[current_primary_geometry_index];
						const ulong current_primary_material_revision = (ulong(current_primary_material.generation_high) << 32u) | ulong(current_primary_material.generation_low);
						if (candidate.source_primary_material_id != ulong(current_primary_geometry_index) + 1u || candidate.source_primary_material_revision != current_primary_material_revision || candidate.source_primary_mask != ulong(current_primary_material.visibility_mask)) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						const float3 source_to_y = candidate.secondary_world_position.xyz - candidate.source_primary_world_position.xyz;
						const float source_distance_squared = dot(source_to_y, source_to_y);
						const float3 reconnect_origin = world_position + geometric_normal * 0.003f;
						const float3 current_to_y = candidate.secondary_world_position.xyz - reconnect_origin;
						const float current_distance = length(current_to_y);
						const float source_distance = sqrt(max(source_distance_squared, 0.0f));
						const float source_cosine = source_distance > 0.0001f ? abs(dot(normalize(candidate.secondary_geometric_normal.xyz), -source_to_y / source_distance)) : 0.0f;
						const float current_cosine = current_distance > 0.0001f ? abs(dot(normalize(candidate.secondary_geometric_normal.xyz), -current_to_y / current_distance)) : 0.0f;
						const float area_pdf = candidate.source_primary_proposal_solid_angle_pdf * source_cosine / max(source_distance_squared, 0.000001f);
						const float current_pdf = area_pdf * current_distance * current_distance / max(current_cosine, 0.000001f);
						uint current_geometry_index = 0u;
						if (!(source_cosine > 0.0f) || !(current_cosine > 0.0f) || !(current_pdf > 0.0f) || !isfinite(current_pdf) || !reusable_path_resolve_current_geometry(candidate.secondary_geometry_instance_id, geometry_records, parameters.geometry_record_count, current_geometry_index)) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						const MaterialRecord current_material = materials[current_geometry_index];
						const ulong current_material_revision = (ulong(current_material.generation_high) << 32u) | ulong(current_material.generation_low);
						if (candidate.secondary_material_revision != current_material_revision || (current_material.flags & 1u) != 0u) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_valid_candidates, 1u, memory_order_relaxed);
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_considered, 1u, memory_order_relaxed);
						// The independently validated cache endpoint is not a blocker.
						// Limit the reconnect to the open segment immediately before it.
						raytracing::ray reconnect = { reconnect_origin, current_to_y / current_distance, 0.001f, max(current_distance - 0.004f, 0.001f) };
						const float2 barycentric = float2(candidate.secondary_barycentric_u, candidate.secondary_barycentric_v);
						float3 current_normal = intersection_normal(geometry_records, current_geometry_index, candidate.secondary_primitive_id, barycentric, -reconnect.direction);
						if (dot(current_normal, -reconnect.direction) < 0.0f) current_normal = -current_normal;
						const float cached_ray_spread = max(1.0f / float(max(parameters.dimensions.x, parameters.dimensions.y)) + 0.15f, 0.00025f);
						const MaterialSample current_sample = sample_material(current_material, geometry_records, current_geometry_index, candidate.secondary_primitive_id, barycentric, primary_distance + current_distance, cached_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
						current_normal = material_shading_normal(current_material, geometry_records, current_geometry_index, candidate.secondary_primitive_id, barycentric, current_normal, primary_distance + current_distance, cached_ray_spread, material_textures, material_sampler, parameters.material_texture_capacity);
						if (!all(isfinite(current_sample.albedo)) || !isfinite(current_sample.roughness) || dot(current_normal, normalize(candidate.secondary_shading_normal.xyz)) < 0.95f) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_shading_invalid, 1u, memory_order_relaxed);
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						// Stable world records are shaded once when staged. Consumers only
						// resample the current material/normal and perform the bounded fresh
						// x-to-y reconnection above.
						const float3 current_incoming = candidate.incident_radiance.xyz;
						const float receiver_bsdf = max(dot(world_normal, current_to_y / current_distance), 0.0f) * (1.0f / M_PI_F);
						const float3 reused_target = current_incoming * primary_diffuse * receiver_bsdf;
						const float reused_target_scalar = max(dot(reused_target, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
						if (!(reused_target_scalar > 0.0f) || !all(isfinite(reused_target))) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_zero_target, 1u, memory_order_relaxed);
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_rejections, 1u, memory_order_relaxed);
							continue;
						}
						const float reused_weight = reused_target_scalar / current_pdf;
						const float new_weight_sum = weight_sum + reused_weight;
						if (isfinite(reused_weight) && reused_weight > 0.0f && isfinite(new_weight_sum)) {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_accepted, 1u, memory_order_relaxed);
							const float select = hammersley_dimension(parameters.frame_index, 0u, 1u, gi_state, FLUX_SAMPLE_DIMENSION_PRIMARY_GI_U + 4u, parameters, stbn_scalar_volume, pixel);
							if (select * new_weight_sum < reused_weight) {
								atomic_fetch_add_explicit(&material_diagnostic.reusable_path_selected, 1u, memory_order_relaxed);
								selected_target = reused_target;
								selected_target_scalar = reused_target_scalar;
								selected_reused = true;
								selected_reconnect_geometry = current_geometry_index;
								selected_reconnect_primitive = candidate.secondary_primitive_id;
								selected_reconnect_mask = uint(candidate.secondary_mask) & 0xffu;
								selected_reconnect_direction = reconnect.direction;
								selected_reconnect_distance = current_distance;
							}
							weight_sum = new_weight_sum;
							represented_m++;
							considered_reused = true;
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_reused_candidates, 1u, memory_order_relaxed);
							atomic_fetch_add_explicit(&material_diagnostic.restir_gi_reused_candidates, 1u, memory_order_relaxed);
						} else {
							atomic_fetch_add_explicit(&material_diagnostic.reusable_path_invalid_weight, 1u, memory_order_relaxed);
						}
					}
				}
				bool selected_visible = true;
				if (selected_reused) {
					const float3 selected_reconnect_origin = world_position + geometric_normal * 0.003f;
					raytracing::ray selected_reconnect = { selected_reconnect_origin, selected_reconnect_direction, 0.001f, max(selected_reconnect_distance - 0.004f, 0.001f) };
					const HybridIntersection selected_hit = hybrid_intersect_split(selected_reconnect, scene, opaque_scene, alpha_scene, selected_reconnect_mask, 0.001f, ALPHA_RAY_CLASS_VISIBILITY, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector);
					atomic_fetch_add_explicit(&material_diagnostic.reusable_path_reconnection_visibility, 1u, memory_order_relaxed);
					selected_visible = selected_hit.type == raytracing::intersection_type::none;
					if (!selected_visible) {
						atomic_fetch_add_explicit(&material_diagnostic.reusable_path_endpoint_blocked, 1u, memory_order_relaxed);
					}
				}
				if (selected_visible && selected_target_scalar > 0.0f && weight_sum > 0.0f && isfinite(weight_sum)) {
					restir_gi_indirect += selected_target * (weight_sum / (float(represented_m) * selected_target_scalar));
					if (selected_reused) atomic_fetch_add_explicit(&material_diagnostic.restir_gi_selected_reuse, 1u, memory_order_relaxed);
				}
			}
		environment_bsdf_direct *= primary_diffuse / float(gi_normalization_samples);
			indirect = (indirect * primary_diffuse + restir_gi_indirect) * parameters.gi_strength / float(gi_normalization_samples);
	}
	// Authored AO modulates ambient/indirect response once. Direct visibility is
	// already resolved by Metal rays, so no additional contact multiplier applies.
	const float3 environment_bsdf_transport = environment_bsdf_direct * primary_ambient_occlusion;
	const float3 indirect_transport = indirect * primary_ambient_occlusion;
	const float3 solar_direct = sample_solar_lobe_lighting(world_position, world_normal, primary_diffuse, view_direction, roughness, primary_f0, parameters.frame_index, state, parameters, materials, geometry_records, material_textures, material_sampler, material_diagnostic, alpha_intersection_table, intersector, scene, opaque_scene, alpha_scene, stbn_scalar_volume, pixel);
	const float3 direct_diffuse_signal = clamp(primary_emission + emissive_direct + bidirectional_caustic_direct + analytic_direct + environment_direct + solar_direct, 0.0f, 32.0f);
	const float3 secondary_diffuse_signal = clamp(environment_bsdf_transport + indirect_transport, 0.0f, 32.0f);
	// Raster contributes stable authored emission only. Metal owns all direct
	// visibility, Sky/GI, and reflection shading, so shadows cannot be duplicated.
	const float3 specular_signal = clamp(reflection * reflection_weight, 0.0f, 32.0f);
	// Classification can suppress only stable secondary transport. MetalFX mode
	// replays one validated raw path sample, never a reconstructed or blended
	// image value, so direct light remains part of that exact frame-stable sample.
	const bool metalfx_image_denoiser = (parameters.reconstruction_flags & 16u) != 0u;
	const bool preserve_diffuse_history = !metalfx_image_denoiser && (parameters.reconstruction_flags & 8u) != 0u && (need_mask & TRACE_NEED_GI) == 0u && (flags & 4u) != 0u;
	const bool preserve_specular_history = !metalfx_image_denoiser && (parameters.reconstruction_flags & 8u) != 0u && (need_mask & TRACE_NEED_REFLECTION) == 0u && (flags & 1u) != 0u;
	const float4 diffuse_transport_sample = diffuse_transport_sample_input.read(pixel);
	const float4 specular_transport_sample = specular_transport_sample_input.read(pixel);
	const bool replay_diffuse_transport_sample = metalfx_image_denoiser && (parameters.reconstruction_flags & 8u) != 0u && (need_mask & TRACE_NEED_GI) == 0u && (flags & 4u) != 0u && diffuse_transport_sample.a > 0.5f && all(isfinite(diffuse_transport_sample.rgb));
	const bool replay_specular_transport_sample = metalfx_image_denoiser && (parameters.reconstruction_flags & 8u) != 0u && (need_mask & TRACE_NEED_REFLECTION) == 0u && (flags & 1u) != 0u && specular_transport_sample.a > 0.5f && all(isfinite(specular_transport_sample.rgb));
	const float3 effective_diffuse_signal = preserve_diffuse_history ? clamp(diffuse_history_input.read(pixel).rgb, 0.0f, 32.0f) : clamp(direct_diffuse_signal + (replay_diffuse_transport_sample ? diffuse_transport_sample.rgb : secondary_diffuse_signal), 0.0f, 32.0f);
	const float3 effective_specular_signal = preserve_specular_history ? clamp(specular_history_input.read(pixel).rgb, 0.0f, 32.0f) : replay_specular_transport_sample ? clamp(specular_transport_sample.rgb, 0.0f, 32.0f) : specular_signal;
	// Zero-radiance transport is still an exact sample. Treating it as missing
	// caused a stable black GI/reflection path to retrace forever; the alpha bit
	// records sampling validity, not contribution magnitude.
	const bool retain_diffuse_transport_sample = !replay_diffuse_transport_sample && (need_mask & TRACE_NEED_GI) != 0u && (flags & 4u) != 0u && all(isfinite(secondary_diffuse_signal));
	const bool retain_specular_transport_sample = !replay_specular_transport_sample && (need_mask & TRACE_NEED_REFLECTION) != 0u && (flags & 1u) != 0u && all(isfinite(specular_signal));
	stage_probe_add(parameters, material_diagnostic.stage_raw_emission, primary_emission);
	stage_probe_add(parameters, material_diagnostic.stage_raw_emissive, emissive_direct);
	stage_probe_add(parameters, material_diagnostic.stage_raw_analytic, analytic_direct);
	stage_probe_add(parameters, material_diagnostic.stage_raw_indirect, bidirectional_caustic_direct + indirect_transport + environment_direct + environment_bsdf_transport + solar_direct);
	stage_probe_add(parameters, material_diagnostic.stage_trace_combined, effective_diffuse_signal + effective_specular_signal);
	if (any(effective_diffuse_signal + effective_specular_signal > float3(0.0001f))) atomic_fetch_add_explicit(&material_diagnostic.primary_lit_pixels, 1u, memory_order_relaxed);
	// These are raw stochastic transport signals. The Flux reconstruction path
	// may consume them only when MetalFX is inactive; MetalFX receives this raw
	// ray-owned radiance plus the guide set below and is then the sole image
	// denoiser.
	split_diffuse.write(float4(effective_diffuse_signal, 0.0f), pixel);
	split_specular.write(float4(effective_specular_signal, preserve_specular_history || replay_specular_transport_sample ? specular_history_input.read(pixel).a : specular_hit_distance), pixel);
	diffuse_transport_sample_output.write(float4(replay_diffuse_transport_sample ? diffuse_transport_sample.rgb : secondary_diffuse_signal, retain_diffuse_transport_sample || replay_diffuse_transport_sample ? 1.0f : 0.0f), pixel);
	specular_transport_sample_output.write(float4(replay_specular_transport_sample ? specular_transport_sample.rgb : specular_signal, retain_specular_transport_sample || replay_specular_transport_sample ? 1.0f : 0.0f), pixel);
	reservoir_output.write(uint4(reservoir.source_type, reservoir.source_index, reservoir.source_identity.x, reservoir.source_identity.y), pixel);
	// Zero is reserved for a ray that did not validate a primary material, so
	// instance zero remains a valid temporal material/geometry identity.
	reservoir_surface_output.write(float4(oct_encode(world_normal), primary_distance, any(primary_surface_identity != uint2(0u)) ? 1.0f : 0.0f), pixel);
	reservoir_primary_identity_output.write(uint4(primary_surface_identity, 0u, 0u), pixel);
	primary_shading_output.write(any(primary_surface_identity != uint2(0u)) ? float4(clamp(primary_diffuse, 0.0f, 1.0f), clamp(roughness, 0.0f, 1.0f)) : float4(0.0f), pixel);
	// Alpha is an explicit validity bit: background and unsupported primary
	// surfaces never become reusable world-space transport samples.
	primary_world_position_output.write(any(primary_surface_identity != uint2(0u)) && all(isfinite(world_position)) ? float4(world_position, 1.0f) : float4(0.0f), pixel);
	reservoir_metadata_output.write(float4(reservoir.target, reservoir.weight_sum, float(reservoir.candidate_count), reservoir.valid ? float(reservoir.age + 1u) : 0.0f), pixel);
	reservoir_sample_output.write(float4(float(reservoir.triangle_record), reservoir.barycentric, reservoir.proposal_pdf), pixel);
	effect_texture.write(float4(effective_diffuse_signal + effective_specular_signal, 1.0f), pixel);
	// Apple MetalFX expects world-space geometric guides. These remain tied to
	// the ray-validated primary surface and its depth/motion record.
	guide_normal.write(float4(reflection_guide_normal, 0.0f), pixel);
	guide_diffuse.write(float4(clamp(reflection_guide_diffuse, 0.0f, 1.0f), 1.0f), pixel);
	// MetalFX needs the view-dependent Fresnel albedo that contributed to this
	// pixel, not a normal-incidence F0 material parameter.
	float3 specular_albedo = reflection_guide_f0 + (1.0f - reflection_guide_f0) * pow(1.0f - reflection_guide_cosine, 5.0f);
	guide_specular.write(float4(clamp(specular_albedo, 0.0f, 1.0f), 1.0f), pixel);
	guide_roughness.write(float4(clamp(reflection_guide_roughness, 0.0f, 1.0f)), pixel);
	// Apple's denoise-strength mask uses 1 to exclude a pixel from denoising,
	// and a reactive value of 1 discards temporal history. Stable opaque transport
	// remains denoisable. For an actual history mismatch, use the raster
	// motion mapping and the same surface identity/depth/normal evidence used by
	// the reservoir, rather than globally treating an asset class as reactive.
	float reactive = 0.0f;
	if (parameters.history_valid != 0u) {
		const float2 raster_velocity = velocity_texture.read(pixel).xy;
		const bool velocity_valid = all(isfinite(raster_velocity));
		const float2 previous_uv = uv + raster_velocity;
		bool compatible_history = velocity_valid && all(previous_uv >= 0.0f) && all(previous_uv < 1.0f) && any(primary_surface_identity != uint2(0u));
		if (compatible_history) {
			const uint2 previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
			const float4 prior_surface = reservoir_surface_input.read(previous_pixel);
			const uint2 prior_identity = reservoir_primary_identity_input.read(previous_pixel).xy;
			compatible_history = all(prior_identity == primary_surface_identity) && prior_surface.w != 0.0f &&
				abs(prior_surface.z - primary_distance) / max(primary_distance, 0.001f) < 0.02f &&
				dot(oct_decode(prior_surface.xy), world_normal) > 0.8f;
		}
		// The velocity maps the current sample into prior screen space; stable
		// identity/depth/normal evidence then decides whether that reprojected
		// sample is usable. A genuine disocclusion must be reactive even when a
		// platform motion stream has no per-vertex delta for a morph that changed
		// its ray-visible depth. Conversely, sub-pixel jitter remains nonreactive
		// because its reprojected primary identity, depth, and normal still match.
		const bool history_mismatch = !compatible_history;
		reactive = history_mismatch ? 1.0f : 0.0f;
	}
	// Reactive history rejection is guide validity, not a Flux image filter. It
	// remains available to the downstream MetalFX denoiser.
	guide_denoise_strength.write(float4(0.0f), pixel);
	guide_reactive.write(float4(reactive), pixel);
	if ((parameters.reconstruction_flags & 2u) != 0u) {
		atomic_fetch_add_explicit(&material_diagnostic.metalfx_reactive_opaque_pixels, 1u, memory_order_relaxed);
		if (reactive > 0.5f) atomic_fetch_add_explicit(&material_diagnostic.metalfx_reactive_rejected_pixels, 1u, memory_order_relaxed);
	}
	guide_specular_distance.write(float4(specular_hit_distance), pixel);
	// Flux raster clears this guide during the ray pass, then renders the alpha list
	// into this same linear overlay after opaque transport. Keeping it zero here
	// avoids double compositing while preserving MetalFX overlay semantics.
	guide_transparency.write(float4(0.0f), pixel);
}

static float split_luminance(float3 color) {
	return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

static bool split_shading_compatible(float4 current_shading, float4 prior_shading) {
	if (!all(isfinite(current_shading)) || !all(isfinite(prior_shading))) return false;
	const float current_roughness = clamp(current_shading.w, 0.0f, 1.0f);
	const float prior_roughness = clamp(prior_shading.w, 0.0f, 1.0f);
	// Glossy transport is exceptionally sensitive to an authored material
	// boundary. Keep the stricter threshold for either low-roughness receiver.
	const bool glossy = min(current_roughness, prior_roughness) < 0.20f;
	const float albedo_delta = max(max(abs(current_shading.x - prior_shading.x), abs(current_shading.y - prior_shading.y)), abs(current_shading.z - prior_shading.z));
	return albedo_delta <= (glossy ? 0.06f : 0.14f) && abs(current_roughness - prior_roughness) <= (glossy ? 0.025f : 0.10f);
}

static bool split_surface_compatible(float4 current_surface, float4 prior_surface, uint2 current_identity, uint2 prior_identity, float4 current_shading, float4 prior_shading) {
	const float current_distance = current_surface.z;
	const float prior_distance = prior_surface.z;
	return any(current_identity != uint2(0u)) && all(current_identity == prior_identity) &&
		current_surface.w != 0.0f && prior_surface.w != 0.0f &&
		uint(current_surface.w) == uint(prior_surface.w) &&
		abs(current_distance - prior_distance) / max(current_distance, 0.001f) < 0.02f &&
		dot(oct_decode(current_surface.xy), oct_decode(prior_surface.xy)) > (min(current_shading.w, prior_shading.w) < 0.20f ? 0.95f : 0.8f) &&
		split_shading_compatible(current_shading, prior_shading);
}

// The stable c2e89bba6c reconstruction filter. Its variance-aware luminance
// window rejects isolated fireflies while depth, normal, and identity checks
// prevent transport from crossing primary-surface boundaries.
static float3 split_spatial_filter(
		texture2d<float, access::read> signal,
		texture2d<float, access::read> surface,
		texture2d<uint, access::read> identity,
		texture2d<float, access::read> shading,
		uint2 pixel,
		uint2 dimensions,
		float variance) {
	const float4 center_surface = surface.read(pixel);
	const uint2 center_identity = identity.read(pixel).xy;
	const float4 center_shading = shading.read(pixel);
	const float3 center = signal.read(pixel).rgb;
	const float center_luminance = split_luminance(center);
	const float standard_deviation = max(sqrt(max(variance, 0.0f)), 0.025f);
	float3 sum = 0.0f;
	float weight_sum = 0.0f;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			const uint2 sample_pixel = uint2(clamp(int2(pixel) + int2(x, y), int2(0), int2(dimensions) - 1));
			const float4 sample_surface = surface.read(sample_pixel);
			if (!split_surface_compatible(center_surface, sample_surface, center_identity, identity.read(sample_pixel).xy, center_shading, shading.read(sample_pixel))) continue;
			const float3 sample = signal.read(sample_pixel).rgb;
			const float spatial_weight = exp(-0.45f * float(x * x + y * y));
			const float normal_weight = pow(max(dot(oct_decode(center_surface.xy), oct_decode(sample_surface.xy)), 0.0f), 32.0f);
			const float depth_weight = exp(-80.0f * abs(sample_surface.z - center_surface.z) / max(center_surface.z, 0.001f));
			const float luminance_weight = exp(-abs(split_luminance(sample) - center_luminance) / (3.0f * standard_deviation + 0.02f));
			const float weight = spatial_weight * normal_weight * depth_weight * luminance_weight;
			sum += sample * weight;
			weight_sum += weight;
		}
	}
	return sum / max(weight_sum, 0.0001f);
}

kernel void reconstruct_split_hybrid(
		constant Parameters &parameters [[buffer(0)]],
		device MaterialDiagnosticAtomic &material_diagnostic [[buffer(1)]],
		depth2d<float, access::read> depth_texture [[texture(0)]],
		texture2d<float, access::read> current_surface [[texture(1)]],
		texture2d<float, access::read> prior_surface [[texture(2)]],
		texture2d<float, access::read> diffuse_signal [[texture(3)]],
		texture2d<float, access::read> specular_signal [[texture(4)]],
		texture2d<float, access::read> diffuse_history_input [[texture(5)]],
		texture2d<float, access::write> diffuse_history_output [[texture(6)]],
		texture2d<float, access::read> specular_history_input [[texture(7)]],
		texture2d<float, access::write> specular_history_output [[texture(8)]],
		texture2d<float, access::read> diffuse_moments_input [[texture(9)]],
		texture2d<float, access::write> diffuse_moments_output [[texture(10)]],
		texture2d<float, access::read> specular_moments_input [[texture(11)]],
		texture2d<float, access::write> specular_moments_output [[texture(12)]],
		texture2d<float, access::write> effect_output [[texture(13)]],
		texture2d<float, access::read> velocity_texture [[texture(14)]],
		texture2d<uint, access::read> current_identity [[texture(15)]],
		texture2d<uint, access::read> prior_identity [[texture(16)]],
		texture2d<float, access::read> current_shading [[texture(17)]],
		texture2d<float, access::read> prior_shading [[texture(18)]],
		texture2d<float, access::write> diffuse_transport_sample_output [[texture(19)]],
		texture2d<float, access::write> specular_transport_sample_output [[texture(20)]],
		uint2 pixel [[thread_position_in_grid]]) {
	if (any(pixel >= parameters.dimensions)) return;
	const float4 surface = current_surface.read(pixel);
	const float depth = depth_texture.read(pixel);
	if (depth <= 0.0f) {
		diffuse_history_output.write(float4(0.0f), pixel);
		specular_history_output.write(float4(0.0f), pixel);
		diffuse_transport_sample_output.write(float4(0.0f), pixel);
		specular_transport_sample_output.write(float4(0.0f), pixel);
		diffuse_moments_output.write(float4(0.0f), pixel);
		specular_moments_output.write(float4(0.0f), pixel);
		effect_output.write(float4(0.0f), pixel);
		return;
	}
	const float3 raw_diffuse = diffuse_signal.read(pixel).rgb;
	const float4 raw_specular_record = specular_signal.read(pixel);
	if (surface.w == 0.0f) {
		// Raster primary depth can be valid while an approximate camera ray misses
		// its matching AS triangle (for example, a test or viewport without the
		// exact projection convention). Preserve this frame's traced transport but
		// fail closed for temporal reuse: there is no material/surface identity with
		// which to validate either split history.
		diffuse_history_output.write(float4(0.0f), pixel);
		specular_history_output.write(float4(0.0f), pixel);
		diffuse_transport_sample_output.write(float4(0.0f), pixel);
		specular_transport_sample_output.write(float4(0.0f), pixel);
		diffuse_moments_output.write(float4(0.0f), pixel);
		specular_moments_output.write(float4(0.0f), pixel);
		effect_output.write(float4(raw_diffuse + raw_specular_record.rgb, 1.0f), pixel);
		stage_probe_add(parameters, material_diagnostic.stage_spatial, raw_diffuse + raw_specular_record.rgb);
		stage_probe_add(parameters, material_diagnostic.stage_temporal_input, float3(0.0f));
		stage_probe_add(parameters, material_diagnostic.stage_temporal_output, raw_diffuse + raw_specular_record.rgb);
		stage_probe_count(parameters, material_diagnostic.stage_temporal_rejected_pixels, 1u);
		return;
	}

	// Flux raster velocity is the primary mapping for animated/skinned/object
	// motion. Camera reprojection remains a validated fallback only when a
	// velocity value is unavailable or non-finite.
	const float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
	const float2 raster_velocity = velocity_texture.read(pixel).xy;
	const bool velocity_valid = all(isfinite(raster_velocity));
	bool reprojection_valid = parameters.history_valid != 0u;
	uint2 previous_pixel = pixel;
	if (reprojection_valid && velocity_valid) {
		const float2 previous_uv = uv + raster_velocity;
		reprojection_valid = all(previous_uv >= 0.0f) && all(previous_uv < 1.0f);
		if (reprojection_valid) previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
	} else if (reprojection_valid) {
		const float2 ndc = uv * 2.0f - 1.0f;
		const float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
		const float3 world_position = (parameters.world_from_view * float4(view_h.xyz / view_h.w, 1.0f)).xyz;
		const float4 previous_clip = parameters.prev_clip_from_world * float4(world_position, 1.0f);
		reprojection_valid = previous_clip.w > 0.0f;
		if (reprojection_valid) {
			const float2 previous_uv = float2(previous_clip.x / previous_clip.w * 0.5f + 0.5f, 0.5f - previous_clip.y / previous_clip.w * 0.5f);
			reprojection_valid = all(previous_uv >= 0.0f) && all(previous_uv < 1.0f);
			if (reprojection_valid) previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
		}
	}
	const float4 old_surface = prior_surface.read(previous_pixel);
	reprojection_valid = reprojection_valid && split_surface_compatible(surface, old_surface, current_identity.read(pixel).xy, prior_identity.read(previous_pixel).xy, current_shading.read(pixel), prior_shading.read(previous_pixel));

	const float4 old_diffuse_moments = diffuse_moments_input.read(previous_pixel);
	const float4 old_specular_moments = specular_moments_input.read(previous_pixel);
	if ((parameters.reconstruction_flags & 16u) != 0u) {
		// MetalFX is the only image-space denoiser in this mode. Maintain only
		// non-image transport confidence: raw luminance moments, a bounded age,
		// and the specular hit-distance needed to reject an incompatible secondary
		// path. No color history is read, blended, filtered, or emitted here.
		const float4 prior_specular_metadata = specular_history_input.read(previous_pixel);
		const float current_specular_distance = raw_specular_record.a;
		const float specular_distance_error = abs(prior_specular_metadata.a - current_specular_distance) / max(max(prior_specular_metadata.a, current_specular_distance), 0.001f);
		const bool specular_metadata_valid = reprojection_valid && isfinite(current_specular_distance) && current_specular_distance >= 0.0f && specular_distance_error < 0.1f;
		const float raw_diffuse_luminance = split_luminance(raw_diffuse);
		const float raw_specular_luminance = split_luminance(raw_specular_record.rgb);
		const float diffuse_weight = reprojection_valid ? 0.875f : 0.0f;
		const float specular_weight = specular_metadata_valid ? 0.80f : 0.0f;
		const float diffuse_mean = mix(raw_diffuse_luminance, old_diffuse_moments.x, diffuse_weight);
		const float diffuse_second = mix(raw_diffuse_luminance * raw_diffuse_luminance, old_diffuse_moments.y, diffuse_weight);
		const float specular_mean = mix(raw_specular_luminance, old_specular_moments.x, specular_weight);
		const float specular_second = mix(raw_specular_luminance * raw_specular_luminance, old_specular_moments.y, specular_weight);
		const float diffuse_confidence = reprojection_valid ? min(max(old_diffuse_moments.w, 0.0f) + 0.125f, 1.0f) : 0.0f;
		const float specular_confidence = specular_metadata_valid ? min(max(old_specular_moments.w, 0.0f) + 0.125f, 1.0f) : 0.0f;
		// Keep image-history textures inert in MetalFX mode. Alpha is explicit
		// sampling metadata, not color transport, and is used solely for the
		// hit-distance validation above on the next frame.
		diffuse_history_output.write(float4(0.0f), pixel);
		specular_history_output.write(float4(0.0f, 0.0f, 0.0f, current_specular_distance), pixel);
		// Trace owns the immutable raw path-sample outputs. This pass can clear
		// them only for an invalid primary; it never filters or rewrites them.
		diffuse_moments_output.write(float4(diffuse_mean, diffuse_second, max(diffuse_second - diffuse_mean * diffuse_mean, 0.0f), diffuse_confidence), pixel);
		specular_moments_output.write(float4(specular_mean, specular_second, max(specular_second - specular_mean * specular_mean, 0.0f), specular_confidence), pixel);
		effect_output.write(float4(raw_diffuse + raw_specular_record.rgb, 1.0f), pixel);
		return;
	}
	const float diffuse_variance = max(old_diffuse_moments.y - old_diffuse_moments.x * old_diffuse_moments.x, 0.0f);
	const float specular_variance = max(old_specular_moments.y - old_specular_moments.x * old_specular_moments.x, 0.0f);
	const float3 filtered_diffuse = split_spatial_filter(diffuse_signal, current_surface, current_identity, current_shading, pixel, parameters.dimensions, diffuse_variance);
	const float3 filtered_specular = split_spatial_filter(specular_signal, current_surface, current_identity, current_shading, pixel, parameters.dimensions, specular_variance);
	const float3 old_diffuse = diffuse_history_input.read(previous_pixel).rgb;
	const float4 old_specular_record = specular_history_input.read(previous_pixel);
	const float4 current_specular_record = specular_signal.read(pixel);
	const float specular_distance_error = abs(old_specular_record.a - current_specular_record.a) / max(max(old_specular_record.a, current_specular_record.a), 0.001f);
	const bool specular_history_valid = reprojection_valid && specular_distance_error < 0.1f;
	stage_probe_add(parameters, material_diagnostic.stage_spatial, filtered_diffuse + filtered_specular);
	stage_probe_add(parameters, material_diagnostic.stage_temporal_input, (reprojection_valid ? old_diffuse : float3(0.0f)) + (specular_history_valid ? old_specular_record.rgb : float3(0.0f)));
	if (reprojection_valid) {
		stage_probe_count(parameters, material_diagnostic.stage_temporal_reused_pixels, 1u);
	} else {
		stage_probe_count(parameters, material_diagnostic.stage_temporal_rejected_pixels, 1u);
	}
	stage_probe_count(parameters, material_diagnostic.stage_temporal_history_samples, reprojection_valid ? 1u : 0u);
	const float diffuse_history_weight = reprojection_valid ? 0.875f / (1.0f + 2.0f * sqrt(diffuse_variance)) : 0.0f;
	const float specular_history_weight = specular_history_valid ? 0.80f / (1.0f + 3.0f * sqrt(specular_variance)) : 0.0f;
	const float3 diffuse_extent = (abs(filtered_diffuse) + 0.02f) * (0.15f + 3.0f * sqrt(diffuse_variance));
	const float3 specular_extent = (abs(filtered_specular) + 0.02f) * (0.20f + 3.0f * sqrt(specular_variance));
	const float3 reconstructed_diffuse = mix(filtered_diffuse, clamp(old_diffuse, filtered_diffuse - diffuse_extent, filtered_diffuse + diffuse_extent), diffuse_history_weight);
	const float3 reconstructed_specular = mix(filtered_specular, clamp(old_specular_record.rgb, filtered_specular - specular_extent, filtered_specular + specular_extent), specular_history_weight);
	stage_probe_add(parameters, material_diagnostic.stage_temporal_output, reconstructed_diffuse + reconstructed_specular);
	const float diffuse_luminance = split_luminance(reconstructed_diffuse);
	const float specular_luminance = split_luminance(reconstructed_specular);
	const float diffuse_mean = mix(diffuse_luminance, old_diffuse_moments.x, diffuse_history_weight);
	const float specular_mean = mix(specular_luminance, old_specular_moments.x, specular_history_weight);
	const float diffuse_second = mix(diffuse_luminance * diffuse_luminance, old_diffuse_moments.y, diffuse_history_weight);
	const float specular_second = mix(specular_luminance * specular_luminance, old_specular_moments.y, specular_history_weight);
	stage_probe_add(parameters, material_diagnostic.stage_temporal_second_moment, float3(diffuse_second + specular_second));
	stage_probe_add(parameters, material_diagnostic.stage_temporal_variance, float3(max(diffuse_second - diffuse_mean * diffuse_mean, 0.0f) + max(specular_second - specular_mean * specular_mean, 0.0f)));
	diffuse_history_output.write(float4(reconstructed_diffuse, 1.0f), pixel);
	specular_history_output.write(float4(reconstructed_specular, current_specular_record.a), pixel);
	diffuse_moments_output.write(float4(diffuse_mean, diffuse_second, max(diffuse_second - diffuse_mean * diffuse_mean, 0.0f), reprojection_valid ? 1.0f : 0.0f), pixel);
	specular_moments_output.write(float4(specular_mean, specular_second, max(specular_second - specular_mean * specular_mean, 0.0f), specular_history_valid ? 1.0f : 0.0f), pixel);
	// The only diffuse/specular composition point. Downstream composition adds
	// this transport once to the raster result or hands it to MetalFX.
	effect_output.write(float4(reconstructed_diffuse + reconstructed_specular, 1.0f), pixel);
}

kernel void composite_hybrid(
	device MaterialDiagnosticAtomic &material_diagnostic [[buffer(0)]],
	constant Parameters &parameters [[buffer(1)]],
    texture2d<float, access::read> effect_texture [[texture(0)]],
    texture2d<float, access::read_write> color_texture [[texture(1)]],
	depth2d<float, access::read> depth_texture [[texture(2)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= uint2(color_texture.get_width(), color_texture.get_height()))) return;
	float4 color = color_texture.read(pixel);
    float4 effect = effect_texture.read(pixel);
	stage_probe_add(parameters, material_diagnostic.stage_composite_base, color.rgb);
	stage_probe_add(parameters, material_diagnostic.stage_composite_input, effect.rgb);
	// Flux raster owns stable emission only. The reconstructed Metal signal owns
	// all direct visibility, Sky/GI, and reflection shading exactly once.
	if (depth_texture.read(pixel) > 0.0f) color.rgb += effect.rgb;
	stage_probe_add(parameters, material_diagnostic.stage_composite_output, color.rgb);
	// PrimarySurfaceV1 temporarily owns color.a for authored AO. Do not leak
	// that internal scalar into the final opaque scene color.
	if (depth_texture.read(pixel) > 0.0f) color.a = 1.0f;
    color_texture.write(color, pixel);
}

kernel void filter_hybrid(
    constant Parameters &parameters [[buffer(0)]],
    depth2d<float, access::read> depth_texture [[texture(0)]],
    texture2d<float, access::read> normal_roughness_texture [[texture(1)]],
    texture2d<float, access::read> effect_texture [[texture(2)]],
    texture2d<float, access::write> filtered_texture [[texture(3)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float center_depth = depth_texture.read(pixel);
    if (center_depth <= 0.0f) {
        filtered_texture.write(effect_texture.read(pixel), pixel);
        return;
    }
    float3 center_normal = normalize(normal_roughness_texture.read(pixel).xyz * 2.0f - 1.0f);
    float4 sum = 0.0f;
    float weight_sum = 0.0f;
    int2 dimensions = int2(parameters.dimensions);
    for (int y = -3; y <= 3; y++) {
        for (int x = -3; x <= 3; x++) {
            int2 sample_position = clamp(int2(pixel) + int2(x, y), int2(0), dimensions - 1);
            uint2 sample_pixel = uint2(sample_position);
            float sample_depth = depth_texture.read(sample_pixel);
            if (sample_depth <= 0.0f) continue;
            float3 sample_normal = normalize(normal_roughness_texture.read(sample_pixel).xyz * 2.0f - 1.0f);
            float spatial_weight = exp(-0.18f * float(x * x + y * y));
            float normal_weight = pow(max(dot(center_normal, sample_normal), 0.0f), 24.0f);
            float relative_depth = abs(sample_depth - center_depth) / max(abs(center_depth), 0.0001f);
            float depth_weight = exp(-relative_depth * 300.0f);
            float weight = spatial_weight * normal_weight * depth_weight;
            sum += effect_texture.read(sample_pixel) * weight;
            weight_sum += weight;
        }
    }
    filtered_texture.write(sum / max(weight_sum, 0.0001f), pixel);
}

kernel void accumulate_hybrid(
    constant Parameters &parameters [[buffer(0)]],
    texture2d<float, access::read> current_effect [[texture(0)]],
    depth2d<float, access::read> current_depth [[texture(1)]],
    texture2d<float, access::read> current_normal [[texture(2)]],
    texture2d<float, access::read> velocity_texture [[texture(3)]],
    texture2d<float, access::read> history_effect [[texture(4)]],
    texture2d<float, access::read> history_depth [[texture(5)]],
    texture2d<float, access::read> history_normal [[texture(6)]],
    texture2d<float, access::write> output_effect [[texture(7)]],
    texture2d<float, access::write> output_depth [[texture(8)]],
    texture2d<float, access::write> output_normal [[texture(9)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (any(pixel >= parameters.dimensions)) return;
    float depth = current_depth.read(pixel);
    float4 normal_roughness = current_normal.read(pixel);
    float4 current = current_effect.read(pixel);
    float4 accumulated = current;
    if (parameters.history_valid != 0u && depth > 0.0f) {
        float2 uv = (float2(pixel) + 0.5f) / float2(parameters.dimensions);
        float2 previous_uv = uv + velocity_texture.read(pixel).xy;
        if (all(previous_uv >= 0.0f) && all(previous_uv < 1.0f)) {
            uint2 previous_pixel = min(uint2(previous_uv * float2(parameters.dimensions)), parameters.dimensions - 1u);
			float2 ndc = uv * 2.0f - 1.0f;
            float4 view_h = parameters.view_from_clip * float4(ndc, depth, 1.0f);
            float3 world_position = (parameters.world_from_view * float4(view_h.xyz / view_h.w, 1.0f)).xyz;
            float4 previous_clip = parameters.prev_clip_from_world * float4(world_position, 1.0f);
            float expected_depth = previous_clip.z / previous_clip.w;
            float old_depth = history_depth.read(previous_pixel).r;
            float3 old_normal = normalize(history_normal.read(previous_pixel).xyz * 2.0f - 1.0f);
            float3 new_normal = normalize(normal_roughness.xyz * 2.0f - 1.0f);
            float depth_error = abs(old_depth - expected_depth) / max(abs(expected_depth), 0.0001f);
            bool valid = previous_clip.w > 0.0f && old_depth > 0.0f && depth_error < 0.02f && dot(old_normal, new_normal) > 0.8f;
            if (valid) {
                float4 old = history_effect.read(previous_pixel);
				float history_samples = clamp(old.a, 0.0f, 31.0f);
				float history_weight = min(history_samples / (history_samples + 1.0f), 0.96875f);
				accumulated.rgb = mix(current.rgb, old.rgb, history_weight);
				accumulated.a = min(history_samples + 1.0f, 32.0f);
            }
        }
    }
    output_effect.write(accumulated, pixel);
    output_depth.write(float4(depth), pixel);
    output_normal.write(normal_roughness, pixel);
}
)";

struct MetalFluxParameters {
	simd::float4x4 world_from_view;
	simd::float4x4 view_from_clip;
	simd::float4x4 clip_from_world;
	simd::float4x4 prev_clip_from_world;
	simd::float4 light_direction_and_reflection_strength;
	simd::float4 directional_light_radiance_enabled;
	simd::float4 ao_distance_strength_roughness_flags;
	simd::float4 contact_visibility_info;
	simd::uint2 dimensions;
	uint32_t shadow_sample_count;
	float directional_light_angular_radius;
	uint32_t gi_sample_count;
	uint32_t frame_index;
	float gi_strength;
	uint32_t history_valid;
	uint32_t emissive_count;
	uint32_t emissive_triangle_count;
	uint32_t punctual_light_count;
	float transport_max_distance;
	simd::float4x4 world_from_radiance;
	simd::float4x4 radiance_from_world;
	simd::float4 environment_info;
	simd::uint2 environment_dimensions;
	simd::uint2 environment_importance_dimensions;
	simd::float4 solar_current_direction_radius;
	simd::float4 solar_previous_direction_transmittance;
	simd::float4 solar_perpendicular_irradiance_enabled;
	simd::uint4 solar_identity;
	simd::uint4 solar_generations;
	simd::uint2 light_distribution_identity;
	simd::uint2 cache_revision;
	simd::uint2 admitted_geometry_generation;
	simd::uint2 visibility_residency_generation;
	simd::uint2 light_distribution_generation;
	simd::uint2 environment_generation;
	uint32_t portal_count;
	uint32_t portal_generation;
	uint32_t adaptive_min_samples;
	uint32_t adaptive_max_samples;
	float adaptive_variance_reference;
	float diffuse_cache_cell_size;
	uint32_t alpha_mask_instance_count = 0;
	uint32_t material_texture_capacity = 0;
	uint32_t geometry_record_count = 0;
	uint32_t raster_primary_surface = 0;
	uint32_t reconstruction_flags = 0;
	uint32_t directional_light_cull_mask = 0xffffffffu;
	uint32_t directional_shadow_caster_mask = 0xffffffffu;
	float directional_shadow_opacity = 1.0f;
	float directional_specular_amount = 1.0f;
	uint32_t directional_flags = 0;
	uint32_t experimental_feature_flags = 0;
	uint32_t sampling_sequence_mode = 0;
	uint32_t sampling_tile_width = 0;
	uint32_t sampling_tile_height = 0;
	uint32_t sampling_tile_depth = 0;
	uint32_t sampling_tile_channels = 0;
	uint32_t bidirectional_caustic_mirror_count = 0;
	uint32_t bidirectional_caustic_source_count = 0;
	float bidirectional_caustic_delta_roughness_threshold = 0.0f;
	uint32_t bidirectional_caustic_flags = 0;
};

// Must match the Metal-side ReGIR ABI above. `ulong4` is used rather than a
// revision hash so a cell cannot survive a coincidental hash match.
struct MetalFluxRegirHeader {
	simd::int4 center_and_cell_size;
	simd::int4 previous_center_and_flags;
	simd::uint4 dimensions_and_blocks;
	simd::ulong4 revisions;
};

struct MetalFluxRegirCell {
	simd::int4 key_valid;
	simd::uint4 source;
	simd::float4 sample;
	simd::float4 metadata;
	simd::ulong4 revisions;
};

struct MetalFluxRegirCandidate {
	uint32_t cell_index;
	uint32_t valid;
	uint32_t padding[2] = {};
	simd::uint4 source;
	simd::float4 sample;
	simd::float4 metadata;
};

struct MetalFluxReusablePathHeader {
	simd::int4 center_and_cell_size;
	simd::int4 previous_center_and_flags;
	simd::uint4 dimensions_and_flags;
	simd::ulong2 geometry_and_residency_revisions;
	simd::ulong2 padding0;
	uint64_t frame = 0;
	uint64_t padding[3] = {};
};

// Verified with the active macOS SDK's simd layout. These assertions mirror
// the live Metal records rather than relying only on vector-size divisibility.
static_assert(sizeof(MetalFluxRegirHeader) == 80, "ReGIR header ABI drifted.");
static_assert(offsetof(MetalFluxRegirHeader, center_and_cell_size) == 0 && offsetof(MetalFluxRegirHeader, dimensions_and_blocks) == 32 && offsetof(MetalFluxRegirHeader, revisions) == 48, "ReGIR header offsets drifted.");
static_assert(sizeof(MetalFluxRegirCell) == 96, "ReGIR cell ABI drifted.");
static_assert(offsetof(MetalFluxRegirCell, key_valid) == 0 && offsetof(MetalFluxRegirCell, sample) == 32 && offsetof(MetalFluxRegirCell, revisions) == 64, "ReGIR cell offsets drifted.");
static_assert(sizeof(MetalFluxRegirCandidate) == 64, "ReGIR candidate ABI drifted.");
static_assert(offsetof(MetalFluxRegirCandidate, source) == 16 && offsetof(MetalFluxRegirCandidate, sample) == 32 && offsetof(MetalFluxRegirCandidate, metadata) == 48, "ReGIR candidate offsets drifted.");
static_assert(sizeof(MetalFluxReusablePathHeader) == 112, "Reusable path header ABI drifted.");
static_assert(offsetof(MetalFluxReusablePathHeader, geometry_and_residency_revisions) == 48 && offsetof(MetalFluxReusablePathHeader, frame) == 80, "Reusable path header offsets drifted.");
static_assert(sizeof(RendererPathTracing::ReusablePathSampleGpuRecord) == 320, "Reusable path record ABI drifted.");
static_assert(offsetof(RendererPathTracing::ReusablePathSampleGpuRecord, secondary_geometry_instance_id) == 0 && offsetof(RendererPathTracing::ReusablePathSampleGpuRecord, incident_radiance_r) == 256 && offsetof(RendererPathTracing::ReusablePathSampleGpuRecord, source_primary_proposal_solid_angle_pdf) == 288, "Reusable path record offsets drifted.");

struct MetalFluxMaterial {
	simd::float4 albedo_metallic;
	simd::float4 emission_roughness;
	simd::float4 uv_scale_offset;
	simd::float4 metallic_texture_channel;
	simd::float4 roughness_texture_channel;
	simd::float4 ao_texture_channel;
	simd::float4 material_factors;
	float albedo_alpha = 1.0f;
	float specular = 0.5f;
	simd::float2 thin_transmission_ior = simd_make_float2(0.0f, 1.5f);
	uint32_t alpha_occupancy_texture_index = 0xffffffffu;
	uint32_t face_flags = 0;
	uint32_t albedo_texture_index = 0xffffffffu;
	uint32_t normal_texture_index = 0xffffffffu;
	uint32_t orm_texture_index = 0xffffffffu;
	uint32_t metallic_texture_index = 0xffffffffu;
	uint32_t roughness_texture_index = 0xffffffffu;
	uint32_t ao_texture_index = 0xffffffffu;
	uint32_t emission_texture_index = 0xffffffffu;
	uint32_t opacity_texture_index = 0xffffffffu;
	uint32_t visibility_mask = 0xffffffffu;
	uint32_t flags = 0;
	uint32_t generation_low = 0;
	uint32_t generation_high = 0;
};

struct MetalFluxGeometry {
	uint64_t vertex_address = 0;
	uint64_t index_address = 0;
	uint64_t attribute_address = 0;
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;
	uint32_t index_type = 0;
	uint32_t position_stride = 0;
	uint32_t normal_offset = 0;
	uint32_t normal_stride = 0;
	uint32_t has_normals = 0;
	uint32_t compressed = 0;
	uint32_t attribute_stride = 0;
	uint32_t uv_offset = 0;
	uint32_t has_uv = 0;
	uint32_t instance_identity_low = 0;
	uint32_t instance_identity_high = 0;
	uint32_t has_tangents = 0;
	simd::float4x4 normal_from_object;
	simd::float4x4 world_from_object;
	simd::float4 position_scale;
	simd::float4 position_offset;
};

// Keep the renderer and MSL constant-buffer layouts locked together. The two
// directional masks intentionally occupy the final aligned scalar slot.
static_assert(sizeof(MetalFluxParameters) == 768, "MSL Parameters ABI drifted.");
static_assert(sizeof(MetalFluxMaterial) == 192, "MSL MaterialRecord ABI drifted.");
static_assert(sizeof(MetalFluxGeometry) == 240, "MSL GeometryRecord ABI drifted.");
static_assert(offsetof(MetalFluxGeometry, has_tangents) == 76, "MSL GeometryRecord tangent availability mapping drifted.");

struct MetalFluxTraceCompactEntry {
	uint32_t pixel_index = 0;
	uint32_t need_mask = 0;
};

struct MetalFluxTraceCompactContext {
	uint32_t enabled = 0;
	uint32_t queue_index = 0;
	uint32_t queue_capacity = 0;
	uint32_t reserved = 0;
};

static_assert(sizeof(MetalFluxTraceCompactEntry) == 8, "Metal Flux compact entry ABI drifted.");
static_assert(sizeof(MetalFluxTraceCompactContext) == 16, "Metal Flux compact context ABI drifted.");

struct MetalFluxEmissive {
	uint32_t instance_id = 0;
	uint32_t triangle_count = 0;
	float selection_pdf = 0.0f;
	float cdf = 0.0f;
	uint32_t source_identity = 0;
	uint32_t padding = 0;
};

static_assert(sizeof(MetalFluxEmissive) == 24, "MSL EmissiveRecord ABI drifted.");

struct MetalFluxEmissiveTriangle {
	uint32_t instance_id = 0;
	uint32_t primitive_id = 0;
	uint32_t instance_identity_low = 0;
	uint32_t instance_identity_high = 0;
	float area = 0.0f;
	float weight = 0.0f;
	float selection_pdf = 0.0f;
	float cdf = 0.0f;
};

static_assert(sizeof(MetalFluxEmissiveTriangle) == 32, "MSL EmissiveTriangleRecord ABI drifted.");

struct MetalFluxBidirectionalCausticMirrorTriangle {
	uint32_t instance_id = 0;
	uint32_t primitive_id = 0;
	uint32_t instance_identity_low = 0;
	uint32_t instance_identity_high = 0;
	uint32_t material_generation_low = 0;
	uint32_t material_generation_high = 0;
	uint32_t visibility_mask = 0;
	uint32_t padding = 0;
};

static_assert(sizeof(MetalFluxBidirectionalCausticMirrorTriangle) == 32, "Metal caustic mirror ABI drifted.");

struct MetalFluxEmissiveTriangleBuildParameters {
	uint32_t emissive_count = 0;
	uint32_t triangle_capacity = 0;
	uint32_t block_count = 0;
	uint32_t padding = 0;
};

struct MetalFluxPunctualLight {
	simd::float4 position_range;
	simd::float4 radiance_attenuation;
	simd::float4 direction_spot_outer;
	simd::float4 area_u_spot_attenuation;
	simd::float4 area_v;
	uint32_t type = 0;
	uint32_t cull_mask = 0xffffffffu;
	uint32_t source_identity = 0;
	uint32_t shadow_caster_mask = 0xffffffffu;
	simd::uint2 stable_identity;
	uint32_t flags = 0;
	float shadow_opacity = 1.0f;
	float specular_amount = 1.0f;
	float indirect_energy = 1.0f;
	uint32_t padding[2] = {};
};

static_assert(sizeof(MetalFluxPunctualLight) == 128, "MSL PunctualLightRecord ABI drifted.");

struct MetalFluxPortal {
	simd::float4 center_weight;
	simd::float4 axis_u;
	simd::float4 axis_v;
};

static_assert(sizeof(MetalFluxPortal) == 48, "MSL PortalRecord ABI drifted.");

struct MetalFluxWork {
	NS::SharedPtr<MTL::ComputePipelineState> trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> trace_classify_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> trace_indirect_finalize_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> shadow_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> alpha_trace_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> alpha_shadow_pipeline;
	NS::SharedPtr<MTL::Function> alpha_intersection_function;
	NS::SharedPtr<MTL::IntersectionFunctionTable> alpha_trace_intersection_table;
	NS::SharedPtr<MTL::IntersectionFunctionTable> alpha_shadow_intersection_table;
	NS::SharedPtr<MTL::ComputePipelineState> filter_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> temporal_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> split_reconstruction_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> composite_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> environment_build_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> environment_reduce_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> environment_diagnostic_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_build_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_block_scan_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_block_prefix_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> emissive_triangle_finalize_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> regir_scroll_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> regir_classify_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> regir_reduce_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> reusable_path_clear_staging_pipeline;
	NS::SharedPtr<MTL::ComputePipelineState> reusable_path_reduce_pipeline;
	NS::SharedPtr<MTL::Buffer> regir_header;
	NS::SharedPtr<MTL::Buffer> regir_input;
	NS::SharedPtr<MTL::Buffer> regir_output;
	NS::SharedPtr<MTL::Buffer> regir_staging;
	uint32_t regir_cell_count = 0;
	uint32_t regir_staging_blocks = 0;
	bool regir_enabled = false;
	NS::SharedPtr<MTL::Buffer> reusable_path_header;
	NS::SharedPtr<MTL::Buffer> reusable_path_previous;
	NS::SharedPtr<MTL::Buffer> reusable_path_next;
	NS::SharedPtr<MTL::Buffer> reusable_path_staging;
	NS::SharedPtr<MTL::Buffer> reusable_path_staging_claims;
	uint32_t reusable_path_cell_count = 0;
	bool reusable_path_enabled = false;
	bool split_reconstruction_enabled = true;
	bool transport_metadata_update_enabled = true;
	Vector<NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor>> blas_descriptors;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas_sources;
	Vector<NS::SharedPtr<MTL::Buffer>> blas_scratch;
	Vector<uint8_t> blas_actions;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas_compaction_query_sources;
	Vector<NS::SharedPtr<MTL::Buffer>> blas_compaction_query_buffers;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas_compaction_copy_sources;
	Vector<NS::SharedPtr<MTL::AccelerationStructure>> blas_compaction_copy_destinations;
	Vector<NS::SharedPtr<MTL::Buffer>> decode_transforms;
	NS::SharedPtr<NS::Array> blas_array;
	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;
	NS::SharedPtr<MTL::Buffer> tlas_scratch;
	NS::SharedPtr<MTL::Buffer> tlas_instances;
	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> opaque_tlas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> opaque_tlas;
	NS::SharedPtr<MTL::Buffer> opaque_tlas_scratch;
	NS::SharedPtr<MTL::Buffer> opaque_tlas_instances;
	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> alpha_tlas_descriptor;
	NS::SharedPtr<MTL::AccelerationStructure> alpha_tlas;
	NS::SharedPtr<MTL::Buffer> alpha_tlas_scratch;
	NS::SharedPtr<MTL::Buffer> alpha_tlas_instances;
	NS::SharedPtr<MTL::Buffer> materials;
	NS::SharedPtr<MTL::Buffer> geometries;
	NS::SharedPtr<MTL::Buffer> emissives;
	NS::SharedPtr<MTL::Buffer> emissive_triangles;
	NS::SharedPtr<MTL::Buffer> emissive_triangle_block_sums;
	NS::SharedPtr<MTL::Buffer> emissive_triangle_total;
	NS::SharedPtr<MTL::Buffer> bidirectional_caustic_mirror_triangles;
	MetalFluxEmissiveTriangleBuildParameters emissive_triangle_build_parameters;
	NS::SharedPtr<MTL::Buffer> punctual_lights;
	NS::SharedPtr<MTL::Buffer> portals;
	NS::SharedPtr<MTL::Buffer> material_texture_argument_buffer;
	NS::SharedPtr<MTL::Buffer> alpha_material_texture_argument_buffer;
	std::shared_ptr<MetalFluxMaterialDiagnosticCapture> material_diagnostic;
	std::shared_ptr<std::atomic<uint64_t>> residency_completion;
	uint64_t residency_submission_token = 0;
	NS::SharedPtr<MTL::SamplerState> albedo_sampler;
	NS::SharedPtr<MTL::SamplerState> environment_sampler;
	MTL::Texture *environment_radiance = nullptr;
	NS::SharedPtr<MTL::Texture> environment_importance;
	NS::SharedPtr<MTL::Texture> stbn_scalar_volume;
	MTL::Texture *full_environment_radiance = nullptr;
	std::shared_ptr<MetalFluxEnvironmentDiagnosticCapture> environment_diagnostic;
	bool environment_rebuild = false;
	bool tlas_build = false;
	bool tlas_refit = false;
	bool opaque_tlas_build = false;
	bool opaque_tlas_refit = false;
	bool alpha_tlas_build = false;
	bool alpha_tlas_refit = false;
	bool split_alpha_domains_exact = false;
	Vector<MTL::Buffer *> vertex_buffers;
	Vector<MTL::Buffer *> index_buffers;
	Vector<MTL::Buffer *> attribute_buffers;
	Vector<MTL::Texture *> material_textures;
	bool bindless_material_textures = false;
	bool alpha_intersection_enabled = false;
	Vector<MTL::Texture *> color;
	Vector<MTL::Texture *> depth;
	Vector<MTL::Texture *> normal_roughness;
	Vector<MTL::Texture *> primary_material;
	Vector<MTL::Texture *> primary_identity;
	Vector<MTL::Texture *> primary_geometry;
	Vector<MTL::Texture *> primary_flags;
	Vector<MTL::Texture *> effect_output;
	Vector<MTL::Texture *> filtered_output;
	Vector<MTL::Texture *> velocity;
	Vector<MTL::Texture *> history_input;
	Vector<MTL::Texture *> history_output;
	Vector<MTL::Texture *> depth_history_input;
	Vector<MTL::Texture *> depth_history_output;
	Vector<MTL::Texture *> normal_history_input;
	Vector<MTL::Texture *> normal_history_output;
	Vector<MTL::Texture *> guide_normal;
	Vector<MTL::Texture *> guide_diffuse;
	Vector<MTL::Texture *> guide_specular;
	Vector<MTL::Texture *> guide_roughness;
	Vector<MTL::Texture *> guide_denoise_strength;
	Vector<MTL::Texture *> guide_reactive;
	Vector<MTL::Texture *> guide_specular_distance;
	Vector<MTL::Texture *> guide_transparency;
	Vector<NS::SharedPtr<MTL::Buffer>> trace_queues;
	Vector<NS::SharedPtr<MTL::Buffer>> trace_queue_counts;
	Vector<NS::SharedPtr<MTL::Buffer>> trace_indirect_arguments;
	Vector<uint32_t> trace_queue_capacity;
	Vector<uint8_t> trace_compaction_view_active;
	Vector<MTL::Texture *> reservoir_input;
	Vector<MTL::Texture *> reservoir_output;
	Vector<MTL::Texture *> reservoir_surface_input;
	Vector<MTL::Texture *> reservoir_surface_output;
	Vector<MTL::Texture *> reservoir_metadata_input;
	Vector<MTL::Texture *> reservoir_metadata_output;
	Vector<MTL::Texture *> reservoir_sample_input;
	Vector<MTL::Texture *> reservoir_sample_output;
	Vector<MTL::Texture *> reservoir_primary_identity_input;
	Vector<MTL::Texture *> reservoir_primary_identity_output;
	Vector<MTL::Texture *> primary_shading_input;
	Vector<MTL::Texture *> primary_shading_output;
	Vector<MTL::Texture *> primary_world_position_output;
	Vector<MTL::Texture *> diffuse_history_input;
	Vector<MTL::Texture *> diffuse_history_output;
	Vector<MTL::Texture *> specular_history_input;
	Vector<MTL::Texture *> specular_history_output;
	Vector<MTL::Texture *> diffuse_transport_sample_input;
	Vector<MTL::Texture *> diffuse_transport_sample_output;
	Vector<MTL::Texture *> specular_transport_sample_input;
	Vector<MTL::Texture *> specular_transport_sample_output;
	Vector<MTL::Texture *> diffuse_moments_input;
	Vector<MTL::Texture *> diffuse_moments_output;
	Vector<MTL::Texture *> specular_moments_input;
	Vector<MTL::Texture *> specular_moments_output;
	Vector<MTL::Texture *> split_diffuse;
	Vector<MTL::Texture *> split_specular;
	Vector<NS::SharedPtr<MTL::Texture>> split_diffuse_owned;
	Vector<NS::SharedPtr<MTL::Texture>> split_specular_owned;
	Vector<MetalFluxParameters> parameters;
	std::shared_ptr<MetalFluxTimingCapture> timing;
	bool shadow_only = false;
	bool temporal_enabled = false;
	bool metalfx_denoiser = false;
	bool trace_compaction_active = false;
	bool trace_compaction_fallback = false;
};

static simd::float4x4 _metal_matrix(const Projection &p_matrix) {
	simd::float4x4 matrix;
	for (uint32_t column = 0; column < 4; column++) {
		matrix.columns[column] = simd_make_float4(p_matrix.columns[column].x, p_matrix.columns[column].y, p_matrix.columns[column].z, p_matrix.columns[column].w);
	}
	return matrix;
}

static simd::float4x4 _metal_matrix(const Transform3D &p_transform) {
	const Vector3 x = p_transform.basis.get_column(0);
	const Vector3 y = p_transform.basis.get_column(1);
	const Vector3 z = p_transform.basis.get_column(2);
	return simd::float4x4(simd_make_float4(x.x, x.y, x.z, 0.0f),
			simd_make_float4(y.x, y.y, y.z, 0.0f),
			simd_make_float4(z.x, z.y, z.z, 0.0f),
			simd_make_float4(p_transform.origin.x, p_transform.origin.y, p_transform.origin.z, 1.0f));
}

static NS::SharedPtr<MTL::Texture> _make_transport_texture(MTL::Device *p_device, uint32_t p_width, uint32_t p_height, MTL::PixelFormat p_format) {
	NS::SharedPtr<MTL::TextureDescriptor> descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
	descriptor->setTextureType(MTL::TextureType2D);
	descriptor->setPixelFormat(p_format);
	descriptor->setWidth(p_width);
	descriptor->setHeight(p_height);
	descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
	descriptor->setStorageMode(MTL::StorageModePrivate);
	return NS::TransferPtr(p_device->newTexture(descriptor.get()));
}

static uint64_t _mix_transport_identity(uint64_t p_hash, uint64_t p_value) {
	p_hash ^= p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2);
	return p_hash;
}

static uint32_t _transport_float_bits(float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

static uint64_t _transport_residency_key_identity(const RendererPathTracing::HybridResidencyResourceKey &p_key) {
	uint64_t identity = _mix_transport_identity(0x8d58ac26afe12e47ULL, p_key.stable_id);
	identity = _mix_transport_identity(identity, p_key.generation);
	identity = _mix_transport_identity(identity, uint64_t(p_key.kind));
	identity = _mix_transport_identity(identity, uint64_t(p_key.texture_channel));
	return _mix_transport_identity(identity, p_key.mip_tier);
}

static uint64_t _transport_set_identity(Vector<uint64_t> p_entries, uint64_t p_seed) {
	p_entries.sort();
	uint64_t identity = _mix_transport_identity(p_seed, p_entries.size());
	for (uint64_t entry : p_entries) {
		identity = _mix_transport_identity(identity, entry);
	}
	return identity;
}

// Importance is a proposal distribution, not the radiance representation.
// Keep full-resolution Sky lookup while constructing a bounded residual map.
// Normalized UV sampling and a finite nonzero floor retain support everywhere.
static Vector2i _environment_importance_resolution(uint32_t p_width, uint32_t p_height) {
	if (p_width == 0 || p_height == 0) {
		return Vector2i(1, 1);
	}
	const uint32_t maximum_axis = 256;
	const uint32_t source_axis = MAX(p_width, p_height);
	if (source_axis <= maximum_axis) {
		return Vector2i(p_width, p_height);
	}
	const double scale = double(maximum_axis) / double(source_axis);
	return Vector2i(MAX(1, int(Math::round(double(p_width) * scale))), MAX(1, int(Math::round(double(p_height) * scale))));
}

static void _hybrid_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, MetalFluxWork *p_work) {
	RenderingDeviceDriverMetal *metal_driver = static_cast<RenderingDeviceDriverMetal *>(p_driver);
	MDCommandBufferBase *command = reinterpret_cast<MDCommandBufferBase *>(p_command_buffer.id);
	command->end();
	MTL3::MDCommandBuffer *metal_command = static_cast<MTL3::MDCommandBuffer *>(command);
	MTL::CommandBuffer *command_buffer = metal_command->get_command_buffer();
	if (p_work->timing) {
		metal_driver->get_device()->sampleTimestamps(&p_work->timing->cpu_begin, &p_work->timing->gpu_begin);
	}
	auto acceleration_encoder = [&](uint32_t p_begin, uint32_t p_end) {
		if (!p_work->timing) {
			return command_buffer->accelerationStructureCommandEncoder();
		}
		MTL::AccelerationStructurePassDescriptor *descriptor = MTL::AccelerationStructurePassDescriptor::accelerationStructurePassDescriptor();
		MTL::AccelerationStructurePassSampleBufferAttachmentDescriptor *attachment = descriptor->sampleBufferAttachments()->object(0);
		attachment->setSampleBuffer(p_work->timing->samples.get());
		attachment->setStartOfEncoderSampleIndex(p_begin);
		attachment->setEndOfEncoderSampleIndex(p_end);
		return command_buffer->accelerationStructureCommandEncoder(descriptor);
	};
	bool has_blas_work = false;
	for (uint8_t action : p_work->blas_actions) {
		has_blas_work |= action != 0;
	}
	if (has_blas_work) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = acceleration_encoder(0, 1);
		for (uint32_t index = 0; index < p_work->blas.size(); index++) {
			if (p_work->blas_actions[index] == 1) {
				as_encoder->buildAccelerationStructure(p_work->blas[index].get(), p_work->blas_descriptors[index].get(), p_work->blas_scratch[index].get(), 0);
			} else if (p_work->blas_actions[index] == 2) {
				as_encoder->refitAccelerationStructure(p_work->blas_sources[index].get(), p_work->blas_descriptors[index].get(), p_work->blas[index].get(), p_work->blas_scratch[index].get(), 0, MTL::AccelerationStructureRefitOptionVertexData);
			}
		}
		for (uint32_t index = 0; index < p_work->blas_compaction_query_sources.size(); index++) {
			as_encoder->writeCompactedAccelerationStructureSize(p_work->blas_compaction_query_sources[index].get(), p_work->blas_compaction_query_buffers[index].get(), 0, MTL::DataTypeULong);
		}
		as_encoder->endEncoding();
	}
	if (!p_work->blas_compaction_copy_sources.is_empty()) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = command_buffer->accelerationStructureCommandEncoder();
		for (uint32_t index = 0; index < p_work->blas_compaction_copy_sources.size(); index++) {
			as_encoder->copyAndCompactAccelerationStructure(p_work->blas_compaction_copy_sources[index].get(), p_work->blas_compaction_copy_destinations[index].get());
		}
		as_encoder->endEncoding();
	}
	if (p_work->tlas_build) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = acceleration_encoder(2, 3);
		as_encoder->buildAccelerationStructure(p_work->tlas.get(), p_work->tlas_descriptor.get(), p_work->tlas_scratch.get(), 0);
		as_encoder->endEncoding();
	} else if (p_work->tlas_refit) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = acceleration_encoder(2, 3);
		// Instance descriptor transforms are part of the TLAS descriptor itself;
		// vertex/per-primitive refit flags apply only to primitive AS geometry.
		as_encoder->refitAccelerationStructure(p_work->tlas.get(), p_work->tlas_descriptor.get(), p_work->tlas.get(), p_work->tlas_scratch.get(), 0, static_cast<MTL::AccelerationStructureRefitOptions>(0));
		as_encoder->endEncoding();
	}
	if (p_work->opaque_tlas_build) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = command_buffer->accelerationStructureCommandEncoder();
		as_encoder->buildAccelerationStructure(p_work->opaque_tlas.get(), p_work->opaque_tlas_descriptor.get(), p_work->opaque_tlas_scratch.get(), 0);
		as_encoder->endEncoding();
	} else if (p_work->opaque_tlas_refit) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = command_buffer->accelerationStructureCommandEncoder();
		as_encoder->refitAccelerationStructure(p_work->opaque_tlas.get(), p_work->opaque_tlas_descriptor.get(), p_work->opaque_tlas.get(), p_work->opaque_tlas_scratch.get(), 0, static_cast<MTL::AccelerationStructureRefitOptions>(0));
		as_encoder->endEncoding();
	}
	if (p_work->alpha_tlas_build) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = command_buffer->accelerationStructureCommandEncoder();
		as_encoder->buildAccelerationStructure(p_work->alpha_tlas.get(), p_work->alpha_tlas_descriptor.get(), p_work->alpha_tlas_scratch.get(), 0);
		as_encoder->endEncoding();
	} else if (p_work->alpha_tlas_refit) {
		MTL::AccelerationStructureCommandEncoder *as_encoder = command_buffer->accelerationStructureCommandEncoder();
		as_encoder->refitAccelerationStructure(p_work->alpha_tlas.get(), p_work->alpha_tlas_descriptor.get(), p_work->alpha_tlas.get(), p_work->alpha_tlas_scratch.get(), 0, static_cast<MTL::AccelerationStructureRefitOptions>(0));
		as_encoder->endEncoding();
	}
	// Exact emissive-triangle distribution: enumerate transformed triangles,
	// scan bounded block sums, then normalize. Separate encoders provide the
	// required visibility ordering before any trace kernel reads the table.
	if (p_work->emissive_triangle_build_parameters.triangle_capacity > 0u) {
		auto declare_emissive_geometry_resources = [&](MTL::ComputeCommandEncoder *p_encoder) {
			for (MTL::Buffer *buffer : p_work->vertex_buffers) p_encoder->useResource(buffer, MTL::ResourceUsageRead);
			for (MTL::Buffer *buffer : p_work->index_buffers) if (buffer) p_encoder->useResource(buffer, MTL::ResourceUsageRead);
		};
		MTL::ComputeCommandEncoder *build = command_buffer->computeCommandEncoder();
		build->setLabel(NS::String::string("emissive_triangle_build", NS::UTF8StringEncoding));
		build->setComputePipelineState(p_work->emissive_triangle_build_pipeline.get());
		build->setBuffer(p_work->geometries.get(), 0, 0);
		build->setBuffer(p_work->materials.get(), 0, 1);
		build->setBytes(&p_work->emissive_triangle_build_parameters, sizeof(MetalFluxEmissiveTriangleBuildParameters), 2);
		build->setBuffer(p_work->emissive_triangles.get(), 0, 3);
		build->setBuffer(p_work->emissives.get(), 0, 4);
		declare_emissive_geometry_resources(build);
		build->useResource(p_work->geometries.get(), MTL::ResourceUsageRead);
		build->useResource(p_work->materials.get(), MTL::ResourceUsageRead);
		build->useResource(p_work->emissives.get(), MTL::ResourceUsageRead);
		build->useResource(p_work->emissive_triangles.get(), MTL::ResourceUsageWrite);
		build->dispatchThreads(MTL::Size(p_work->emissive_triangle_build_parameters.triangle_capacity, 1, 1), MTL::Size(64, 1, 1));
		build->endEncoding();
		MTL::ComputeCommandEncoder *block_scan = command_buffer->computeCommandEncoder();
		block_scan->setLabel(NS::String::string("emissive_triangle_block_scan", NS::UTF8StringEncoding));
		block_scan->setComputePipelineState(p_work->emissive_triangle_block_scan_pipeline.get());
		block_scan->setBuffer(p_work->emissive_triangles.get(), 0, 0);
		block_scan->setBuffer(p_work->emissive_triangle_block_sums.get(), 0, 1);
		block_scan->setBytes(&p_work->emissive_triangle_build_parameters, sizeof(MetalFluxEmissiveTriangleBuildParameters), 2);
		block_scan->useResource(p_work->emissive_triangles.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		block_scan->useResource(p_work->emissive_triangle_block_sums.get(), MTL::ResourceUsageWrite);
		block_scan->dispatchThreads(MTL::Size(p_work->emissive_triangle_build_parameters.triangle_capacity, 1, 1), MTL::Size(64, 1, 1));
		block_scan->endEncoding();
		MTL::ComputeCommandEncoder *block_prefix = command_buffer->computeCommandEncoder();
		block_prefix->setLabel(NS::String::string("emissive_triangle_block_prefix", NS::UTF8StringEncoding));
		block_prefix->setComputePipelineState(p_work->emissive_triangle_block_prefix_pipeline.get());
		block_prefix->setBuffer(p_work->emissive_triangle_block_sums.get(), 0, 0);
		block_prefix->setBuffer(p_work->emissive_triangle_total.get(), 0, 1);
		block_prefix->setBytes(&p_work->emissive_triangle_build_parameters, sizeof(MetalFluxEmissiveTriangleBuildParameters), 2);
		block_prefix->useResource(p_work->emissive_triangle_block_sums.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		block_prefix->useResource(p_work->emissive_triangle_total.get(), MTL::ResourceUsageWrite);
		block_prefix->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
		block_prefix->endEncoding();
		MTL::ComputeCommandEncoder *finalize = command_buffer->computeCommandEncoder();
		finalize->setLabel(NS::String::string("emissive_triangle_finalize", NS::UTF8StringEncoding));
		finalize->setComputePipelineState(p_work->emissive_triangle_finalize_pipeline.get());
		finalize->setBuffer(p_work->emissive_triangles.get(), 0, 0);
		finalize->setBuffer(p_work->emissive_triangle_block_sums.get(), 0, 1);
		finalize->setBuffer(p_work->emissive_triangle_total.get(), 0, 2);
		finalize->setBytes(&p_work->emissive_triangle_build_parameters, sizeof(MetalFluxEmissiveTriangleBuildParameters), 3);
		finalize->useResource(p_work->emissive_triangles.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		finalize->useResource(p_work->emissive_triangle_block_sums.get(), MTL::ResourceUsageRead);
		finalize->useResource(p_work->emissive_triangle_total.get(), MTL::ResourceUsageRead);
		finalize->dispatchThreads(MTL::Size(p_work->emissive_triangle_build_parameters.triangle_capacity, 1, 1), MTL::Size(64, 1, 1));
		finalize->endEncoding();
	}
	if (p_work->environment_rebuild && p_work->environment_radiance && p_work->environment_importance && !p_work->parameters.is_empty()) {
		MTL::ComputeCommandEncoder *build = command_buffer->computeCommandEncoder();
		build->setLabel(NS::String::string("environment_importance_build", NS::UTF8StringEncoding));
		build->setComputePipelineState(p_work->environment_build_pipeline.get());
		build->setBytes(&p_work->parameters[0], sizeof(MetalFluxParameters), 0);
		build->setBuffer(p_work->environment_diagnostic->values.get(), 0, 1);
		build->setTexture(p_work->environment_radiance, 0);
		build->setTexture(p_work->environment_importance.get(), 1);
		build->setSamplerState(p_work->environment_sampler.get(), 0);
		build->useResource(p_work->environment_radiance, MTL::ResourceUsageRead);
		build->useResource(p_work->environment_importance.get(), MTL::ResourceUsageWrite);
		build->useResource(p_work->environment_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		build->dispatchThreads(MTL::Size(p_work->parameters[0].environment_importance_dimensions.x, p_work->parameters[0].environment_importance_dimensions.y, 1), MTL::Size(8, 8, 1));
		build->endEncoding();
		for (uint32_t mip = 1; mip < uint32_t(p_work->parameters[0].environment_info.z); mip++) {
			MetalFluxParameters reduce_parameters = p_work->parameters[0];
			reduce_parameters.frame_index = mip;
			MTL::ComputeCommandEncoder *reduce = command_buffer->computeCommandEncoder();
			reduce->setLabel(NS::String::string("environment_importance_build", NS::UTF8StringEncoding));
			reduce->setComputePipelineState(p_work->environment_reduce_pipeline.get());
			reduce->setBytes(&reduce_parameters, sizeof(MetalFluxParameters), 0);
			reduce->setTexture(p_work->environment_importance.get(), 0);
			reduce->useResource(p_work->environment_importance.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
			const uint32_t width = MAX(1u, p_work->parameters[0].environment_importance_dimensions.x >> mip);
			const uint32_t height = MAX(1u, p_work->parameters[0].environment_importance_dimensions.y >> mip);
			reduce->dispatchThreads(MTL::Size(width, height, 1), MTL::Size(8, 8, 1));
			reduce->endEncoding();
		}
		MTL::ComputeCommandEncoder *diagnostic = command_buffer->computeCommandEncoder();
		diagnostic->setLabel(NS::String::string("environment_importance_diagnostic", NS::UTF8StringEncoding));
		diagnostic->setComputePipelineState(p_work->environment_diagnostic_pipeline.get());
		diagnostic->setBytes(&p_work->parameters[0], sizeof(MetalFluxParameters), 0);
		diagnostic->setBuffer(p_work->environment_diagnostic->values.get(), 0, 1);
		diagnostic->setTexture(p_work->environment_radiance, 0);
		diagnostic->setTexture(p_work->environment_importance.get(), 1);
		diagnostic->setSamplerState(p_work->environment_sampler.get(), 0);
		diagnostic->useResource(p_work->environment_radiance, MTL::ResourceUsageRead);
		diagnostic->useResource(p_work->environment_importance.get(), MTL::ResourceUsageRead);
		diagnostic->useResource(p_work->environment_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		diagnostic->dispatchThreads(MTL::Size(p_work->parameters[0].environment_dimensions.x, p_work->parameters[0].environment_dimensions.y, 1), MTL::Size(8, 8, 1));
		 diagnostic->endEncoding();
	}
	if (p_work->regir_enabled) {
		// Read the old grid and write a separately allocated scrolled grid before
		// tracing. There is no in-place grid mutation and no view-history alias.
		MTL::ComputeCommandEncoder *scroll = command_buffer->computeCommandEncoder();
		scroll->setLabel(NS::String::string("regir_scroll", NS::UTF8StringEncoding));
		scroll->setComputePipelineState(p_work->regir_scroll_pipeline.get());
		scroll->setBuffer(p_work->regir_header.get(), 0, 0);
		scroll->setBuffer(p_work->regir_input.get(), 0, 1);
		scroll->setBuffer(p_work->regir_output.get(), 0, 2);
		scroll->useResource(p_work->regir_header.get(), MTL::ResourceUsageRead);
		scroll->useResource(p_work->regir_input.get(), MTL::ResourceUsageRead);
		scroll->useResource(p_work->regir_output.get(), MTL::ResourceUsageWrite);
		scroll->dispatchThreads(MTL::Size(p_work->regir_cell_count, 1, 1), MTL::Size(32, 1, 1));
		scroll->endEncoding();
	}
	if (p_work->reusable_path_enabled) {
		command->retain_resource(p_work->reusable_path_clear_staging_pipeline.get());
		command->retain_resource(p_work->reusable_path_reduce_pipeline.get());
		command->retain_resource(p_work->reusable_path_header.get());
		command->retain_resource(p_work->reusable_path_previous.get());
		command->retain_resource(p_work->reusable_path_next.get());
		command->retain_resource(p_work->reusable_path_staging.get());
		command->retain_resource(p_work->reusable_path_staging_claims.get());
	}
	if (p_work->reusable_path_enabled) {
		// Staging is cleared before trace. Trace has one deterministic world-cell
		// owner and writes only staging; it never mutates the persistent cache.
		MTL::ComputeCommandEncoder *clear = command_buffer->computeCommandEncoder();
		clear->setLabel(NS::String::string("reusable_path_clear_staging", NS::UTF8StringEncoding));
		clear->setComputePipelineState(p_work->reusable_path_clear_staging_pipeline.get());
		clear->setBuffer(p_work->reusable_path_staging.get(), 0, 0);
		clear->setBuffer(p_work->reusable_path_staging_claims.get(), 0, 1);
		clear->useResource(p_work->reusable_path_staging.get(), MTL::ResourceUsageWrite);
		clear->useResource(p_work->reusable_path_staging_claims.get(), MTL::ResourceUsageWrite);
		clear->dispatchThreads(MTL::Size(p_work->reusable_path_cell_count, 1, 1), MTL::Size(32, 1, 1));
		clear->endEncoding();
	}
	for (uint32_t view = 0; view < p_work->color.size(); view++) {
		// Ten samples are reserved for the five ordered compact queues, followed
		// by reconstruction, the (external) MetalFX temporal slot, and composition.
		const uint32_t timing_base = 4 + view * 16;
		auto compute_encoder = [&](uint32_t p_begin, uint32_t p_end) {
			if (!p_work->timing) {
				return command_buffer->computeCommandEncoder();
			}
			MTL::ComputePassDescriptor *descriptor = MTL::ComputePassDescriptor::computePassDescriptor();
			MTL::ComputePassSampleBufferAttachmentDescriptor *attachment = descriptor->sampleBufferAttachments()->object(0);
			attachment->setSampleBuffer(p_work->timing->samples.get());
			attachment->setStartOfEncoderSampleIndex(p_begin);
			attachment->setEndOfEncoderSampleIndex(p_end);
			return command_buffer->computeCommandEncoder(descriptor);
		};
		if (!p_work->shadow_only && view < uint32_t(p_work->trace_compaction_view_active.size()) && p_work->trace_compaction_view_active[view] != 0u) {
			// Classification is intentionally full-screen: it owns inactive/sky
			// clearing and emits one complete entry into one exclusive queue.
			MTL::ComputeCommandEncoder *classify = command_buffer->computeCommandEncoder();
			classify->setLabel(NS::String::string("trace_classify", NS::UTF8StringEncoding));
			classify->setComputePipelineState(p_work->trace_classify_pipeline.get());
			classify->setBytes(&p_work->parameters[view], sizeof(MetalFluxParameters), 0);
			classify->setBuffer(p_work->material_diagnostic->values.get(), 0, 1);
			classify->setBuffer(p_work->trace_queues[view].get(), 0, 2);
			classify->setBuffer(p_work->trace_queue_counts[view].get(), 0, 3);
			classify->setTexture(p_work->depth[view], 0);
			classify->setTexture(p_work->normal_roughness[view], 1);
			classify->setTexture(p_work->primary_material[view] ? p_work->primary_material[view] : p_work->color[view], 2);
			classify->setTexture(p_work->primary_geometry[view] ? p_work->primary_geometry[view] : p_work->normal_roughness[view], 3);
			classify->setTexture(p_work->primary_flags[view] ? p_work->primary_flags[view] : p_work->reservoir_output[view], 4);
			classify->setTexture(p_work->effect_output[view], 5);
			classify->setTexture(p_work->guide_normal[view], 6);
			classify->setTexture(p_work->guide_diffuse[view], 7);
			classify->setTexture(p_work->guide_specular[view], 8);
			classify->setTexture(p_work->guide_roughness[view], 9);
			classify->setTexture(p_work->guide_denoise_strength[view], 10);
			classify->setTexture(p_work->guide_reactive[view], 11);
			classify->setTexture(p_work->guide_specular_distance[view], 12);
			classify->setTexture(p_work->guide_transparency[view], 13);
			classify->setTexture(p_work->reservoir_output[view], 14);
			classify->setTexture(p_work->reservoir_surface_output[view], 15);
			classify->setTexture(p_work->reservoir_metadata_output[view], 16);
			classify->setTexture(p_work->reservoir_sample_output[view], 17);
			classify->setTexture(p_work->reservoir_primary_identity_output[view], 18);
			classify->setTexture(p_work->primary_world_position_output[view], 19);
			classify->setTexture(p_work->split_diffuse[view], 20);
			classify->setTexture(p_work->split_specular[view], 21);
			classify->setTexture(p_work->diffuse_history_output[view], 22);
			classify->setTexture(p_work->specular_history_output[view], 23);
			classify->setTexture(p_work->diffuse_moments_output[view], 24);
			classify->setTexture(p_work->specular_moments_output[view], 25);
			classify->setTexture(p_work->diffuse_moments_input[view], 26);
			classify->setTexture(p_work->specular_moments_input[view], 27);
			classify->setTexture(p_work->velocity[view], 28);
			classify->setTexture(p_work->primary_shading_output[view], 29);
			classify->setTexture(p_work->primary_identity[view] ? p_work->primary_identity[view] : p_work->reservoir_primary_identity_input[view], 30);
			classify->setTexture(p_work->reservoir_surface_input[view], 31);
			classify->setTexture(p_work->reservoir_primary_identity_input[view], 32);
			classify->setTexture(p_work->primary_shading_input[view], 33);
			classify->setTexture(p_work->diffuse_transport_sample_input[view], 34);
			classify->setTexture(p_work->specular_transport_sample_input[view], 35);
			classify->useResource(p_work->trace_queues[view].get(), MTL::ResourceUsageWrite);
			classify->useResource(p_work->trace_queue_counts[view].get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
			classify->useResource(p_work->material_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
			classify->useResource(p_work->depth[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->normal_roughness[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->effect_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->reservoir_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->reservoir_surface_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->reservoir_metadata_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->reservoir_sample_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->reservoir_primary_identity_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->primary_world_position_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->split_diffuse[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->split_specular[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->diffuse_history_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->specular_history_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->diffuse_moments_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->specular_moments_output[view], MTL::ResourceUsageWrite);
			classify->useResource(p_work->diffuse_moments_input[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->specular_moments_input[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->velocity[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->primary_shading_output[view], MTL::ResourceUsageWrite);
			if (p_work->primary_identity[view]) classify->useResource(p_work->primary_identity[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->reservoir_surface_input[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->reservoir_primary_identity_input[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->primary_shading_input[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->diffuse_transport_sample_input[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->specular_transport_sample_input[view], MTL::ResourceUsageRead);
			classify->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
			classify->endEncoding();

			MTL::ComputeCommandEncoder *finalize = command_buffer->computeCommandEncoder();
			finalize->setLabel(NS::String::string("trace_indirect_finalize", NS::UTF8StringEncoding));
			finalize->setComputePipelineState(p_work->trace_indirect_finalize_pipeline.get());
			finalize->setBuffer(p_work->trace_queue_counts[view].get(), 0, 0);
			finalize->setBuffer(p_work->trace_indirect_arguments[view].get(), 0, 1);
			finalize->setBuffer(p_work->material_diagnostic->values.get(), 0, 2);
			finalize->useResource(p_work->trace_queue_counts[view].get(), MTL::ResourceUsageRead);
			finalize->useResource(p_work->trace_indirect_arguments[view].get(), MTL::ResourceUsageWrite);
			finalize->useResource(p_work->material_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
			finalize->dispatchThreads(MTL::Size(5, 1, 1), MTL::Size(5, 1, 1));
			finalize->endEncoding();
		}
		const bool compact_view = !p_work->shadow_only && view < uint32_t(p_work->trace_compaction_view_active.size()) && p_work->trace_compaction_view_active[view] != 0u;
		MetalFluxTraceCompactContext compact_context = {};
		compact_context.enabled = compact_view ? 1u : 0u;
		compact_context.queue_capacity = compact_view ? p_work->trace_queue_capacity[view] : 0u;
		auto configure_trace_encoder = [&](MTL::ComputeCommandEncoder *trace) {
		trace->setComputePipelineState(p_work->alpha_intersection_enabled ? (p_work->shadow_only ? p_work->alpha_shadow_pipeline.get() : p_work->alpha_trace_pipeline.get()) : (p_work->shadow_only ? p_work->shadow_pipeline.get() : p_work->trace_pipeline.get()));
		trace->setAccelerationStructure(p_work->tlas.get(), 0);
		trace->setAccelerationStructure(p_work->opaque_tlas.get(), 17);
		trace->useResource(p_work->opaque_tlas.get(), MTL::ResourceUsageRead);
		trace->setAccelerationStructure(p_work->alpha_tlas ? p_work->alpha_tlas.get() : p_work->tlas.get(), 22);
		trace->useResource(p_work->alpha_tlas ? p_work->alpha_tlas.get() : p_work->tlas.get(), MTL::ResourceUsageRead);
		// The TLAS indirectly references primitive acceleration structures. Declare
		// both levels resident for every trace rather than relying on a build in an
		// earlier encoder to keep the primitive resources available.
		trace->useResource(p_work->tlas.get(), MTL::ResourceUsageRead);
		for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : p_work->blas) {
			trace->useResource(blas.get(), MTL::ResourceUsageRead);
		}
		trace->setBytes(&p_work->parameters[view], sizeof(MetalFluxParameters), 1);
		trace->setBuffer(p_work->materials.get(), 0, 2);
		trace->setBuffer(p_work->geometries.get(), 0, 3);
		trace->setBuffer(p_work->material_diagnostic->values.get(), 0, 9);
		if (p_work->regir_enabled) {
			trace->setBuffer(p_work->regir_output.get(), 0, 11);
			trace->setBuffer(p_work->regir_header.get(), 0, 12);
			trace->useResource(p_work->regir_output.get(), MTL::ResourceUsageRead);
			trace->useResource(p_work->regir_header.get(), MTL::ResourceUsageRead);
		}
		if (p_work->reusable_path_enabled) {
			trace->setBuffer(p_work->reusable_path_previous.get(), 0, 13);
			trace->setBuffer(p_work->reusable_path_header.get(), 0, 14);
			trace->setBuffer(p_work->reusable_path_staging.get(), 0, 15);
			trace->setBuffer(p_work->reusable_path_staging_claims.get(), 0, 21);
			trace->useResource(p_work->reusable_path_previous.get(), MTL::ResourceUsageRead);
			trace->useResource(p_work->reusable_path_header.get(), MTL::ResourceUsageRead);
			trace->useResource(p_work->reusable_path_staging.get(), MTL::ResourceUsageWrite);
			trace->useResource(p_work->reusable_path_staging_claims.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		}
		if (p_work->bidirectional_caustic_mirror_triangles) {
			trace->setBuffer(p_work->bidirectional_caustic_mirror_triangles.get(), 0, 16);
			trace->useResource(p_work->bidirectional_caustic_mirror_triangles.get(), MTL::ResourceUsageRead);
		}
		if (compact_view) {
			trace->setBuffer(p_work->trace_queues[view].get(), 0, 18);
			trace->setBuffer(p_work->trace_queue_counts[view].get(), 0, 20);
			trace->useResource(p_work->trace_queues[view].get(), MTL::ResourceUsageRead);
			trace->useResource(p_work->trace_queue_counts[view].get(), MTL::ResourceUsageRead);
		}
		trace->useResource(p_work->material_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		if (p_work->alpha_intersection_enabled) {
			MTL::IntersectionFunctionTable *alpha_table = p_work->shadow_only ? p_work->alpha_shadow_intersection_table.get() : p_work->alpha_trace_intersection_table.get();
			trace->setIntersectionFunctionTable(alpha_table, 10);
			trace->useResource(alpha_table, MTL::ResourceUsageRead);
			trace->useResource(p_work->alpha_material_texture_argument_buffer.get(), MTL::ResourceUsageRead);
		}
		if (p_work->bindless_material_textures) {
			trace->setBuffer(p_work->material_texture_argument_buffer.get(), 0, 8);
			trace->useResource(p_work->material_texture_argument_buffer.get(), MTL::ResourceUsageRead);
		} else {
			for (uint32_t texture_index = 0; texture_index < p_work->material_textures.size(); texture_index++) {
				trace->setTexture(p_work->material_textures[texture_index], 12 + texture_index);
			}
		}
		for (MTL::Texture *texture : p_work->material_textures) {
			if (texture) {
				trace->useResource(texture, MTL::ResourceUsageRead);
			}
		}
		trace->setSamplerState(p_work->albedo_sampler.get(), 0);
		if (!p_work->shadow_only) {
			trace->setBuffer(p_work->emissives.get(), 0, 4);
			trace->setBuffer(p_work->punctual_lights.get(), 0, 5);
			trace->setBuffer(p_work->portals.get(), 0, 6);
			trace->setBuffer(p_work->emissive_triangles.get(), 0, 7);
			trace->useResource(p_work->emissive_triangles.get(), MTL::ResourceUsageRead);
			for (MTL::Buffer *buffer : p_work->vertex_buffers) {
				trace->useResource(buffer, MTL::ResourceUsageRead);
			}
			for (MTL::Buffer *buffer : p_work->index_buffers) {
				if (buffer) {
					trace->useResource(buffer, MTL::ResourceUsageRead);
				}
			}
			for (MTL::Buffer *buffer : p_work->attribute_buffers) {
				if (buffer) {
					trace->useResource(buffer, MTL::ResourceUsageRead);
				}
			}
			trace->setTexture(p_work->environment_radiance, 28);
			trace->setTexture(p_work->environment_importance.get(), 29);
			trace->setTexture(p_work->full_environment_radiance, 30);
			trace->setTexture(p_work->reservoir_input[view], 31);
			trace->setTexture(p_work->reservoir_output[view], 32);
			trace->setTexture(p_work->reservoir_surface_input[view], 33);
			trace->setTexture(p_work->reservoir_surface_output[view], 34);
			trace->setTexture(p_work->split_diffuse[view], 36);
			trace->setTexture(p_work->split_specular[view], 37);
			trace->setTexture(p_work->diffuse_history_input[view], 38);
			trace->setTexture(p_work->diffuse_history_output[view], 39);
			trace->setTexture(p_work->specular_history_input[view], 40);
			trace->setTexture(p_work->specular_history_output[view], 41);
			trace->setTexture(p_work->diffuse_moments_input[view], 42);
			trace->setTexture(p_work->diffuse_moments_output[view], 43);
			trace->setTexture(p_work->specular_moments_input[view], 44);
			trace->setTexture(p_work->specular_moments_output[view], 45);
			trace->setTexture(p_work->reservoir_metadata_input[view], 46);
			trace->setTexture(p_work->reservoir_metadata_output[view], 47);
			trace->setTexture(p_work->reservoir_sample_input[view], 48);
			trace->setTexture(p_work->reservoir_sample_output[view], 49);
			trace->setTexture(p_work->primary_material[view] ? p_work->primary_material[view] : p_work->color[view], 50);
			trace->setTexture(p_work->primary_identity[view] ? p_work->primary_identity[view] : p_work->reservoir_primary_identity_input[view], 51);
			trace->setTexture(p_work->reservoir_primary_identity_input[view], 52);
			trace->setTexture(p_work->reservoir_primary_identity_output[view], 53);
			trace->setTexture(p_work->velocity[view], 54);
			trace->setTexture(p_work->primary_geometry[view], 55);
			trace->setTexture(p_work->primary_flags[view], 56);
			if (p_work->stbn_scalar_volume) {
				trace->setTexture(p_work->stbn_scalar_volume.get(), 57);
			}
			trace->setTexture(p_work->primary_world_position_output[view], 58);
			trace->setTexture(p_work->primary_shading_output[view], 59);
			trace->setTexture(p_work->diffuse_transport_sample_input[view], 60);
			trace->setTexture(p_work->specular_transport_sample_input[view], 61);
			trace->setTexture(p_work->diffuse_transport_sample_output[view], 62);
			trace->setTexture(p_work->specular_transport_sample_output[view], 63);
			trace->setTexture(p_work->primary_shading_input[view], 66);
			trace->setSamplerState(p_work->environment_sampler.get(), 1);
			if (p_work->environment_radiance) {
				trace->useResource(p_work->environment_radiance, MTL::ResourceUsageRead);
			}
			if (p_work->environment_importance) {
				trace->useResource(p_work->environment_importance.get(), MTL::ResourceUsageRead);
			}
			if (p_work->full_environment_radiance) {
				trace->useResource(p_work->full_environment_radiance, MTL::ResourceUsageRead);
			}
			trace->useResource(p_work->reservoir_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->reservoir_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->reservoir_surface_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->reservoir_surface_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->split_diffuse[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->split_specular[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->diffuse_history_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->diffuse_history_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->specular_history_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->specular_history_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->diffuse_moments_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->diffuse_moments_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->specular_moments_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->specular_moments_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->reservoir_metadata_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->reservoir_metadata_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->reservoir_sample_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->reservoir_sample_output[view], MTL::ResourceUsageWrite);
			if (p_work->primary_material[view]) {
				trace->useResource(p_work->primary_material[view], MTL::ResourceUsageRead);
				trace->useResource(p_work->primary_identity[view], MTL::ResourceUsageRead);
				trace->useResource(p_work->primary_geometry[view], MTL::ResourceUsageRead);
				trace->useResource(p_work->primary_flags[view], MTL::ResourceUsageRead);
			}
			trace->useResource(p_work->reservoir_primary_identity_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->reservoir_primary_identity_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->primary_world_position_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->primary_shading_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->diffuse_transport_sample_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->specular_transport_sample_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->diffuse_transport_sample_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->specular_transport_sample_output[view], MTL::ResourceUsageWrite);
			trace->useResource(p_work->primary_shading_input[view], MTL::ResourceUsageRead);
			trace->useResource(p_work->velocity[view], MTL::ResourceUsageRead);
			if (p_work->stbn_scalar_volume) {
				trace->useResource(p_work->stbn_scalar_volume.get(), MTL::ResourceUsageRead);
			}
			trace->setLabel(NS::String::string("environment_sampling", NS::UTF8StringEncoding));
		}
		trace->setTexture(p_work->depth[view], 0);
		trace->setTexture(p_work->normal_roughness[view], 1);
		if (p_work->shadow_only) {
			trace->setTexture(p_work->effect_output[view], 2);
		} else {
			trace->setTexture(p_work->color[view], 2);
			trace->setTexture(p_work->effect_output[view], 3);
			trace->setTexture(p_work->guide_normal[view], 4);
			trace->setTexture(p_work->guide_diffuse[view], 5);
			trace->setTexture(p_work->guide_specular[view], 6);
			trace->setTexture(p_work->guide_roughness[view], 7);
			trace->setTexture(p_work->guide_denoise_strength[view], 8);
			trace->setTexture(p_work->guide_reactive[view], 9);
			trace->setTexture(p_work->guide_specular_distance[view], 10);
			trace->setTexture(p_work->guide_transparency[view], 11);
		}
	};
		if (compact_view) {
			// One fixed 64-lane 1D group per logical compact entry. The indirect
			// argument records contain rounded group counts; trace_hybrid bounds
			// lanes against the atomic logical queue count.
			for (uint32_t queue_index = 0; queue_index < 5u; queue_index++) {
				MTL::ComputeCommandEncoder *trace = compute_encoder(timing_base + queue_index * 2u, timing_base + queue_index * 2u + 1u);
				configure_trace_encoder(trace);
				compact_context.queue_index = queue_index;
				trace->setBytes(&compact_context, sizeof(MetalFluxTraceCompactContext), 19);
				trace->dispatchThreadgroups(p_work->trace_indirect_arguments[view].get(), uint64_t(queue_index) * sizeof(MTL::DispatchThreadgroupsIndirectArguments), MTL::Size(64, 1, 1));
				trace->endEncoding();
			}
		} else {
			MTL::ComputeCommandEncoder *trace = compute_encoder(timing_base, timing_base + 1);
			configure_trace_encoder(trace);
			trace->setBytes(&compact_context, sizeof(MetalFluxTraceCompactContext), 19);
			trace->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
			trace->endEncoding();
		}
		if (p_work->shadow_only) {
			continue;
		}
		if (p_work->regir_enabled) {
			// Staging is one record per 8x8 tile/cell. Threadgroup-local reduction
			// makes classification deterministic; the global one-thread-per-cell
			// reduction is the only grid update and happens after tracing.
			MTL::ComputeCommandEncoder *classify = command_buffer->computeCommandEncoder();
			classify->setLabel(NS::String::string("regir_classify", NS::UTF8StringEncoding));
			classify->setComputePipelineState(p_work->regir_classify_pipeline.get());
			classify->setBuffer(p_work->regir_header.get(), 0, 0);
			classify->setBuffer(p_work->regir_staging.get(), 0, 1);
			classify->setTexture(p_work->primary_world_position_output[view], 0);
			classify->setTexture(p_work->reservoir_output[view], 1);
			classify->setTexture(p_work->reservoir_sample_output[view], 2);
			classify->setTexture(p_work->reservoir_metadata_output[view], 3);
			classify->useResource(p_work->regir_header.get(), MTL::ResourceUsageRead);
			classify->useResource(p_work->regir_staging.get(), MTL::ResourceUsageWrite);
			classify->useResource(p_work->primary_world_position_output[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->reservoir_output[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->reservoir_sample_output[view], MTL::ResourceUsageRead);
			classify->useResource(p_work->reservoir_metadata_output[view], MTL::ResourceUsageRead);
			classify->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
			classify->endEncoding();
			MTL::ComputeCommandEncoder *reduce = command_buffer->computeCommandEncoder();
			reduce->setLabel(NS::String::string("regir_reduce", NS::UTF8StringEncoding));
			reduce->setComputePipelineState(p_work->regir_reduce_pipeline.get());
			reduce->setBuffer(p_work->regir_header.get(), 0, 0);
			reduce->setBuffer(p_work->regir_staging.get(), 0, 1);
			reduce->setBuffer(p_work->regir_output.get(), 0, 2);
			reduce->useResource(p_work->regir_header.get(), MTL::ResourceUsageRead);
			reduce->useResource(p_work->regir_staging.get(), MTL::ResourceUsageRead);
			reduce->useResource(p_work->regir_output.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
			reduce->dispatchThreads(MTL::Size(p_work->regir_cell_count, 1, 1), MTL::Size(32, 1, 1));
			reduce->endEncoding();
		}
		if (p_work->reusable_path_enabled) {
			// This is deliberately after the trace. It reads immutable previous and
			// staged records, then writes the separate next cache exactly once/cell.
			MTL::ComputeCommandEncoder *reduce = command_buffer->computeCommandEncoder();
			reduce->setLabel(NS::String::string("reusable_path_reduce", NS::UTF8StringEncoding));
			reduce->setComputePipelineState(p_work->reusable_path_reduce_pipeline.get());
			reduce->setBuffer(p_work->reusable_path_header.get(), 0, 0);
			reduce->setBuffer(p_work->reusable_path_previous.get(), 0, 1);
			reduce->setBuffer(p_work->reusable_path_staging.get(), 0, 2);
			reduce->setBuffer(p_work->reusable_path_next.get(), 0, 3);
			reduce->setBuffer(p_work->material_diagnostic->values.get(), 0, 4);
			reduce->useResource(p_work->reusable_path_header.get(), MTL::ResourceUsageRead);
			reduce->useResource(p_work->reusable_path_previous.get(), MTL::ResourceUsageRead);
			reduce->useResource(p_work->reusable_path_staging.get(), MTL::ResourceUsageRead);
			reduce->useResource(p_work->reusable_path_next.get(), MTL::ResourceUsageWrite);
			reduce->useResource(p_work->material_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
			reduce->dispatchThreads(MTL::Size(p_work->reusable_path_cell_count, 1, 1), MTL::Size(32, 1, 1));
			reduce->endEncoding();
		}
		if (p_work->transport_metadata_update_enabled) {
		MTL::ComputeCommandEncoder *reconstruction = compute_encoder(timing_base + 10, timing_base + 11);
		reconstruction->setComputePipelineState(p_work->split_reconstruction_pipeline.get());
		reconstruction->setBytes(&p_work->parameters[view], sizeof(MetalFluxParameters), 0);
		reconstruction->setBuffer(p_work->material_diagnostic->values.get(), 0, 1);
		reconstruction->setTexture(p_work->depth[view], 0);
		reconstruction->setTexture(p_work->reservoir_surface_output[view], 1);
		reconstruction->setTexture(p_work->reservoir_surface_input[view], 2);
		reconstruction->setTexture(p_work->split_diffuse[view], 3);
		reconstruction->setTexture(p_work->split_specular[view], 4);
		reconstruction->setTexture(p_work->diffuse_history_input[view], 5);
		reconstruction->setTexture(p_work->diffuse_history_output[view], 6);
		reconstruction->setTexture(p_work->specular_history_input[view], 7);
		reconstruction->setTexture(p_work->specular_history_output[view], 8);
		reconstruction->setTexture(p_work->diffuse_moments_input[view], 9);
		reconstruction->setTexture(p_work->diffuse_moments_output[view], 10);
		reconstruction->setTexture(p_work->specular_moments_input[view], 11);
		reconstruction->setTexture(p_work->specular_moments_output[view], 12);
		reconstruction->setTexture(p_work->effect_output[view], 13);
		reconstruction->setTexture(p_work->velocity[view], 14);
		reconstruction->setTexture(p_work->reservoir_primary_identity_output[view], 15);
		reconstruction->setTexture(p_work->reservoir_primary_identity_input[view], 16);
		reconstruction->setTexture(p_work->primary_shading_output[view], 17);
		reconstruction->setTexture(p_work->primary_shading_input[view], 18);
		reconstruction->setTexture(p_work->diffuse_transport_sample_output[view], 19);
		reconstruction->setTexture(p_work->specular_transport_sample_output[view], 20);
		reconstruction->useResource(p_work->reservoir_surface_output[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->reservoir_surface_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->split_diffuse[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->split_specular[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->diffuse_history_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->diffuse_history_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->specular_history_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->specular_history_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->diffuse_moments_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->diffuse_moments_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->specular_moments_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->specular_moments_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->effect_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->velocity[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->reservoir_primary_identity_output[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->reservoir_primary_identity_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->primary_shading_output[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->primary_shading_input[view], MTL::ResourceUsageRead);
		reconstruction->useResource(p_work->diffuse_transport_sample_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->specular_transport_sample_output[view], MTL::ResourceUsageWrite);
		reconstruction->useResource(p_work->material_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		reconstruction->setLabel(NS::String::string(p_work->split_reconstruction_enabled ? "split_transport_reconstruction" : "metalfx_transport_metadata", NS::UTF8StringEncoding));
		reconstruction->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
		reconstruction->endEncoding();
		}
		MTL::ComputeCommandEncoder *composite = compute_encoder(timing_base + 14, timing_base + 15);
		composite->setComputePipelineState(p_work->composite_pipeline.get());
		composite->setBuffer(p_work->material_diagnostic->values.get(), 0, 0);
		composite->setBytes(&p_work->parameters[view], sizeof(MetalFluxParameters), 1);
		composite->setTexture(p_work->effect_output[view], 0);
		composite->setTexture(p_work->color[view], 1);
		composite->setTexture(p_work->depth[view], 2);
		composite->useResource(p_work->material_diagnostic->values.get(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		composite->dispatchThreads(MTL::Size(p_work->parameters[view].dimensions.x, p_work->parameters[view].dimensions.y, 1), MTL::Size(8, 8, 1));
		composite->endEncoding();
	}
	if (p_work->timing) {
		const std::shared_ptr<MetalFluxTimingCapture> timing = p_work->timing;
		MTL::Device *device = metal_driver->get_device();
		command_buffer->addCompletedHandler([timing, device](MTL::CommandBuffer *) {
			device->sampleTimestamps(&timing->cpu_end, &timing->gpu_end);
			timing->complete.store(true, std::memory_order_release);
		});
	}
	if (p_work->environment_diagnostic) {
		const std::shared_ptr<MetalFluxEnvironmentDiagnosticCapture> diagnostic = p_work->environment_diagnostic;
		command_buffer->addCompletedHandler([diagnostic](MTL::CommandBuffer *) {
			diagnostic->complete.store(true, std::memory_order_release);
		});
	}
	if (p_work->material_diagnostic) {
		const std::shared_ptr<MetalFluxMaterialDiagnosticCapture> diagnostic = p_work->material_diagnostic;
		command_buffer->addCompletedHandler([diagnostic](MTL::CommandBuffer *) {
			if (diagnostic->report_metalfx_reactive_coverage) {
				const uint32_t *words = static_cast<const uint32_t *>(diagnostic->values->contents());
				const uint32_t opaque_pixels = words[MATERIAL_DIAGNOSTIC_METALFX_REACTIVE_OPAQUE_PIXELS];
				const uint32_t rejected_pixels = words[MATERIAL_DIAGNOSTIC_METALFX_REACTIVE_REJECTED_PIXELS];
				if (opaque_pixels > 0) {
					print_line(vformat("Flux MetalFX reactive coverage frame=%d: rejected=%d/%d (%.4f).", diagnostic->submission_frame, rejected_pixels, opaque_pixels, double(rejected_pixels) / double(opaque_pixels)));
				}
			}
			diagnostic->complete.store(true, std::memory_order_release);
		});
	}
	if (p_work->residency_completion) {
		const std::shared_ptr<std::atomic<uint64_t>> completion = p_work->residency_completion;
		const uint64_t token = p_work->residency_submission_token;
		command_buffer->addCompletedHandler([completion, token](MTL::CommandBuffer *) {
			uint64_t old = completion->load(std::memory_order_relaxed);
			while (old < token && !completion->compare_exchange_weak(old, token, std::memory_order_release, std::memory_order_relaxed)) {
			}
		});
	}
	command->retain_resource(p_work->trace_pipeline.get());
	if (p_work->trace_classify_pipeline) {
		command->retain_resource(p_work->trace_classify_pipeline.get());
	}
	if (p_work->trace_indirect_finalize_pipeline) {
		command->retain_resource(p_work->trace_indirect_finalize_pipeline.get());
	}
	command->retain_resource(p_work->shadow_pipeline.get());
	if (p_work->alpha_intersection_enabled) {
		command->retain_resource(p_work->alpha_trace_pipeline.get());
		command->retain_resource(p_work->alpha_shadow_pipeline.get());
		command->retain_resource(p_work->alpha_trace_intersection_table.get());
		command->retain_resource(p_work->alpha_shadow_intersection_table.get());
		command->retain_resource(p_work->alpha_material_texture_argument_buffer.get());
	}
	command->retain_resource(p_work->filter_pipeline.get());
	command->retain_resource(p_work->temporal_pipeline.get());
	command->retain_resource(p_work->split_reconstruction_pipeline.get());
	command->retain_resource(p_work->composite_pipeline.get());
	command->retain_resource(p_work->environment_build_pipeline.get());
	command->retain_resource(p_work->environment_reduce_pipeline.get());
	command->retain_resource(p_work->environment_diagnostic_pipeline.get());
	command->retain_resource(p_work->emissive_triangle_build_pipeline.get());
	command->retain_resource(p_work->emissive_triangle_block_scan_pipeline.get());
	command->retain_resource(p_work->emissive_triangle_block_prefix_pipeline.get());
	command->retain_resource(p_work->emissive_triangle_finalize_pipeline.get());
	if (p_work->regir_enabled) {
		command->retain_resource(p_work->regir_scroll_pipeline.get());
		command->retain_resource(p_work->regir_classify_pipeline.get());
		command->retain_resource(p_work->regir_reduce_pipeline.get());
		command->retain_resource(p_work->regir_header.get());
		command->retain_resource(p_work->regir_input.get());
		command->retain_resource(p_work->regir_output.get());
		command->retain_resource(p_work->regir_staging.get());
	}
	command->retain_resource(p_work->tlas.get());
	if (p_work->tlas_scratch) {
		command->retain_resource(p_work->tlas_scratch.get());
	}
	if (p_work->tlas_instances) {
		command->retain_resource(p_work->tlas_instances.get());
	}
	if (p_work->opaque_tlas && p_work->opaque_tlas.get() != p_work->tlas.get()) {
		command->retain_resource(p_work->opaque_tlas.get());
	}
	if (p_work->opaque_tlas_scratch) {
		command->retain_resource(p_work->opaque_tlas_scratch.get());
	}
	if (p_work->opaque_tlas_instances) {
		command->retain_resource(p_work->opaque_tlas_instances.get());
	}
	if (p_work->alpha_tlas && p_work->alpha_tlas.get() != p_work->tlas.get()) {
		command->retain_resource(p_work->alpha_tlas.get());
	}
	if (p_work->alpha_tlas_scratch) {
		command->retain_resource(p_work->alpha_tlas_scratch.get());
	}
	if (p_work->alpha_tlas_instances) {
		command->retain_resource(p_work->alpha_tlas_instances.get());
	}
	if (p_work->materials) {
		command->retain_resource(p_work->materials.get());
	}
	if (p_work->geometries) {
		command->retain_resource(p_work->geometries.get());
	}
	if (p_work->emissives) {
		command->retain_resource(p_work->emissives.get());
	}
	if (p_work->emissive_triangles) command->retain_resource(p_work->emissive_triangles.get());
	if (p_work->emissive_triangle_block_sums) command->retain_resource(p_work->emissive_triangle_block_sums.get());
	if (p_work->emissive_triangle_total) command->retain_resource(p_work->emissive_triangle_total.get());
	if (p_work->bidirectional_caustic_mirror_triangles) command->retain_resource(p_work->bidirectional_caustic_mirror_triangles.get());
	if (p_work->punctual_lights) {
		command->retain_resource(p_work->punctual_lights.get());
	}
	if (p_work->portals) {
		command->retain_resource(p_work->portals.get());
	}
	if (p_work->albedo_sampler) {
		command->retain_resource(p_work->albedo_sampler.get());
	}
	if (p_work->environment_sampler) {
		command->retain_resource(p_work->environment_sampler.get());
	}
	if (p_work->environment_radiance) {
		command->retain_resource(p_work->environment_radiance);
	}
	if (p_work->environment_importance) {
		command->retain_resource(p_work->environment_importance.get());
	}
	if (p_work->environment_diagnostic) {
		command->retain_resource(p_work->environment_diagnostic->values.get());
	}
	if (p_work->material_diagnostic) {
		command->retain_resource(p_work->material_diagnostic->values.get());
	}
	if (p_work->material_texture_argument_buffer) {
		command->retain_resource(p_work->material_texture_argument_buffer.get());
	}
	for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : p_work->blas) {
		command->retain_resource(blas.get());
	}
	for (const NS::SharedPtr<MTL::AccelerationStructure> &source : p_work->blas_sources) {
		if (source) {
			command->retain_resource(source.get());
		}
	}
	for (const NS::SharedPtr<MTL::Buffer> &scratch : p_work->blas_scratch) {
		if (scratch) {
			command->retain_resource(scratch.get());
		}
	}
	for (const NS::SharedPtr<MTL::Buffer> &readback : p_work->blas_compaction_query_buffers) if (readback) command->retain_resource(readback.get());
	for (const NS::SharedPtr<MTL::AccelerationStructure> &source : p_work->blas_compaction_copy_sources) if (source) command->retain_resource(source.get());
	for (const NS::SharedPtr<MTL::AccelerationStructure> &destination : p_work->blas_compaction_copy_destinations) if (destination) command->retain_resource(destination.get());
	for (const NS::SharedPtr<MTL::Buffer> &transform : p_work->decode_transforms) {
		command->retain_resource(transform.get());
	}
	for (MTL::Buffer *buffer : p_work->vertex_buffers) {
		command->retain_resource(buffer);
	}
	for (MTL::Buffer *buffer : p_work->index_buffers) {
		if (buffer) {
			command->retain_resource(buffer);
		}
	}
	for (MTL::Buffer *buffer : p_work->attribute_buffers) {
		if (buffer) {
			command->retain_resource(buffer);
		}
	}
	for (MTL::Texture *texture : p_work->material_textures) {
		if (texture) {
			command->retain_resource(texture);
		}
	}
	for (MTL::Texture *texture : p_work->reservoir_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_surface_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_surface_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_metadata_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_metadata_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_sample_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_sample_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_primary_identity_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->reservoir_primary_identity_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->primary_shading_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->primary_shading_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->primary_world_position_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->diffuse_history_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->diffuse_history_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->specular_history_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->specular_history_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->diffuse_transport_sample_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->diffuse_transport_sample_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->specular_transport_sample_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->specular_transport_sample_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->diffuse_moments_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->diffuse_moments_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->specular_moments_input) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->specular_moments_output) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->split_diffuse) command->retain_resource(texture);
	for (MTL::Texture *texture : p_work->split_specular) command->retain_resource(texture);
	for (const NS::SharedPtr<MTL::Buffer> &buffer : p_work->trace_queues) if (buffer) command->retain_resource(buffer.get());
	for (const NS::SharedPtr<MTL::Buffer> &buffer : p_work->trace_queue_counts) if (buffer) command->retain_resource(buffer.get());
	for (const NS::SharedPtr<MTL::Buffer> &buffer : p_work->trace_indirect_arguments) if (buffer) command->retain_resource(buffer.get());
	memdelete(p_work);
}

MetalFluxEffect::MetalFluxEffect() {
	cache = memnew(MetalFluxEffectCache);
}

MetalFluxEffect::~MetalFluxEffect() {
	memdelete(cache);
}

bool MetalFluxEffect::is_native_ray_tracing_supported() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || rd->get_device_api_name() != "Metal") {
		return false;
	}
	RenderingDeviceDriverMetal *driver = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	return driver && driver->get_device() && driver->get_device()->supportsRaytracing();
}

bool MetalFluxEffect::is_supported() const {
	return is_native_ray_tracing_supported();
}

Error MetalFluxEffect::append_streamed_cluster_surfaces(FrameRequest &r_request, const Vector<RendererPathTracing::StreamedClusterSurface> &p_surfaces, const Transform3D &p_world_transform, const Vector<Instance> &p_material_templates) {
	for (const RendererPathTracing::StreamedClusterSurface &resident : p_surfaces) {
		ERR_FAIL_COND_V_MSG(resident.stable_id == 0 || !resident.vertex_buffer.is_valid() || !resident.index_buffer.is_valid() || resident.vertex_count < 3 || resident.index_count < 3 || resident.index_count % 3 != 0, ERR_INVALID_DATA, "Metal hybrid streamed cluster surface contains incomplete triangle geometry.");
		ERR_FAIL_COND_V_MSG(resident.vertex_stride != 12 || (resident.index_stride != 2 && resident.index_stride != 4), ERR_UNAVAILABLE, "Metal hybrid streamed cluster surface uses an unsupported vertex or index layout.");
		ERR_FAIL_COND_V_MSG(resident.material_index >= uint32_t(p_material_templates.size()), ERR_UNAVAILABLE, vformat("Metal hybrid streamed cluster surface %d has no scalar material/emission template for material slot %d.", resident.stable_id, resident.material_index));

		Surface surface;
		surface.stable_id = resident.stable_id;
		surface.topology_revision = resident.topology_revision;
		surface.deformation_revision = resident.topology_revision;
		surface.vertex_buffer = resident.vertex_buffer;
		surface.index_buffer = resident.index_buffer;
		surface.vertex_count = resident.vertex_count;
		surface.index_count = resident.index_count;
		surface.vertex_stride = resident.vertex_stride;
		surface.index_stride = resident.index_stride;
		surface.compressed_aabb = resident.bounds;
		r_request.surfaces.push_back(surface);

		Instance instance = p_material_templates[resident.material_index];
		instance.stable_id = resident.stable_id ^ 0x6a09e667f3bcc909ull;
		if (instance.stable_id == 0) {
			instance.stable_id = resident.stable_id;
		}
		instance.surface_id = resident.stable_id;
		instance.transform = p_world_transform;
		r_request.instances.push_back(instance);
	}
	return OK;
}

static Error _hybrid_fail(Error p_error, const String &p_message, String *r_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

static MTL::CounterSet *_timestamp_counter_set(MTL::Device *p_device) {
	NS::Array *sets = p_device->counterSets();
	for (NS::UInteger index = 0; index < sets->count(); index++) {
		MTL::CounterSet *set = static_cast<MTL::CounterSet *>(sets->object(index));
		if (set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
			return set;
		}
	}
	return nullptr;
}

Error MetalFluxEffect::render(const FrameRequest &p_request, FrameResult &r_result, String *r_error) {
	const uint64_t cpu_render_begin = mach_absolute_time();
	mach_timebase_info_data_t cpu_timebase = {};
	mach_timebase_info(&cpu_timebase);
	auto cpu_milliseconds_since = [&cpu_timebase](uint64_t p_begin, uint64_t p_end) {
		return double(p_end - p_begin) * double(cpu_timebase.numer) / double(cpu_timebase.denom) / 1000000.0;
	};
	r_result = FrameResult();
	r_result.unsupported_materials = p_request.unsupported_materials;
	r_result.unsupported_clearcoat_materials = p_request.unsupported_clearcoat_materials;
	r_result.unsupported_anisotropy_materials = p_request.unsupported_anisotropy_materials;
	r_result.unsupported_transmission_materials = p_request.unsupported_transmission_materials;
	r_result.supported_thin_transmission_materials = p_request.supported_thin_transmission_materials;
	r_result.unsupported_transmission_texture_materials = p_request.unsupported_transmission_texture_materials;
	r_result.unsupported_transmission_volume_materials = p_request.unsupported_transmission_volume_materials;
	r_result.unsupported_refraction_materials = p_request.unsupported_refraction_materials;
	r_result.ray_geometry_base_triangles = p_request.ray_geometry_base_triangles;
	r_result.ray_geometry_selected_triangles = p_request.ray_geometry_selected_triangles;
	r_result.ray_lod_instance_surfaces = p_request.ray_lod_instance_surfaces;
	r_result.ray_lod_base_dynamic_surfaces = p_request.ray_lod_base_dynamic_surfaces;
	r_result.ray_lod_base_alpha_mask_surfaces = p_request.ray_lod_base_alpha_mask_surfaces;
	r_result.ray_lod_base_near_field_surfaces = p_request.ray_lod_base_near_field_surfaces;
	if (!is_supported()) {
		return _hybrid_fail(ERR_UNAVAILABLE, "The active Metal device does not support hybrid ray effects.", r_error);
	}
	if (p_request.surfaces.is_empty() || p_request.instances.is_empty() || p_request.views.is_empty()) {
		return _hybrid_fail(ERR_INVALID_PARAMETER, "Hybrid rendering requires geometry, instances, and at least one view.", r_error);
	}
	for (const Surface &surface : p_request.surfaces) {
		if (surface.has_tangents && !surface.has_normals) {
			return _hybrid_fail(ERR_INVALID_PARAMETER, "Flux tangent geometry requires a normal stream.", r_error);
		}
		if (surface.has_normals && (surface.normal_stride < sizeof(uint32_t) || (surface.compressed && surface.vertex_stride < sizeof(uint16_t) * 4) || (!surface.compressed && surface.has_tangents && surface.normal_stride < sizeof(uint32_t) * 2))) {
			return _hybrid_fail(ERR_INVALID_PARAMETER, "Flux geometry does not match the normal/tangent record ABI.", r_error);
		}
	}
	RenderingDevice *rd = RD::get_singleton();
	RenderingDeviceDriverMetal *rdd = static_cast<RenderingDeviceDriverMetal *>(rd->get_device_driver());
	MTL::Device *device = rdd->get_device();
	for (int capture_index = cache->environment_diagnostic_captures.size() - 1; capture_index >= 0; capture_index--) {
		const std::shared_ptr<MetalFluxEnvironmentDiagnosticCapture> &capture = cache->environment_diagnostic_captures[capture_index];
		if (!capture->complete.load(std::memory_order_acquire)) {
			continue;
		}
		const uint32_t *words = static_cast<const uint32_t *>(capture->values->contents());
		RendererPathTracing::EnvironmentImportanceRebuildDiagnostics diagnostic;
		diagnostic.nonfinite_texel_count = words[ENVIRONMENT_DIAGNOSTIC_NONFINITE_COUNT];
		diagnostic.finite_peak_luminance = _environment_diagnostic_float(words[ENVIRONMENT_DIAGNOSTIC_PEAK_LUMINANCE]);
		diagnostic.maximum_texel_weight = _environment_diagnostic_float(words[ENVIRONMENT_DIAGNOSTIC_MAXIMUM_WEIGHT]);
		diagnostic.finite_peak_rgb = Vector3(
				_environment_diagnostic_float(words[ENVIRONMENT_DIAGNOSTIC_PEAK_RED]),
				_environment_diagnostic_float(words[ENVIRONMENT_DIAGNOSTIC_PEAK_GREEN]),
				_environment_diagnostic_float(words[ENVIRONMENT_DIAGNOSTIC_PEAK_BLUE]));
		diagnostic.total_importance_weight = _environment_diagnostic_float(words[ENVIRONMENT_DIAGNOSTIC_TOTAL_WEIGHT]);
		print_line(vformat("Flux environment GPU (provisional Metal): source_id=%d generation=%d checksum=%d sharp_format=RGBA32Float sharp_bytes=%d post_sky_nonfinite_texels=%d finite_peak_rgb=(%s, %s, %s) finite_peak_luminance=%s total_importance_weight=%s maximum_texel_weight=%s top_probability=%s selectable=%s; one-shot distribution-rebuild readback.",
				capture->source_id,
				capture->generation,
				capture->checksum,
				RendererPathTracing::environment_importance_full_float_radiance_bytes(capture->width, capture->height),
				diagnostic.nonfinite_texel_count,
				String::num(diagnostic.finite_peak_rgb.x),
				String::num(diagnostic.finite_peak_rgb.y),
				String::num(diagnostic.finite_peak_rgb.z),
				String::num(diagnostic.finite_peak_luminance),
				String::num(diagnostic.total_importance_weight),
				String::num(diagnostic.maximum_texel_weight),
				String::num(diagnostic.top_probability()),
				diagnostic.is_finite() && diagnostic.total_importance_weight > 0.0f ? "yes" : "no"));
		cache->environment_diagnostic_captures.remove_at(capture_index);
	}
	bool material_diagnostics_observed = false;
	bool full_material_diagnostics_observed = false;
	for (int capture_index = cache->material_diagnostic_captures.size() - 1; capture_index >= 0; capture_index--) {
		const std::shared_ptr<MetalFluxMaterialDiagnosticCapture> &capture = cache->material_diagnostic_captures[capture_index];
		if (!capture->complete.load(std::memory_order_acquire)) {
			continue;
		}
		const uint32_t *words = static_cast<const uint32_t *>(capture->values->contents());
		if (!capture->shadow_only && capture->diagnostics_owner_id != 0) {
			MetalFluxEffect::WorkAttribution attribution;
			attribution.diagnostics_owner_id = capture->diagnostics_owner_id;
			attribution.diagnostics_frame = capture->diagnostics_frame;
			attribution.light_revision_completed = capture->light_revision;
			attribution.trace_compaction_active = capture->trace_compaction_active;
			attribution.trace_compaction_fallback = capture->trace_compaction_fallback;
			attribution.alpha_candidates = words[MATERIAL_DIAGNOSTIC_ALPHA_CANDIDATES];
			attribution.alpha_rejections = words[MATERIAL_DIAGNOSTIC_ALPHA_REJECTIONS];
			attribution.alpha_candidate_exhaustions = words[MATERIAL_DIAGNOSTIC_ALPHA_CANDIDATE_EXHAUSTIONS];
			attribution.alpha_mixed_intersections = words[MATERIAL_DIAGNOSTIC_MIXED_INTERSECTIONS];
			for (uint32_t ray_class = 0; ray_class < 4; ray_class++) {
				attribution.alpha_split_opaque_queries[ray_class] = words[MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_QUERIES_PRIMARY + ray_class];
				attribution.alpha_split_opaque_hits[ray_class] = words[MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_HITS_PRIMARY + ray_class];
				attribution.alpha_split_alpha_queries[ray_class] = words[MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_QUERIES_PRIMARY + ray_class];
				attribution.alpha_split_alpha_hits[ray_class] = words[MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_HITS_PRIMARY + ray_class];
				attribution.alpha_split_alpha_rejections[ray_class] = words[MATERIAL_DIAGNOSTIC_SPLIT_ALPHA_REJECTIONS_PRIMARY + ray_class];
				attribution.alpha_split_mixed_fallbacks[ray_class] = words[MATERIAL_DIAGNOSTIC_SPLIT_MIXED_FALLBACK_PRIMARY + ray_class];
			}
			attribution.alpha_rear_opaque_hits = words[MATERIAL_DIAGNOSTIC_REAR_OPAQUE_HITS];
			attribution.alpha_primary_candidates = words[MATERIAL_DIAGNOSTIC_ALPHA_PRIMARY_CANDIDATES];
			attribution.alpha_primary_rejections = words[MATERIAL_DIAGNOSTIC_ALPHA_PRIMARY_REJECTIONS];
			attribution.alpha_visibility_candidates = words[MATERIAL_DIAGNOSTIC_ALPHA_VISIBILITY_CANDIDATES];
			attribution.alpha_visibility_rejections = words[MATERIAL_DIAGNOSTIC_ALPHA_VISIBILITY_REJECTIONS];
			attribution.alpha_reflection_candidates = words[MATERIAL_DIAGNOSTIC_ALPHA_REFLECTION_CANDIDATES];
			attribution.alpha_reflection_rejections = words[MATERIAL_DIAGNOSTIC_ALPHA_REFLECTION_REJECTIONS];
			attribution.alpha_indirect_candidates = words[MATERIAL_DIAGNOSTIC_ALPHA_INDIRECT_CANDIDATES];
			attribution.alpha_indirect_rejections = words[MATERIAL_DIAGNOSTIC_ALPHA_INDIRECT_REJECTIONS];
			attribution.alpha_max_candidates_per_ray = words[MATERIAL_DIAGNOSTIC_ALPHA_MAX_CANDIDATES_PER_RAY];
			attribution.alpha_occupancy_empty_rejections = words[MATERIAL_DIAGNOSTIC_OCCUPANCY_EMPTY_REJECTIONS];
			attribution.alpha_occupancy_opaque_accepts = words[MATERIAL_DIAGNOSTIC_OCCUPANCY_OPAQUE_ACCEPTS];
			attribution.alpha_occupancy_mixed_samples = words[MATERIAL_DIAGNOSTIC_OCCUPANCY_MIXED_SAMPLES];
			attribution.invalid_pdf_samples = words[MATERIAL_DIAGNOSTIC_INVALID_PDF_SAMPLES];
			attribution.nonfinite_lobe_samples = words[MATERIAL_DIAGNOSTIC_NONFINITE_LOBE_SAMPLES];
			attribution.rejected_energy_samples = words[MATERIAL_DIAGNOSTIC_REJECTED_ENERGY_SAMPLES];
			attribution.primary_valid_pixels = words[MATERIAL_DIAGNOSTIC_PRIMARY_VALID_PIXELS];
			attribution.primary_invalid_pixels = words[MATERIAL_DIAGNOSTIC_PRIMARY_INVALID_PIXELS];
			attribution.primary_lit_pixels = words[MATERIAL_DIAGNOSTIC_PRIMARY_LIT_PIXELS];
			attribution.primary_analytic_selected = words[MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_SELECTED];
			attribution.primary_analytic_contributed = words[MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_CONTRIBUTED];
				attribution.primary_analytic_visibility_tests = words[MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_VISIBILITY_TESTS];
				attribution.reusable_path_staged = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_STAGED];
				attribution.reusable_path_updates = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_UPDATES];
				attribution.reusable_path_queries = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_QUERIES];
				attribution.reusable_path_valid_candidates = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_VALID_CANDIDATES];
				attribution.reusable_path_reused_candidates = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_REUSED_CANDIDATES];
				attribution.reusable_path_rejections = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_REJECTIONS];
				attribution.reusable_path_occupied = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_OCCUPIED];
				attribution.reusable_path_invalid_record = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_INVALID_RECORD];
				attribution.reusable_path_endpoint_blocked = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ENDPOINT_BLOCKED];
				attribution.reusable_path_shading_invalid = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_SHADING_INVALID];
				attribution.reusable_path_zero_target = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ZERO_TARGET];
				attribution.reusable_path_invalid_weight = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_INVALID_WEIGHT];
				attribution.reusable_path_considered = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_CONSIDERED];
				attribution.reusable_path_accepted = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ACCEPTED];
				attribution.reusable_path_selected = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_SELECTED];
				attribution.reusable_path_reevaluations = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_REEVALUATIONS];
				attribution.reusable_path_reconnection_visibility = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_RECONNECTION_VISIBILITY];
				attribution.restir_gi_current_candidates = words[MATERIAL_DIAGNOSTIC_RESTIR_GI_CURRENT_CANDIDATES];
				attribution.restir_gi_reused_candidates = words[MATERIAL_DIAGNOSTIC_RESTIR_GI_REUSED_CANDIDATES];
				attribution.restir_gi_selected_reuse = words[MATERIAL_DIAGNOSTIC_RESTIR_GI_SELECTED_REUSE];
				attribution.reusable_path_lighting_reevaluations = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_LIGHTING_REEVALUATIONS];
				attribution.reusable_path_environment_reevaluations = words[MATERIAL_DIAGNOSTIC_REUSABLE_PATH_ENVIRONMENT_REEVALUATIONS];
				attribution.bidirectional_caustic_candidates = words[MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_CANDIDATES];
				attribution.bidirectional_caustic_valid = words[MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_VALID];
				attribution.bidirectional_caustic_contributed = words[MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_CONTRIBUTED];
				attribution.bidirectional_caustic_visibility_rays = words[MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_VISIBILITY_RAYS];
				attribution.bidirectional_caustic_rejections = words[MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_REJECTIONS];
				attribution.bidirectional_caustic_nonfinite_or_pdf_failures = words[MATERIAL_DIAGNOSTIC_BIDIRECTIONAL_CAUSTIC_NONFINITE_OR_PDF_FAILURES];
				attribution.direct_candidate_evaluations = words[MATERIAL_DIAGNOSTIC_DIRECT_CANDIDATE_EVALUATIONS];
				attribution.direct_selected_visibility = words[MATERIAL_DIAGNOSTIC_DIRECT_SELECTED_VISIBILITY];
				attribution.direct_temporal_reuse = words[MATERIAL_DIAGNOSTIC_DIRECT_TEMPORAL_REUSE];
				attribution.direct_spatial_reuse = words[MATERIAL_DIAGNOSTIC_DIRECT_SPATIAL_REUSE];
				attribution.gi_fresh_rays = words[MATERIAL_DIAGNOSTIC_GI_FRESH_RAYS];
				attribution.reflection_rays = words[MATERIAL_DIAGNOSTIC_REFLECTION_RAYS];
				attribution.gi_converged_skips = words[MATERIAL_DIAGNOSTIC_GI_CONVERGED_SKIPS];
				attribution.reflection_converged_skips = words[MATERIAL_DIAGNOSTIC_REFLECTION_CONVERGED_SKIPS];
				attribution.reflection_validation_reprojection_rejections = words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_REPROJECTION_REJECTIONS];
				attribution.reflection_validation_surface_rejections = words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_SURFACE_REJECTIONS];
				attribution.reflection_validation_motion_rejections = words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_MOTION_REJECTIONS];
				attribution.reflection_validation_refreshes = words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_REFRESHES];
				attribution.trace_compaction_inactive_pixels = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_INACTIVE_PIXELS];
				attribution.trace_compaction_active_pixels = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ACTIVE_PIXELS];
				attribution.trace_compaction_need[0] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_DIRECT];
				attribution.trace_compaction_need[1] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_GI];
				attribution.trace_compaction_need[2] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_REFLECTION];
				attribution.trace_compaction_need[3] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_EXACT_ALPHA];
				attribution.trace_compaction_need[4] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_NEED_COMPLEX];
				attribution.trace_compaction_enqueued[0] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_DIRECT];
				attribution.trace_compaction_enqueued[1] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_GI];
				attribution.trace_compaction_enqueued[2] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_REFLECTION];
				attribution.trace_compaction_enqueued[3] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_EXACT_ALPHA];
				attribution.trace_compaction_enqueued[4] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_ENQUEUED_COMPLEX];
				attribution.trace_compaction_dispatched[0] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_DIRECT];
				attribution.trace_compaction_dispatched[1] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_GI];
				attribution.trace_compaction_dispatched[2] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_REFLECTION];
				attribution.trace_compaction_dispatched[3] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_EXACT_ALPHA];
				attribution.trace_compaction_dispatched[4] = words[MATERIAL_DIAGNOSTIC_TRACE_COMPACTION_DISPATCHED_COMPLEX];
			if (cache->completed_work_attribution.size() == 64) {
				cache->completed_work_attribution.remove_at(0);
			}
			cache->completed_work_attribution.push_back(attribution);
		}
		if (capture->stage_probe && !capture->shadow_only) {
			const uint32_t primary_pixels = words[MATERIAL_DIAGNOSTIC_PRIMARY_VALID_PIXELS];
			auto stage_mean = [&](MetalFluxMaterialDiagnosticWord p_word) -> double {
				return primary_pixels > 0 ? double(words[p_word]) / (double(primary_pixels) * 64.0) : 0.0;
			};
			const uint32_t temporal_reused = words[MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_REUSED_PIXELS];
			const uint32_t temporal_rejected = words[MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_REJECTED_PIXELS];
			const double temporal_samples = temporal_reused > 0 ? double(words[MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_HISTORY_SAMPLES]) / double(temporal_reused) : 0.0;
			print_line(vformat("Flux stage probe frame=%d primary_pixels=%d temporal_reused/rejected=%d/%d avg_history_samples=%.2f temporal_second_moment=%.6f temporal_variance=%.6f mean_luminance: raw_emission=%.6f raw_emissive=%.6f raw_analytic=%.6f raw_indirect=%.6f trace_combined=%.6f spatial=%.6f temporal_input=%.6f temporal_output=%.6f composite_base=%.6f composite_input=%.6f composite_output=%.6f.",
				capture->submission_frame,
				primary_pixels,
				temporal_reused,
				temporal_rejected,
				temporal_samples,
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_SECOND_MOMENT),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_VARIANCE),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_RAW_EMISSION),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_RAW_EMISSIVE),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_RAW_ANALYTIC),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_RAW_INDIRECT),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_TRACE_COMBINED),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_SPATIAL),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_INPUT),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_TEMPORAL_OUTPUT),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_COMPOSITE_BASE),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_COMPOSITE_INPUT),
				stage_mean(MATERIAL_DIAGNOSTIC_STAGE_COMPOSITE_OUTPUT)));
		}
		cache->material_alpha_candidates += words[MATERIAL_DIAGNOSTIC_ALPHA_CANDIDATES];
		cache->material_alpha_rejections += words[MATERIAL_DIAGNOSTIC_ALPHA_REJECTIONS];
		cache->material_alpha_candidate_exhaustions += words[MATERIAL_DIAGNOSTIC_ALPHA_CANDIDATE_EXHAUSTIONS];
		cache->material_generation_rejects += words[MATERIAL_DIAGNOSTIC_GENERATION_REJECTIONS];
		cache->material_alpha_mixed_intersections += words[MATERIAL_DIAGNOSTIC_MIXED_INTERSECTIONS];
		cache->material_alpha_rear_opaque_hits += words[MATERIAL_DIAGNOSTIC_REAR_OPAQUE_HITS];
		cache->material_alpha_primary_candidates += words[MATERIAL_DIAGNOSTIC_ALPHA_PRIMARY_CANDIDATES];
		cache->material_alpha_primary_rejections += words[MATERIAL_DIAGNOSTIC_ALPHA_PRIMARY_REJECTIONS];
		cache->material_alpha_visibility_candidates += words[MATERIAL_DIAGNOSTIC_ALPHA_VISIBILITY_CANDIDATES];
		cache->material_alpha_visibility_rejections += words[MATERIAL_DIAGNOSTIC_ALPHA_VISIBILITY_REJECTIONS];
		cache->material_alpha_reflection_candidates += words[MATERIAL_DIAGNOSTIC_ALPHA_REFLECTION_CANDIDATES];
		cache->material_alpha_reflection_rejections += words[MATERIAL_DIAGNOSTIC_ALPHA_REFLECTION_REJECTIONS];
		cache->material_alpha_indirect_candidates += words[MATERIAL_DIAGNOSTIC_ALPHA_INDIRECT_CANDIDATES];
		cache->material_alpha_indirect_rejections += words[MATERIAL_DIAGNOSTIC_ALPHA_INDIRECT_REJECTIONS];
		cache->material_alpha_max_candidates_per_ray = MAX(cache->material_alpha_max_candidates_per_ray, words[MATERIAL_DIAGNOSTIC_ALPHA_MAX_CANDIDATES_PER_RAY]);
		cache->material_occupancy_empty_rejections += words[MATERIAL_DIAGNOSTIC_OCCUPANCY_EMPTY_REJECTIONS];
		cache->material_occupancy_opaque_accepts += words[MATERIAL_DIAGNOSTIC_OCCUPANCY_OPAQUE_ACCEPTS];
		cache->material_occupancy_mixed_samples += words[MATERIAL_DIAGNOSTIC_OCCUPANCY_MIXED_SAMPLES];
		cache->metalfx_reactive_opaque_pixels += words[MATERIAL_DIAGNOSTIC_METALFX_REACTIVE_OPAQUE_PIXELS];
		cache->metalfx_reactive_rejected_pixels += words[MATERIAL_DIAGNOSTIC_METALFX_REACTIVE_REJECTED_PIXELS];
		cache->invalid_pdf_samples += words[MATERIAL_DIAGNOSTIC_INVALID_PDF_SAMPLES];
		cache->nonfinite_lobe_samples += words[MATERIAL_DIAGNOSTIC_NONFINITE_LOBE_SAMPLES];
		cache->rejected_energy_samples += words[MATERIAL_DIAGNOSTIC_REJECTED_ENERGY_SAMPLES];
		cache->primary_valid_pixels += words[MATERIAL_DIAGNOSTIC_PRIMARY_VALID_PIXELS];
		cache->primary_invalid_pixels += words[MATERIAL_DIAGNOSTIC_PRIMARY_INVALID_PIXELS];
		cache->primary_lit_pixels += words[MATERIAL_DIAGNOSTIC_PRIMARY_LIT_PIXELS];
		cache->primary_analytic_selected += words[MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_SELECTED];
		cache->primary_analytic_contributed += words[MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_CONTRIBUTED];
		cache->primary_analytic_visibility_tests += words[MATERIAL_DIAGNOSTIC_PRIMARY_ANALYTIC_VISIBILITY_TESTS];
		r_result.reflection_environment_misses = MAX(r_result.reflection_environment_misses, words[MATERIAL_DIAGNOSTIC_REFLECTION_ENVIRONMENT_MISSES]);
		r_result.reflection_environment_contributions = MAX(r_result.reflection_environment_contributions, words[MATERIAL_DIAGNOSTIC_REFLECTION_ENVIRONMENT_CONTRIBUTIONS]);
		r_result.reflection_validation_reprojection_rejections = MAX(r_result.reflection_validation_reprojection_rejections, words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_REPROJECTION_REJECTIONS]);
		r_result.reflection_validation_surface_rejections = MAX(r_result.reflection_validation_surface_rejections, words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_SURFACE_REJECTIONS]);
		r_result.reflection_validation_motion_rejections = MAX(r_result.reflection_validation_motion_rejections, words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_MOTION_REJECTIONS]);
		r_result.reflection_validation_refreshes = MAX(r_result.reflection_validation_refreshes, words[MATERIAL_DIAGNOSTIC_REFLECTION_VALIDATION_REFRESHES]);
		material_diagnostics_observed = true;
		full_material_diagnostics_observed |= !capture->shadow_only;
		cache->material_diagnostic_captures.remove_at(capture_index);
	}
	r_result.material_diagnostics_observed = material_diagnostics_observed;
	r_result.full_material_diagnostics_observed = full_material_diagnostics_observed;
	if (!cache->trace_pipeline) {
		const MetalDeviceProperties &properties = rdd->get_device_properties();
		cache->bindless_material_textures = properties.features.argument_buffers_tier == MTL::ArgumentBuffersTier2 && properties.limits.maxTexturesPerArgumentBuffer > HYBRID_FALLBACK_MATERIAL_TEXTURES;
		cache->material_texture_capacity = cache->bindless_material_textures ? MIN(uint64_t(HYBRID_MAX_BINDLESS_MATERIAL_TEXTURES), properties.limits.maxTexturesPerArgumentBuffer) : HYBRID_FALLBACK_MATERIAL_TEXTURES;
		const String shader_source = vformat("#define HYBRID_BINDLESS_MATERIALS %d\n", cache->bindless_material_textures ? 1 : 0) + HYBRID_MSL;
		const CharString shader_source_utf8 = shader_source.utf8();
		NS::Error *compile_error = nullptr;
		NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(NS::String::string(shader_source_utf8.get_data(), NS::UTF8StringEncoding), nullptr, &compile_error));
		if (!library) {
			return _hybrid_fail(ERR_CANT_CREATE, compile_error ? String::utf8(compile_error->localizedDescription()->utf8String()) : "Metal hybrid shader compilation failed.", r_error);
		}
		NS::SharedPtr<MTL::Function> trace = NS::TransferPtr(library->newFunction(NS::String::string("trace_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> shadow = NS::TransferPtr(library->newFunction(NS::String::string("trace_hybrid_shadow", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> trace_classify = NS::TransferPtr(library->newFunction(NS::String::string("trace_classify", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> trace_indirect_finalize = NS::TransferPtr(library->newFunction(NS::String::string("trace_indirect_finalize", NS::UTF8StringEncoding)));
		cache->trace_compute_function = trace;
		cache->shadow_compute_function = shadow;
		cache->alpha_intersection_function = NS::TransferPtr(library->newFunction(NS::String::string("hybrid_alpha_triangle_intersection", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> filter = NS::TransferPtr(library->newFunction(NS::String::string("filter_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> temporal = NS::TransferPtr(library->newFunction(NS::String::string("accumulate_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> split_reconstruction = NS::TransferPtr(library->newFunction(NS::String::string("reconstruct_split_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> composite = NS::TransferPtr(library->newFunction(NS::String::string("composite_hybrid", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> environment_build = NS::TransferPtr(library->newFunction(NS::String::string("environment_importance_build", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> environment_reduce = NS::TransferPtr(library->newFunction(NS::String::string("environment_importance_reduce", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> environment_diagnostic = NS::TransferPtr(library->newFunction(NS::String::string("environment_importance_diagnostic", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> emissive_triangle_build = NS::TransferPtr(library->newFunction(NS::String::string("emissive_triangle_build", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> emissive_triangle_block_scan = NS::TransferPtr(library->newFunction(NS::String::string("emissive_triangle_block_scan", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> emissive_triangle_block_prefix = NS::TransferPtr(library->newFunction(NS::String::string("emissive_triangle_block_prefix", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> emissive_triangle_finalize = NS::TransferPtr(library->newFunction(NS::String::string("emissive_triangle_finalize", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> regir_scroll = NS::TransferPtr(library->newFunction(NS::String::string("regir_scroll", NS::UTF8StringEncoding)));
		NS::SharedPtr<MTL::Function> regir_classify = NS::TransferPtr(library->newFunction(NS::String::string("regir_classify", NS::UTF8StringEncoding)));
			NS::SharedPtr<MTL::Function> regir_reduce = NS::TransferPtr(library->newFunction(NS::String::string("regir_reduce", NS::UTF8StringEncoding)));
			NS::SharedPtr<MTL::Function> reusable_path_clear_staging = NS::TransferPtr(library->newFunction(NS::String::string("reusable_path_clear_staging", NS::UTF8StringEncoding)));
			NS::SharedPtr<MTL::Function> reusable_path_reduce = NS::TransferPtr(library->newFunction(NS::String::string("reusable_path_reduce", NS::UTF8StringEncoding)));
		if (cache->bindless_material_textures) {
			cache->material_texture_argument_encoder = NS::TransferPtr(trace->newArgumentEncoder(8));
			if (!cache->material_texture_argument_encoder) {
				return _hybrid_fail(ERR_CANT_CREATE, "Metal Tier-2 material texture argument encoder could not be created.", r_error);
			}
		}
		cache->trace_pipeline = NS::TransferPtr(device->newComputePipelineState(trace.get(), &compile_error));
		if (trace_classify && trace_indirect_finalize) {
			cache->trace_classify_pipeline = NS::TransferPtr(device->newComputePipelineState(trace_classify.get(), &compile_error));
			cache->trace_indirect_finalize_pipeline = NS::TransferPtr(device->newComputePipelineState(trace_indirect_finalize.get(), &compile_error));
		}
		cache->shadow_pipeline = NS::TransferPtr(device->newComputePipelineState(shadow.get(), &compile_error));
		cache->filter_pipeline = NS::TransferPtr(device->newComputePipelineState(filter.get(), &compile_error));
		cache->temporal_pipeline = NS::TransferPtr(device->newComputePipelineState(temporal.get(), &compile_error));
		cache->split_reconstruction_pipeline = NS::TransferPtr(device->newComputePipelineState(split_reconstruction.get(), &compile_error));
		cache->composite_pipeline = NS::TransferPtr(device->newComputePipelineState(composite.get(), &compile_error));
		cache->environment_build_pipeline = NS::TransferPtr(device->newComputePipelineState(environment_build.get(), &compile_error));
		cache->environment_reduce_pipeline = NS::TransferPtr(device->newComputePipelineState(environment_reduce.get(), &compile_error));
		cache->environment_diagnostic_pipeline = NS::TransferPtr(device->newComputePipelineState(environment_diagnostic.get(), &compile_error));
		cache->emissive_triangle_build_pipeline = NS::TransferPtr(device->newComputePipelineState(emissive_triangle_build.get(), &compile_error));
		cache->emissive_triangle_block_scan_pipeline = NS::TransferPtr(device->newComputePipelineState(emissive_triangle_block_scan.get(), &compile_error));
		cache->emissive_triangle_block_prefix_pipeline = NS::TransferPtr(device->newComputePipelineState(emissive_triangle_block_prefix.get(), &compile_error));
		cache->emissive_triangle_finalize_pipeline = NS::TransferPtr(device->newComputePipelineState(emissive_triangle_finalize.get(), &compile_error));
		cache->regir_scroll_pipeline = NS::TransferPtr(device->newComputePipelineState(regir_scroll.get(), &compile_error));
		cache->regir_classify_pipeline = NS::TransferPtr(device->newComputePipelineState(regir_classify.get(), &compile_error));
			cache->regir_reduce_pipeline = NS::TransferPtr(device->newComputePipelineState(regir_reduce.get(), &compile_error));
			cache->reusable_path_clear_staging_pipeline = NS::TransferPtr(device->newComputePipelineState(reusable_path_clear_staging.get(), &compile_error));
			cache->reusable_path_reduce_pipeline = NS::TransferPtr(device->newComputePipelineState(reusable_path_reduce.get(), &compile_error));
			if (!cache->trace_pipeline || !cache->shadow_pipeline || !cache->filter_pipeline || !cache->temporal_pipeline || !cache->split_reconstruction_pipeline || !cache->composite_pipeline || !cache->environment_build_pipeline || !cache->environment_reduce_pipeline || !cache->environment_diagnostic_pipeline || !cache->emissive_triangle_build_pipeline || !cache->emissive_triangle_block_scan_pipeline || !cache->emissive_triangle_block_prefix_pipeline || !cache->emissive_triangle_finalize_pipeline || !cache->regir_scroll_pipeline || !cache->regir_classify_pipeline || !cache->regir_reduce_pipeline || !cache->reusable_path_clear_staging_pipeline || !cache->reusable_path_reduce_pipeline) {
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid pipelines could not be created.", r_error);
		}
		NS::SharedPtr<MTL::SamplerDescriptor> sampler_descriptor = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
		// Ray queries estimate an explicit continuous LOD from their UV footprint.
		// Linear mip filtering preserves the fractional LOD instead of snapping
		// every hit to the nearest mip; no anisotropic sampler behavior is implied.
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterLinear);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeRepeat);
		cache->albedo_sampler = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		if (!cache->albedo_sampler) {
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid albedo sampler could not be created.", r_error);
		}
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		cache->environment_sampler = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		if (!cache->environment_sampler) {
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid environment sampler could not be created.", r_error);
		}
	}
	cache->frame++;
	MetalFluxWork *work = memnew(MetalFluxWork);
	work->bindless_material_textures = cache->bindless_material_textures;
	work->residency_completion = cache->completed_residency_token;
	work->residency_submission_token = cache->frame;
	work->trace_pipeline = cache->trace_pipeline;
	work->trace_classify_pipeline = cache->trace_classify_pipeline;
	work->trace_indirect_finalize_pipeline = cache->trace_indirect_finalize_pipeline;
	work->shadow_pipeline = cache->shadow_pipeline;
	work->alpha_trace_pipeline = cache->alpha_trace_pipeline;
	work->alpha_shadow_pipeline = cache->alpha_shadow_pipeline;
	work->alpha_intersection_function = cache->alpha_intersection_function;
	work->filter_pipeline = cache->filter_pipeline;
	work->temporal_pipeline = cache->temporal_pipeline;
	work->split_reconstruction_pipeline = cache->split_reconstruction_pipeline;
	work->composite_pipeline = cache->composite_pipeline;
	work->regir_scroll_pipeline = cache->regir_scroll_pipeline;
	work->regir_classify_pipeline = cache->regir_classify_pipeline;
	work->regir_reduce_pipeline = cache->regir_reduce_pipeline;
	work->reusable_path_clear_staging_pipeline = cache->reusable_path_clear_staging_pipeline;
	work->reusable_path_reduce_pipeline = cache->reusable_path_reduce_pipeline;
	// Flux has no production substitute for MetalFX image reconstruction. Raw
	// ray transport is composed directly when MetalFX is off; when it is on,
	// MetalFX alone owns image-space reconstruction and temporal state.
	work->split_reconstruction_enabled = false;
	work->transport_metadata_update_enabled = p_request.use_metalfx_denoiser && !p_request.shadow_only && !p_request.fresh_ray_oracle;
	r_result.metalfx_denoiser = p_request.use_metalfx_denoiser && !p_request.shadow_only;
	r_result.flux_image_reconstruction = work->split_reconstruction_enabled;
	if (!p_request.shadow_only && p_request.regir_direct_reuse && !METAL_FLUX_REGIR_SUPPORTED) {
		WARN_PRINT_ONCE("Flux: ReGIR is disabled until its complete proposal-PDF contract is validated.");
	}
	if (!p_request.shadow_only && p_request.reusable_path_reuse && !METAL_FLUX_REUSABLE_PATH_SUPPORTED) {
		WARN_PRINT_ONCE("Flux: reusable path transport is disabled until current-measure endpoint transport is validated.");
	}
	if (!p_request.shadow_only && p_request.restir_di_reuse && !METAL_FLUX_RESTIR_DI_TEMPORAL_SPATIAL_REUSE_SUPPORTED) {
		WARN_PRINT_ONCE("Flux: temporal/spatial ReSTIR DI reuse is disabled until receiver-dependent proposal PDFs are validated.");
	}
	work->environment_build_pipeline = cache->environment_build_pipeline;
	work->environment_reduce_pipeline = cache->environment_reduce_pipeline;
	work->environment_diagnostic_pipeline = cache->environment_diagnostic_pipeline;
	work->emissive_triangle_build_pipeline = cache->emissive_triangle_build_pipeline;
	work->emissive_triangle_block_scan_pipeline = cache->emissive_triangle_block_scan_pipeline;
	work->emissive_triangle_block_prefix_pipeline = cache->emissive_triangle_block_prefix_pipeline;
	work->emissive_triangle_finalize_pipeline = cache->emissive_triangle_finalize_pipeline;
	work->albedo_sampler = cache->albedo_sampler;
	work->environment_sampler = cache->environment_sampler;
	work->material_diagnostic = std::make_shared<MetalFluxMaterialDiagnosticCapture>();
	work->material_diagnostic->values = NS::TransferPtr(device->newBuffer(sizeof(uint32_t) * MATERIAL_DIAGNOSTIC_WORD_COUNT, MTL::ResourceStorageModeShared));
	work->material_diagnostic->shadow_only = p_request.shadow_only;
	work->trace_compaction_active = !p_request.shadow_only && work->trace_classify_pipeline && work->trace_indirect_finalize_pipeline;
	work->trace_compaction_fallback = !p_request.shadow_only && !work->trace_compaction_active;
	work->material_diagnostic->stage_probe = p_request.collect_stage_probe;
	work->material_diagnostic->report_metalfx_reactive_coverage = !p_request.shadow_only && p_request.use_metalfx_denoiser && p_request.collect_metalfx_reactive_telemetry && p_request.report_metalfx_reactive_coverage;
	work->material_diagnostic->diagnostics_owner_id = p_request.diagnostics_owner_id;
	work->material_diagnostic->diagnostics_frame = p_request.diagnostics_frame;
	work->material_diagnostic->submission_frame = p_request.metalfx_diagnostic_submission_index;
	if (!work->material_diagnostic->values) {
		memdelete(work);
		return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid material diagnostic buffer could not be created.", r_error);
	}
	memset(work->material_diagnostic->values->contents(), 0, sizeof(uint32_t) * MATERIAL_DIAGNOSTIC_WORD_COUNT);
	cache->material_diagnostic_captures.push_back(work->material_diagnostic);
	if (!cache->environment_fallback_radiance || !cache->environment_fallback_importance) {
		NS::SharedPtr<MTL::TextureDescriptor> radiance_descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
		radiance_descriptor->setTextureType(MTL::TextureType2D);
		radiance_descriptor->setPixelFormat(MTL::PixelFormatRGBA32Float);
		radiance_descriptor->setWidth(1);
		radiance_descriptor->setHeight(1);
		radiance_descriptor->setUsage(MTL::TextureUsageShaderRead);
		radiance_descriptor->setStorageMode(MTL::StorageModeShared);
		cache->environment_fallback_radiance = NS::TransferPtr(device->newTexture(radiance_descriptor.get()));
		NS::SharedPtr<MTL::TextureDescriptor> importance_descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
		importance_descriptor->setTextureType(MTL::TextureType2D);
		importance_descriptor->setPixelFormat(MTL::PixelFormatR32Float);
		importance_descriptor->setWidth(1);
		importance_descriptor->setHeight(1);
		importance_descriptor->setUsage(MTL::TextureUsageShaderRead);
		importance_descriptor->setStorageMode(MTL::StorageModeShared);
		cache->environment_fallback_importance = NS::TransferPtr(device->newTexture(importance_descriptor.get()));
		if (!cache->environment_fallback_radiance || !cache->environment_fallback_importance) {
			memdelete(work);
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid fallback environment textures could not be created.", r_error);
		}
		const float black_radiance[4] = {};
		const float black_weight = 0.0f;
		cache->environment_fallback_radiance->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, black_radiance, sizeof(black_radiance));
		cache->environment_fallback_importance->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, &black_weight, sizeof(black_weight));
	}
	work->environment_radiance = cache->environment_fallback_radiance.get();
	work->full_environment_radiance = cache->environment_fallback_radiance.get();
	work->environment_importance = cache->environment_fallback_importance;
	const bool stbn_requested = !p_request.shadow_only && p_request.sampling_sequence_mode == RendererPathTracing::SAMPLE_SEQUENCE_MODE_SPATIOTEMPORAL_BLUE_NOISE;
	if (stbn_requested && !cache->stbn_scalar_volume_attempted) {
		cache->stbn_scalar_volume_attempted = true;
		Vector<uint16_t> ranks;
		ranks.resize(RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_TEXEL_COUNT);
		if (RendererPathTracing::sample_sequence_generate_stbn_scalar_volume(ranks.ptrw(), ranks.size())) {
			NS::SharedPtr<MTL::TextureDescriptor> descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
			descriptor->setTextureType(MTL::TextureType2DArray);
			descriptor->setPixelFormat(MTL::PixelFormatR16Uint);
			descriptor->setWidth(RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH);
			descriptor->setHeight(RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT);
			descriptor->setArrayLength(RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH * RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT);
			descriptor->setUsage(MTL::TextureUsageShaderRead);
			descriptor->setStorageMode(MTL::StorageModeShared);
			cache->stbn_scalar_volume = NS::TransferPtr(device->newTexture(descriptor.get()));
			if (cache->stbn_scalar_volume) {
				const MTL::Region region = MTL::Region::Make2D(0, 0, RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH, RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT);
				const NS::UInteger bytes_per_row = RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH * sizeof(uint16_t);
				const NS::UInteger bytes_per_image = RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT * sizeof(uint16_t);
				for (uint32_t slice = 0; slice < RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH * RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT; slice++) {
					cache->stbn_scalar_volume->replaceRegion(region, 0, slice, ranks.ptr() + size_t(slice) * RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_SLICE_TEXEL_COUNT, bytes_per_row, bytes_per_image);
				}
				cache->stbn_scalar_volume_checksum = RendererPathTracing::sample_sequence_stbn_scalar_volume_checksum(ranks.ptr(), ranks.size());
				print_line(vformat("Flux sampling: generated scalar STBN volume v%d %dx%dx%dx%d checksum=%d.", RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_VERSION, RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH, RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT, RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH, RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT, cache->stbn_scalar_volume_checksum));
			}
		}
		if (!cache->stbn_scalar_volume) {
			WARN_PRINT_ONCE("Flux sampling: scalar STBN volume is unavailable; using progressive Owen-scrambled low-discrepancy sampling.");
		}
	}
	work->stbn_scalar_volume = cache->stbn_scalar_volume;
	work->shadow_only = p_request.shadow_only;
	work->metalfx_denoiser = p_request.use_metalfx_denoiser && !p_request.shadow_only;
	Vector<RID> sampled_texture_resources;
	if (p_request.environment.active && !p_request.shadow_only) {
		const RendererPathTracing::EnvironmentImportanceMetadata &metadata = p_request.environment.metadata;
		MTL::Texture *radiance = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_request.environment.sharp_radiance));
		MTL::Texture *full_radiance = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_request.environment.full_sharp_radiance));
		if (!radiance || !full_radiance || metadata.width == 0 || metadata.height == 0 || metadata.border < 0.0f || metadata.border >= 0.5f) {
			memdelete(work);
			return _hybrid_fail(ERR_INVALID_PARAMETER, "Environment importance source is not a valid sharp radiance texture.", r_error);
		}
		const Vector2i importance_resolution = _environment_importance_resolution(metadata.width, metadata.height);
		uint64_t key = metadata.distribution_key();
		key = _mix_transport_identity(key, uint32_t(importance_resolution.x));
		key = _mix_transport_identity(key, uint32_t(importance_resolution.y));
		const RendererPathTracing::EnvironmentImportancePaddedExtent extent = RendererPathTracing::environment_importance_padded_extent(importance_resolution.x, importance_resolution.y);
		const bool allocation_changed = !cache->environment_importance || cache->environment_importance->width() != extent.width || cache->environment_importance->height() != extent.height || cache->environment_importance->mipmapLevelCount() != extent.mip_count;
		if (allocation_changed) {
			NS::SharedPtr<MTL::TextureDescriptor> descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
			descriptor->setTextureType(MTL::TextureType2D);
			descriptor->setPixelFormat(MTL::PixelFormatR32Float);
			descriptor->setWidth(extent.width);
			descriptor->setHeight(extent.height);
			descriptor->setMipmapLevelCount(extent.mip_count);
			descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
			descriptor->setStorageMode(MTL::StorageModePrivate);
			cache->environment_importance = NS::TransferPtr(device->newTexture(descriptor.get()));
			if (!cache->environment_importance) {
				memdelete(work);
				return _hybrid_fail(ERR_CANT_CREATE, "Metal environment importance pyramid could not be created.", r_error);
			}
			cache->environment_mip_count = extent.mip_count;
		}
		if (allocation_changed || cache->environment_distribution_key != key) {
			work->environment_rebuild = true;
			std::shared_ptr<MetalFluxEnvironmentDiagnosticCapture> diagnostic = std::make_shared<MetalFluxEnvironmentDiagnosticCapture>();
			diagnostic->values = NS::TransferPtr(device->newBuffer(ENVIRONMENT_DIAGNOSTIC_WORD_COUNT * sizeof(uint32_t), MTL::ResourceStorageModeShared));
			if (!diagnostic->values) {
				memdelete(work);
				return _hybrid_fail(ERR_CANT_CREATE, "Metal environment rebuild diagnostic buffer could not be created.", r_error);
			}
			memset(diagnostic->values->contents(), 0, ENVIRONMENT_DIAGNOSTIC_WORD_COUNT * sizeof(uint32_t));
			diagnostic->source_id = metadata.source_id;
			diagnostic->generation = metadata.generation;
			diagnostic->checksum = metadata.checksum();
			diagnostic->width = metadata.width;
			diagnostic->height = metadata.height;
			work->environment_diagnostic = diagnostic;
			cache->environment_diagnostic_captures.push_back(diagnostic);
			cache->environment_distribution_key = key;
			r_result.environment.cache_decision = RendererPathTracing::ENVIRONMENT_IMPORTANCE_CACHE_REBUILT;
			r_result.environment.cache_reason = allocation_changed ? "importance pyramid allocation changed" : "bounded transport revision changed";
		} else {
			r_result.environment.cache_decision = RendererPathTracing::ENVIRONMENT_IMPORTANCE_CACHE_REUSED;
			r_result.environment.cache_reason = "sharp radiance identity unchanged";
		}
		work->environment_radiance = radiance;
		work->full_environment_radiance = full_radiance;
		work->environment_importance = cache->environment_importance;
		sampled_texture_resources.push_back(p_request.environment.sharp_radiance);
		if (p_request.environment.full_sharp_radiance != p_request.environment.sharp_radiance) {
			sampled_texture_resources.push_back(p_request.environment.full_sharp_radiance);
		}
		r_result.environment.status = RendererPathTracing::ENVIRONMENT_IMPORTANCE_ACTIVE;
		r_result.environment.status_reason = "active sharp renderer-owned sky radiance";
		r_result.environment.source_id = metadata.source_id;
		r_result.environment.generation = metadata.generation;
		r_result.environment.checksum = metadata.checksum();
		r_result.environment.weight_state = work->environment_rebuild ? "pending one-shot GPU rebuild diagnostic" : "GPU distribution reused";
		r_result.environment.radiance_width = metadata.width;
		r_result.environment.radiance_height = metadata.height;
		r_result.environment.proposal_width = importance_resolution.x;
		r_result.environment.proposal_height = importance_resolution.y;
	}
	HashMap<uint64_t, uint32_t> surface_indices;
	HashMap<uint64_t, bool> alpha_mask_surfaces;
	Vector<uint64_t> deferred_blas_entries;
	uint32_t scheduled_blas_builds = 0;
	uint64_t scheduled_blas_build_triangles = 0;
	const uint64_t completed_submission_serial = cache->completed_residency_token->load(std::memory_order_acquire);
	// Native compaction is deliberately two submissions: size readback becomes
	// CPU-visible only after the build completes, and the compact copy becomes
	// active only after its own completion. The old immutable AS remains the
	// TLAS target throughout both pending phases.
	for (KeyValue<uint64_t, MetalFluxCachedGeometry *> &entry : cache->geometries) {
		MetalFluxCachedGeometry *geometry = entry.value;
		if (geometry->compaction_copy_pending && geometry->compaction_copy_serial <= completed_submission_serial && geometry->pending_compacted_structure) {
			if (geometry->acceleration_structure) cache->retired_acceleration_structures.push_back({ geometry->acceleration_structure, geometry->last_use_serial });
			geometry->acceleration_structure = geometry->pending_compacted_structure;
			geometry->pending_compacted_structure.reset();
			geometry->compaction_copy_pending = false;
			geometry->compaction_copy_serial = 0;
			r_result.blas_compaction_swaps++;
		}
		if (geometry->compaction_query_pending && geometry->compaction_query_serial <= completed_submission_serial && geometry->compacted_size_readback && !geometry->compaction_copy_pending && geometry->acceleration_structure) {
			const uint64_t compacted_size = *static_cast<const uint64_t *>(geometry->compacted_size_readback->contents());
			const uint64_t source_size = geometry->acceleration_structure->allocatedSize();
			geometry->compaction_query_pending = false;
			geometry->compaction_query_serial = 0;
			if (compacted_size > 0 && compacted_size < source_size) {
				geometry->pending_compacted_structure = NS::TransferPtr(device->newAccelerationStructure(compacted_size));
				if (geometry->pending_compacted_structure) {
					work->blas_compaction_copy_sources.push_back(geometry->acceleration_structure);
					work->blas_compaction_copy_destinations.push_back(geometry->pending_compacted_structure);
					geometry->compaction_copy_pending = true;
					geometry->compaction_copy_serial = cache->frame;
					r_result.blas_compactions++;
					r_result.ray_group_uncompacted_bytes += source_size;
					r_result.ray_group_compacted_bytes += compacted_size;
				}
			}
		}
		r_result.ray_group_pending += geometry->compaction_query_pending || geometry->compaction_copy_pending;
	}
	for (int index = cache->retired_acceleration_structures.size() - 1; index >= 0; index--) {
		if (cache->retired_acceleration_structures[index].last_use_serial <= completed_submission_serial) {
			cache->retired_acceleration_structures.remove_at(index);
			r_result.blas_compaction_retirements++;
		}
	}
	for (const Instance &instance : p_request.instances) {
		if (instance.alpha_mode == Instance::ALPHA_MASK) {
			alpha_mask_surfaces.insert(instance.surface_id, true);
		}
	}
	for (uint32_t index = 0; index < (uint32_t)p_request.surfaces.size(); index++) {
		const Surface &surface = p_request.surfaces[index];
		if (!surface.vertex_buffer.is_valid() || surface.vertex_count < 3 || (surface.index_count && !surface.index_buffer.is_valid())) {
			continue;
		}
		MTL::Buffer *vertex = reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, surface.vertex_buffer));
		MTL::Buffer *indices = surface.index_buffer.is_valid() ? reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, surface.index_buffer)) : nullptr;
		MTL::Buffer *attributes = surface.has_uv && surface.attribute_buffer.is_valid() ? reinterpret_cast<MTL::Buffer *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, surface.attribute_buffer)) : nullptr;
		if (!vertex) {
			continue;
		}
		work->vertex_buffers.push_back(vertex);
		work->index_buffers.push_back(indices);
		work->attribute_buffers.push_back(attributes);
		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> triangle = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		triangle->setVertexBuffer(vertex);
		triangle->setVertexBufferOffset(surface.vertex_buffer_offset);
		triangle->setVertexFormat(surface.compressed ? MTL::AttributeFormatUShort4Normalized : MTL::AttributeFormatFloat3);
		triangle->setVertexStride(surface.vertex_stride);
		if (surface.compressed) {
			MTL::PackedFloat4x3 decode(MTL::PackedFloat3(surface.compressed_aabb.size.x, 0, 0), MTL::PackedFloat3(0, surface.compressed_aabb.size.y, 0), MTL::PackedFloat3(0, 0, surface.compressed_aabb.size.z), MTL::PackedFloat3(surface.compressed_aabb.position.x, surface.compressed_aabb.position.y, surface.compressed_aabb.position.z));
			NS::SharedPtr<MTL::Buffer> decode_buffer = NS::TransferPtr(device->newBuffer(&decode, sizeof(decode), MTL::ResourceStorageModeShared));
			triangle->setTransformationMatrixBuffer(decode_buffer.get());
			work->decode_transforms.push_back(decode_buffer);
		}
		if (indices) {
			triangle->setIndexBuffer(indices);
			triangle->setIndexBufferOffset(surface.index_buffer_offset);
			const bool uses_16_bit_indices = surface.index_stride == 2 || (surface.index_stride == 0 && surface.vertex_count <= 65536);
			triangle->setIndexType(uses_16_bit_indices ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32);
		}
		triangle->setTriangleCount((surface.index_count ? surface.index_count : surface.vertex_count) / 3);
		const bool surface_opaque = !alpha_mask_surfaces.has(surface.stable_id);
		triangle->setOpaque(surface_opaque);
		if (!surface_opaque) {
			triangle->setIntersectionFunctionTableOffset(0);
		}
		NS::Object *object = triangle.get();
		NS::SharedPtr<NS::Array> array = NS::TransferPtr(NS::Array::array(&object, 1)->retain());
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
		descriptor->setGeometryDescriptors(array.get());
		descriptor->setUsage(surface.dynamic ? MTL::AccelerationStructureUsageRefit : MTL::AccelerationStructureUsagePreferFastIntersection);
		MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(descriptor.get());
		MetalFluxCachedGeometry **cached_ptr = cache->geometries.getptr(surface.stable_id);
		MetalFluxCachedGeometry *cached = cached_ptr ? *cached_ptr : nullptr;
		uint8_t action = 1;
		if (!cached) {
			cached = memnew(MetalFluxCachedGeometry);
			cache->geometries.insert(surface.stable_id, cached);
		} else if (cached->topology_revision == surface.topology_revision && cached->opaque == surface_opaque && cached->acceleration_structure) {
			action = cached->deformation_revision == surface.deformation_revision ? 0 : (surface.dynamic ? 2 : 1);
		}
		const uint64_t triangle_count = triangle->triangleCount();
		const uint64_t remaining_triangle_budget = scheduled_blas_build_triangles < p_request.maximum_blas_build_triangles_per_frame ? p_request.maximum_blas_build_triangles_per_frame - scheduled_blas_build_triangles : 0;
		const bool build_budget_exceeded = action == 1 && scheduled_blas_builds > 0 && (scheduled_blas_builds >= p_request.maximum_blas_builds_per_frame || triangle_count > remaining_triangle_budget);
		if (build_budget_exceeded) {
			// Keep the existing cached resource alive and retry this surface next frame.
			// Raster primary visibility remains complete while the secondary ray world
			// converges without one unbounded acceleration-structure submission.
			cached->last_seen_frame = cache->frame;
			work->vertex_buffers.remove_at(work->vertex_buffers.size() - 1);
			work->index_buffers.remove_at(work->index_buffers.size() - 1);
			work->attribute_buffers.remove_at(work->attribute_buffers.size() - 1);
			if (surface.compressed) {
				work->decode_transforms.remove_at(work->decode_transforms.size() - 1);
			}
			r_result.blas_builds_deferred++;
			r_result.blas_build_triangles_deferred += triangle_count;
			uint64_t deferred_identity = _mix_transport_identity(surface.stable_id, surface.topology_revision);
			deferred_identity = _mix_transport_identity(deferred_identity, surface.deformation_revision);
			deferred_identity = _mix_transport_identity(deferred_identity, surface_opaque ? 1u : 0u);
			deferred_blas_entries.push_back(deferred_identity);
			continue;
		}
		if (action == 1) {
			scheduled_blas_builds++;
			scheduled_blas_build_triangles += triangle_count;
		}
		NS::SharedPtr<MTL::AccelerationStructure> source;
		if (action == 1) {
			source = cached->acceleration_structure;
			cached->acceleration_structure = NS::TransferPtr(device->newAccelerationStructure(sizes.accelerationStructureSize));
			cached->pending_compacted_structure.reset();
			cached->compaction_query_pending = false;
			cached->compaction_copy_pending = false;
		} else if (action == 2) {
			// Metal explicitly supports an in-place refit. Keeping the resource identity
			// stable also means an unchanged instance hierarchy can reuse its TLAS.
			source = cached->acceleration_structure;
		}
		cached->topology_revision = surface.topology_revision;
		cached->deformation_revision = surface.deformation_revision;
		cached->opaque = surface_opaque;
		cached->last_seen_frame = cache->frame;
		cached->last_use_serial = cache->frame;
		work->blas_descriptors.push_back(descriptor);
		work->blas.push_back(cached->acceleration_structure);
		work->blas_sources.push_back(source);
		work->blas_actions.push_back(action);
		uint64_t scratch_size = action == 2 ? sizes.refitScratchBufferSize : sizes.buildScratchBufferSize;
		work->blas_scratch.push_back(action == 0 ? NS::SharedPtr<MTL::Buffer>() : NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), scratch_size), MTL::ResourceStorageModePrivate)));
		surface_indices.insert(surface.stable_id, work->blas.size() - 1);
		if (action == 1) {
			r_result.scene.blas_built++;
			if (!surface.dynamic && cached->acceleration_structure) {
				cached->compacted_size_readback = NS::TransferPtr(device->newBuffer(sizeof(uint64_t), MTL::ResourceStorageModeShared));
				if (cached->compacted_size_readback) {
					*static_cast<uint64_t *>(cached->compacted_size_readback->contents()) = 0;
					cached->compaction_query_serial = cache->frame;
					cached->compaction_query_pending = true;
					work->blas_compaction_query_sources.push_back(cached->acceleration_structure);
					work->blas_compaction_query_buffers.push_back(cached->compacted_size_readback);
					r_result.blas_compaction_queries++;
				}
			}
		} else if (action == 2) {
			r_result.scene.blas_refit++;
		} else {
			r_result.scene.blas_reused++;
		}
	}
	// A virtual tier becomes TLAS-visible only when every surface in the group
	// has a complete immutable BLAS. Choose one highest complete tier per stable
	// transport region; all other group instances are omitted atomically.
	HashMap<uint64_t, uint32_t> ray_group_expected;
	HashMap<uint64_t, uint32_t> ray_group_ready;
	HashMap<uint64_t, const Surface *> ray_group_record;
	for (const Surface &surface : p_request.surfaces) {
		if (surface.ray_group_id == 0) continue;
		ray_group_expected[surface.ray_group_id]++;
		if (surface_indices.has(surface.stable_id)) ray_group_ready[surface.ray_group_id]++;
		ray_group_record.insert(surface.ray_group_id, &surface);
	}
	HashMap<uint64_t, uint64_t> selected_group_by_region;
	HashMap<uint64_t, uint32_t> selected_tier_by_region;
	for (const KeyValue<uint64_t, uint32_t> &entry : ray_group_expected) {
		const uint32_t *ready = ray_group_ready.getptr(entry.key);
		const Surface *const *record = ray_group_record.getptr(entry.key);
		if (!ready || *ready != entry.value || !record) {
			r_result.ray_group_pending++;
			continue;
		}
		const Surface &surface = **record;
		uint32_t *selected_tier = selected_tier_by_region.getptr(surface.transport_region_id);
		if (!selected_tier || surface.ray_tier > *selected_tier) {
			selected_tier_by_region.insert(surface.transport_region_id, surface.ray_tier);
			selected_group_by_region.insert(surface.transport_region_id, surface.ray_group_id);
		}
	}
	for (const Surface &surface : p_request.surfaces) {
		if (surface.ray_group_id == 0) continue;
		const uint64_t *selected = selected_group_by_region.getptr(surface.transport_region_id);
		if (!selected || *selected != surface.ray_group_id) surface_indices.erase(surface.stable_id);
	}
	for (const KeyValue<uint64_t, uint32_t> &entry : selected_tier_by_region) {
		switch (entry.value) {
			case 2: r_result.ray_group_near++; break;
			case 1: r_result.ray_group_middle++; break;
			default: r_result.ray_group_far++; break;
		}
		if (entry.value < 2) r_result.ray_group_coarse_fallbacks++;
	}
	for (const Instance &instance : p_request.instances) if (instance.ray_group_id != 0 && surface_indices.has(instance.surface_id) && instance.off_screen_transport) r_result.ray_group_off_screen_retained++;
	Vector<uint64_t> retired_geometries;
	for (const KeyValue<uint64_t, MetalFluxCachedGeometry *> &entry : cache->geometries) {
		if (entry.value->last_seen_frame == cache->frame) {
			continue;
		}
		if (entry.value->last_seen_frame + 3 <= cache->frame) {
			retired_geometries.push_back(entry.key);
		} else {
			r_result.scene.pending_retirements++;
		}
	}
	for (uint64_t stable_id : retired_geometries) {
		MetalFluxCachedGeometry **geometry = cache->geometries.getptr(stable_id);
		if (geometry) {
			memdelete(*geometry);
			cache->geometries.erase(stable_id);
			r_result.scene.retired++;
		}
	}
	Vector<MTL::AccelerationStructure *> blas_ptrs;
	for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : work->blas) {
		blas_ptrs.push_back(blas.get());
	}
	work->blas_array = NS::TransferPtr(NS::Array::array(reinterpret_cast<NS::Object *const *>(blas_ptrs.ptr()), blas_ptrs.size())->retain());
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> metal_instances;
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> opaque_metal_instances;
	Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> alpha_metal_instances;
	Vector<MetalFluxMaterial> metal_materials;
	Vector<MetalFluxGeometry> metal_geometries;
	Vector<MetalFluxEmissive> metal_emissives;
	Vector<MetalFluxBidirectionalCausticMirrorTriangle> metal_bidirectional_caustic_mirrors;
	Vector<float> emissive_weights;
	HashMap<uint64_t, const Surface *> surface_records;
	for (const Surface &surface : p_request.surfaces) {
		surface_records.insert(surface.stable_id, &surface);
	}
	// Tier-2 argument buffers carry individual renderer-owned texture views. The
	// texture remains BC-compressed and keeps the sRGB/linear view selected by
	// material extraction; this is a resource table, not a texture array copy.
	const bool texture_bindings_supported = rd->limit_get(RD::LIMIT_MAX_TEXTURES_PER_SHADER_STAGE) >= 50u;
	RID default_albedo_texture;
	MTL::Texture *default_albedo = nullptr;
	if (texture_bindings_supported) {
		TextureStorage *texture_storage = TextureStorage::get_singleton();
		if (texture_storage) {
			default_albedo_texture = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
			default_albedo = default_albedo_texture.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, default_albedo_texture)) : nullptr;
		} else {
			// Standalone RenderingDevice tests do not construct the renderer-owned
			// TextureStorage singleton. Keep the native binding legal without
			// changing the normal renderer path or requiring a fake renderer lifecycle.
			if (!cache->standalone_fallback_albedo) {
				NS::SharedPtr<MTL::TextureDescriptor> descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
				descriptor->setTextureType(MTL::TextureType2D);
				descriptor->setPixelFormat(MTL::PixelFormatRGBA32Float);
				descriptor->setWidth(1);
				descriptor->setHeight(1);
				descriptor->setUsage(MTL::TextureUsageShaderRead);
				descriptor->setStorageMode(MTL::StorageModeShared);
				cache->standalone_fallback_albedo = NS::TransferPtr(device->newTexture(descriptor.get()));
				if (cache->standalone_fallback_albedo) {
					const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
					cache->standalone_fallback_albedo->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, white, sizeof(white));
				}
			}
			default_albedo = cache->standalone_fallback_albedo.get();
		}
	}
	if (!default_albedo) {
		memdelete(work);
		return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid material fallback texture is unavailable.", r_error);
	}
	if (default_albedo_texture.is_valid()) {
		sampled_texture_resources.push_back(default_albedo_texture);
	}

	using RendererPathTracing::HybridResidencyPool;
	using RendererPathTracing::HybridResidencyResourceKind;
	using RendererPathTracing::HybridResidencyResourceRequest;
	using RendererPathTracing::HybridResidencyRetention;
	using RendererPathTracing::HybridResidencyState;
	using RendererPathTracing::HybridResidencyTextureChannel;
	const uint64_t cpu_residency_begin = mach_absolute_time();
	if (!cache->material_residency_configured) {
		for (uint32_t pool = 0; pool < static_cast<uint32_t>(HybridResidencyPool::MAX); pool++) {
			cache->residency_budgets.pools[pool].maximum_resident_bytes = 4ull * 1024ull * 1024ull * 1024ull;
			cache->residency_budgets.pools[pool].maximum_upload_bytes_per_frame = 4ull * 1024ull * 1024ull * 1024ull;
			cache->residency_budgets.pools[pool].maximum_resident_slots = 65536;
			cache->residency_budgets.pools[pool].maximum_requests_per_frame = 65536;
			cache->residency_budgets.pools[pool].maximum_uploads_per_frame = 65536;
		}
		const uint32_t texture_pool = static_cast<uint32_t>(HybridResidencyPool::TEXTURE_MIPS);
		cache->residency_budgets.pools[texture_pool].maximum_resident_slots = cache->material_texture_capacity;
		cache->residency_budgets.pools[texture_pool].maximum_uploads_per_frame = cache->material_texture_capacity;
		cache->residency_budgets.pools[texture_pool].maximum_requests_per_frame = cache->material_texture_capacity;
		if (cache->residency_planner.set_budgets(cache->residency_budgets) != OK) {
			memdelete(work);
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid residency budgets could not be configured.", r_error);
		}
		cache->material_residency_configured = true;
	}
	const uint8_t eye_mask = p_request.views.size() > 1 ? RendererPathTracing::HYBRID_RESIDENCY_EYE_STEREO : RendererPathTracing::HYBRID_RESIDENCY_EYE_LEFT;
	uint64_t residency_plan_identity = 0x7615af8d3e2c4901ULL;
	auto mix_residency_plan_identity = [&](uint64_t p_value) {
		residency_plan_identity = _mix_transport_identity(residency_plan_identity, p_value);
	};
	mix_residency_plan_identity(cache->material_texture_capacity);
	mix_residency_plan_identity(cache->bindless_material_textures ? 1u : 0u);
	mix_residency_plan_identity(eye_mask);
	mix_residency_plan_identity(uint64_t(reinterpret_cast<uintptr_t>(default_albedo)));
	for (uint32_t pool = 0; pool < static_cast<uint32_t>(HybridResidencyPool::MAX); pool++) {
		const RendererPathTracing::HybridResidencyPoolBudget &budget = cache->residency_budgets.pools[pool];
		mix_residency_plan_identity(budget.maximum_resident_bytes);
		mix_residency_plan_identity(budget.maximum_upload_bytes_per_frame);
		mix_residency_plan_identity(budget.maximum_resident_slots);
		mix_residency_plan_identity(budget.maximum_requests_per_frame);
		mix_residency_plan_identity(budget.maximum_uploads_per_frame);
	}
	bool stable_residency_inputs_ready = deferred_blas_entries.is_empty() && cache->retired_acceleration_structures.is_empty() && work->blas_compaction_query_sources.is_empty() && work->blas_compaction_copy_sources.is_empty();
	for (const Surface &surface : p_request.surfaces) {
		mix_residency_plan_identity(surface.stable_id);
		mix_residency_plan_identity(surface.topology_revision);
		mix_residency_plan_identity(surface.deformation_revision);
		mix_residency_plan_identity(surface.vertex_buffer.get_id());
		mix_residency_plan_identity(surface.index_buffer.get_id());
		mix_residency_plan_identity(surface.attribute_buffer.get_id());
		mix_residency_plan_identity(surface.vertex_buffer_offset);
		mix_residency_plan_identity(surface.index_buffer_offset);
		mix_residency_plan_identity(surface.attribute_buffer_offset);
		const uint32_t *surface_index = surface_indices.getptr(surface.stable_id);
		if (!surface_index) {
			stable_residency_inputs_ready = false;
			continue;
		}
		mix_residency_plan_identity(uint64_t(reinterpret_cast<uintptr_t>(work->vertex_buffers[*surface_index])));
		mix_residency_plan_identity(uint64_t(reinterpret_cast<uintptr_t>(work->index_buffers[*surface_index])));
		mix_residency_plan_identity(uint64_t(reinterpret_cast<uintptr_t>(work->attribute_buffers[*surface_index])));
		mix_residency_plan_identity(uint64_t(reinterpret_cast<uintptr_t>(work->blas[*surface_index].get())));
		mix_residency_plan_identity(work->blas_actions[*surface_index]);
		stable_residency_inputs_ready &= work->blas_actions[*surface_index] == 0u;
	}
	for (uint64_t deferred_identity : deferred_blas_entries) mix_residency_plan_identity(deferred_identity);
	auto mix_instance_texture = [&](RID p_rid, HybridResidencyTextureChannel p_channel) {
		mix_residency_plan_identity(p_rid.get_id());
		mix_residency_plan_identity(static_cast<uint32_t>(p_channel));
	};
	for (const Instance &instance : p_request.instances) {
		mix_residency_plan_identity(instance.stable_id);
		mix_residency_plan_identity(instance.surface_id);
		mix_residency_plan_identity(instance.material_stable_id);
		mix_residency_plan_identity(instance.material_generation);
		mix_residency_plan_identity(instance.visibility_mask);
		mix_residency_plan_identity(instance.face_flags);
		mix_residency_plan_identity(static_cast<uint32_t>(instance.alpha_mode));
		mix_instance_texture(instance.albedo_texture, HybridResidencyTextureChannel::ALBEDO);
		mix_instance_texture(instance.normal_texture, HybridResidencyTextureChannel::NORMAL);
		mix_instance_texture(instance.orm_texture, HybridResidencyTextureChannel::ORM);
		mix_instance_texture(instance.metallic_texture, HybridResidencyTextureChannel::ORM);
		mix_instance_texture(instance.roughness_texture, HybridResidencyTextureChannel::ORM);
		mix_instance_texture(instance.ambient_occlusion_texture, HybridResidencyTextureChannel::ORM);
		mix_instance_texture(instance.emission_texture, HybridResidencyTextureChannel::EMISSIVE);
		mix_instance_texture(instance.opacity_texture, HybridResidencyTextureChannel::OPACITY);
		mix_instance_texture(instance.alpha_occupancy_texture, HybridResidencyTextureChannel::ALPHA_OCCUPANCY);
	}
	for (const KeyValue<uint64_t, MetalFluxCachedGeometry *> &entry : cache->geometries) {
		stable_residency_inputs_ready &= !entry.value->compaction_query_pending && !entry.value->compaction_copy_pending;
	}
	auto stable_residency_textures_ready = [&]() {
		for (const MetalFluxResidencyTexturePlanEntry &entry : cache->stable_residency_plan.textures) {
			MTL::Texture *texture = entry.rid.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, entry.rid)) : nullptr;
			if (texture != entry.texture || !texture || texture->textureType() != MTL::TextureType2D || texture->sampleCount() != 1) return false;
		}
		return true;
	};
	const uint64_t completed_residency_token = cache->completed_residency_token->load(std::memory_order_acquire);
	const bool stable_residency_plan_hit = stable_residency_inputs_ready && cache->stable_residency_plan.valid && cache->stable_residency_plan.complete && cache->stable_residency_plan.identity == residency_plan_identity && stable_residency_textures_ready() && cache->residency_planner.reuse_committed_frame(cache->frame, completed_residency_token, cache->stable_residency_plan.visible_keys);
	if (stable_residency_plan_hit) {
		cache->stable_residency_plan_hits++;
	} else {
		cache->stable_residency_plan_misses++;
		cache->stable_residency_plan_rebuilds++;
		if (cache->residency_planner.begin_frame(cache->frame, completed_residency_token) != OK) {
			memdelete(work);
			return _hybrid_fail(ERR_BUSY, "Metal hybrid residency planner could not begin the frame.", r_error);
		}
	}

	Vector<MetalFluxResidencyTexturePlanEntry> texture_plan;
	HashMap<uint64_t, int> texture_plan_indices;
	Vector<RendererPathTracing::HybridResidencyResourceKey> stable_residency_keys;
	RendererPathTracing::HybridResidencyCommitResult residency_commit;
	Vector<uint64_t> visibility_residency_entries;
	bool visibility_residency_complete = false;
	r_result.residency_completion_token = completed_residency_token;
	if (stable_residency_plan_hit) {
		texture_plan = cache->stable_residency_plan.textures;
		stable_residency_keys = cache->stable_residency_plan.visible_keys;
		visibility_residency_complete = cache->stable_residency_plan.complete;
		r_result.visibility_residency_generation = cache->stable_residency_plan.visibility_generation;
		memcpy(r_result.material_texture_requested, cache->stable_residency_plan.requested, sizeof(r_result.material_texture_requested));
		memcpy(r_result.material_texture_resident, cache->stable_residency_plan.resident, sizeof(r_result.material_texture_resident));
		memcpy(r_result.material_texture_misses, cache->stable_residency_plan.misses, sizeof(r_result.material_texture_misses));
		r_result.texture_fallbacks = cache->stable_residency_plan.texture_fallbacks;
	} else {
	auto append_texture_resource = [&](Vector<HybridResidencyResourceRequest> &r_resources, RID p_rid, HybridResidencyTextureChannel p_channel) {
		if (!p_rid.is_valid()) {
			return;
		}
		const uint64_t plan_key = (uint64_t(p_rid.get_id()) << 8u) | uint64_t(p_channel);
		const int *existing_plan_index = texture_plan_indices.getptr(plan_key);
		int plan_index = existing_plan_index ? *existing_plan_index : -1;
		if (plan_index < 0) {
			MetalFluxResidencyTexturePlanEntry entry;
			entry.rid = p_rid;
			entry.channel = p_channel;
			entry.generation = MAX(uint64_t(1), p_rid.get_id());
			entry.texture = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_rid));
			if (entry.texture && entry.texture->textureType() == MTL::TextureType2D && entry.texture->sampleCount() == 1) {
				entry.bytes = MAX(uint64_t(1), uint64_t(entry.texture->allocatedSize()));
			} else {
				entry.texture = nullptr;
			}
			texture_plan.push_back(entry);
			plan_index = texture_plan.size() - 1;
			texture_plan_indices.insert(plan_key, plan_index);
			r_result.material_texture_requested[static_cast<uint32_t>(p_channel)]++;
		}
		const MetalFluxResidencyTexturePlanEntry &entry = texture_plan[plan_index];
		HybridResidencyResourceRequest resource;
		resource.key.kind = HybridResidencyResourceKind::TEXTURE_MIP;
		resource.key.texture_channel = p_channel;
		resource.key.stable_id = MAX(uint64_t(1), p_rid.get_id());
		resource.key.generation = entry.generation;
		resource.bytes = entry.bytes;
		resource.retention = HybridResidencyRetention::STREAMABLE;
		resource.available = entry.texture != nullptr;
		r_resources.push_back(resource);
	};

	for (const Instance &instance : p_request.instances) {
		const uint32_t *surface_index = surface_indices.getptr(instance.surface_id);
		const Surface *const *surface_ptr = surface_records.getptr(instance.surface_id);
		if (!surface_index || !surface_ptr) {
			continue;
		}
		RendererPathTracing::HybridResidencyRequest residency_request;
		residency_request.request_id = MAX(uint64_t(1), instance.stable_id);
		residency_request.priority = instance.alpha_mode == Instance::ALPHA_MASK ? 2 : 1;
		residency_request.eye_mask = eye_mask;
		HybridResidencyResourceRequest material_resource;
		material_resource.key.kind = HybridResidencyResourceKind::MATERIAL_DESCRIPTOR;
		material_resource.key.stable_id = MAX(uint64_t(1), instance.material_stable_id);
		material_resource.key.generation = MAX(uint64_t(1), instance.material_generation);
		material_resource.bytes = sizeof(MetalFluxMaterial);
		material_resource.retention = HybridResidencyRetention::STREAMABLE;
		residency_request.resources.push_back(material_resource);
		const Surface &surface = **surface_ptr;
		HybridResidencyResourceRequest geometry_resource;
		geometry_resource.key.kind = HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE;
		geometry_resource.key.stable_id = MAX(uint64_t(1), surface.stable_id);
		geometry_resource.key.generation = MAX(uint64_t(1), surface.topology_revision);
		geometry_resource.bytes = MAX(uint64_t(1), uint64_t(work->vertex_buffers[*surface_index]->allocatedSize()) + (work->index_buffers[*surface_index] ? uint64_t(work->index_buffers[*surface_index]->allocatedSize()) : 0) + (work->attribute_buffers[*surface_index] ? uint64_t(work->attribute_buffers[*surface_index]->allocatedSize()) : 0));
		geometry_resource.retention = HybridResidencyRetention::STREAMABLE;
		residency_request.resources.push_back(geometry_resource);
		HybridResidencyResourceRequest blas_resource;
		blas_resource.key.kind = HybridResidencyResourceKind::BLAS;
		blas_resource.key.stable_id = geometry_resource.key.stable_id;
		// A refit updates the BLAS contents in place. Residency generations identify
		// allocations, so deformation must not churn this key.
		blas_resource.key.generation = MAX(uint64_t(1), surface.topology_revision);
		blas_resource.bytes = MAX(uint64_t(1), uint64_t(work->blas[*surface_index]->allocatedSize()));
		blas_resource.retention = HybridResidencyRetention::STREAMABLE;
		residency_request.resources.push_back(blas_resource);
		HybridResidencyResourceRequest tlas_resource;
		tlas_resource.key.kind = HybridResidencyResourceKind::TLAS_INSTANCE;
		tlas_resource.key.stable_id = MAX(uint64_t(1), instance.stable_id);
		tlas_resource.key.generation = 1;
		tlas_resource.bytes = sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor);
		tlas_resource.retention = HybridResidencyRetention::STREAMABLE;
		residency_request.resources.push_back(tlas_resource);
		append_texture_resource(residency_request.resources, instance.albedo_texture, HybridResidencyTextureChannel::ALBEDO);
		append_texture_resource(residency_request.resources, instance.normal_texture, HybridResidencyTextureChannel::NORMAL);
		append_texture_resource(residency_request.resources, instance.orm_texture, HybridResidencyTextureChannel::ORM);
		append_texture_resource(residency_request.resources, instance.metallic_texture, HybridResidencyTextureChannel::ORM);
		append_texture_resource(residency_request.resources, instance.roughness_texture, HybridResidencyTextureChannel::ORM);
		append_texture_resource(residency_request.resources, instance.ambient_occlusion_texture, HybridResidencyTextureChannel::ORM);
		append_texture_resource(residency_request.resources, instance.emission_texture, HybridResidencyTextureChannel::EMISSIVE);
		append_texture_resource(residency_request.resources, instance.opacity_texture, HybridResidencyTextureChannel::OPACITY);
		append_texture_resource(residency_request.resources, instance.alpha_occupancy_texture, HybridResidencyTextureChannel::ALPHA_OCCUPANCY);
		for (const HybridResidencyResourceRequest &resource : residency_request.resources) stable_residency_keys.push_back(resource.key);
		if (cache->residency_planner.request(residency_request) != OK) {
			memdelete(work);
			return _hybrid_fail(ERR_INVALID_DATA, "Metal hybrid residency request metadata is invalid.", r_error);
		}
	}
	residency_commit = cache->residency_planner.commit();
	for (const RendererPathTracing::HybridResidencyRetirement &retirement : residency_commit.retirements) {
		cache->residency_planner.retire(retirement.key, cache->frame);
	}
	// This is intentionally built only after commit. It represents the resource
	// keys and states the submitted ray scene can actually observe, rather than
	// CPU planning order or camera-derived selection inputs.
	visibility_residency_entries = deferred_blas_entries;
	// Residency completion is a readiness condition, not an identity input. The
	// committed resource state below already captures actual admitted/stale
	// changes; asynchronously retired command buffers must not churn history.
	visibility_residency_complete = deferred_blas_entries.is_empty();
	auto append_residency_query = [&](const RendererPathTracing::HybridResidencyResourceKey &p_key) {
		const RendererPathTracing::HybridResidencyQuery query = cache->residency_planner.query(p_key);
		visibility_residency_complete &= query.state == HybridResidencyState::RESIDENT && !query.fail_open_required;
		uint64_t identity = _transport_residency_key_identity(p_key);
		identity = _mix_transport_identity(identity, _transport_residency_key_identity(query.resident_key));
		identity = _mix_transport_identity(identity, uint64_t(query.state));
		identity = _mix_transport_identity(identity, query.slot);
		// Retirement completion is observed separately. It is not visible ray
		// scene content and therefore cannot invalidate screen history.
		identity = _mix_transport_identity(identity, query.fail_open_required ? 1u : 0u);
		visibility_residency_entries.push_back(identity);
	};
	for (const Instance &instance : p_request.instances) {
		const uint32_t *surface_index = surface_indices.getptr(instance.surface_id);
		const Surface *const *surface_ptr = surface_records.getptr(instance.surface_id);
		if (!surface_index || !surface_ptr) {
			continue;
		}
		const Surface &surface = **surface_ptr;
		RendererPathTracing::HybridResidencyResourceKey key;
		key.kind = HybridResidencyResourceKind::MATERIAL_DESCRIPTOR;
		key.stable_id = MAX(uint64_t(1), instance.material_stable_id);
		key.generation = MAX(uint64_t(1), instance.material_generation);
		append_residency_query(key);
		key.kind = HybridResidencyResourceKind::GEOMETRY_CLUSTER_PAGE;
		key.stable_id = MAX(uint64_t(1), surface.stable_id);
		key.generation = MAX(uint64_t(1), surface.topology_revision);
		append_residency_query(key);
		key.kind = HybridResidencyResourceKind::BLAS;
		append_residency_query(key);
		key.kind = HybridResidencyResourceKind::TLAS_INSTANCE;
		key.stable_id = MAX(uint64_t(1), instance.stable_id);
		key.generation = 1;
		append_residency_query(key);
		uint64_t ray_visible_state = _mix_transport_identity(instance.stable_id, surface.stable_id);
		ray_visible_state = _mix_transport_identity(ray_visible_state, instance.visibility_mask & 0xffu);
		ray_visible_state = _mix_transport_identity(ray_visible_state, uint64_t(instance.alpha_mode));
		ray_visible_state = _mix_transport_identity(ray_visible_state, instance.face_flags);
		visibility_residency_entries.push_back(ray_visible_state);
	}
	for (const RendererPathTracing::HybridResidencyAllocation &allocation : residency_commit.allocations) {
		uint64_t identity = _transport_residency_key_identity(allocation.key);
		identity = _mix_transport_identity(identity, uint64_t(allocation.pool));
		identity = _mix_transport_identity(identity, allocation.bytes);
		identity = _mix_transport_identity(identity, allocation.slot);
		visibility_residency_entries.push_back(identity);
	}
	for (const RendererPathTracing::HybridResidencyRetirement &retirement : residency_commit.retirements) {
		uint64_t identity = _transport_residency_key_identity(retirement.key);
		identity = _mix_transport_identity(identity, uint64_t(retirement.pool));
		identity = _mix_transport_identity(identity, retirement.bytes);
		identity = _mix_transport_identity(identity, retirement.slot);
		visibility_residency_entries.push_back(identity);
	}
	for (const RendererPathTracing::HybridResidencyAdmission &admission : residency_commit.admissions) {
		uint64_t identity = _mix_transport_identity(admission.request_id, uint64_t(admission.status));
		visibility_residency_entries.push_back(_mix_transport_identity(identity, admission.eye_mask));
	}
	r_result.visibility_residency_generation = _transport_set_identity(visibility_residency_entries, 0xa6e4d25bc1493f07ULL);
	r_result.material_texture_capacity = cache->material_texture_capacity;
	r_result.material_texture_tier2 = cache->bindless_material_textures;
	} // full residency request/admission/query plan
	uint32_t requested_alpha_mask_instances = 0;
	for (const Instance &instance : p_request.instances) {
		requested_alpha_mask_instances += instance.alpha_mode == Instance::ALPHA_MASK ? 1u : 0u;
	}
	if (requested_alpha_mask_instances > 0u && !cache->alpha_intersection_configured) {
		cache->alpha_intersection_configured = true;
		if (cache->alpha_intersection_function && cache->trace_compute_function && cache->shadow_compute_function) {
			auto create_alpha_pipeline = [&](MTL::Function *p_compute) -> NS::SharedPtr<MTL::ComputePipelineState> {
				NS::SharedPtr<MTL::ComputePipelineDescriptor> descriptor = NS::TransferPtr(MTL::ComputePipelineDescriptor::alloc()->init());
				descriptor->setComputeFunction(p_compute);
				NS::SharedPtr<MTL::LinkedFunctions> linked = NS::TransferPtr(MTL::LinkedFunctions::alloc()->init());
				NS::Object *function_object = cache->alpha_intersection_function.get();
				NS::SharedPtr<NS::Array> functions = NS::TransferPtr(NS::Array::array(&function_object, 1)->retain());
				linked->setFunctions(functions.get());
				descriptor->setLinkedFunctions(linked.get());
				NS::Error *pipeline_error = nullptr;
				return NS::TransferPtr(device->newComputePipelineState(descriptor.get(), MTL::PipelineOptionNone, nullptr, &pipeline_error));
			};
			cache->alpha_trace_pipeline = create_alpha_pipeline(cache->trace_compute_function.get());
			cache->alpha_shadow_pipeline = create_alpha_pipeline(cache->shadow_compute_function.get());
			cache->alpha_material_texture_argument_encoder = NS::TransferPtr(cache->alpha_intersection_function->newArgumentEncoder(3));
			cache->alpha_intersection_supported = cache->alpha_trace_pipeline && cache->alpha_shadow_pipeline && cache->alpha_material_texture_argument_encoder;
		}
	}
	work->alpha_intersection_enabled = requested_alpha_mask_instances > 0u && cache->alpha_intersection_supported;
	work->alpha_trace_pipeline = cache->alpha_trace_pipeline;
	work->alpha_shadow_pipeline = cache->alpha_shadow_pipeline;
	if (requested_alpha_mask_instances > 0u && !work->alpha_intersection_enabled) {
		// The supported failure mode is visible flat-card opacity: preserve the
		// geometry as opaque instead of silently omitting it from ray traversal.
		r_result.alpha_traversal_fallbacks = requested_alpha_mask_instances;
		WARN_PRINT_ONCE("Metal hybrid strict alpha traversal is unavailable; alpha-mask instances use opaque fail-open traversal.");
	}
	const uint32_t material_texture_count = cache->bindless_material_textures ? cache->material_texture_capacity : HYBRID_FALLBACK_MATERIAL_TEXTURES;
	if (stable_residency_plan_hit && cache->stable_residency_plan.material_textures.size() == material_texture_count) {
		work->material_textures = cache->stable_residency_plan.material_textures;
	} else {
		work->material_textures.resize(material_texture_count);
		for (MTL::Texture *&texture : work->material_textures) texture = cache->bindless_material_textures ? nullptr : default_albedo;
	}
	HashMap<uint64_t, uint32_t> resident_texture_slots;
	for (MetalFluxResidencyTexturePlanEntry &entry : texture_plan) {
		RendererPathTracing::HybridResidencyResourceKey key;
		key.kind = HybridResidencyResourceKind::TEXTURE_MIP;
		key.texture_channel = entry.channel;
		key.stable_id = MAX(uint64_t(1), entry.rid.get_id());
		key.generation = entry.generation;
		bool texture_resident = entry.resident;
		uint32_t texture_slot = entry.slot;
		if (!stable_residency_plan_hit) {
			const RendererPathTracing::HybridResidencyQuery query = cache->residency_planner.query(key);
			texture_resident = query.state == HybridResidencyState::RESIDENT && !query.fail_open_required;
			texture_slot = query.slot;
			entry.resident = texture_resident;
			entry.slot = texture_slot;
			visibility_residency_complete &= texture_resident;
			uint64_t residency_identity = _mix_transport_identity(uint64_t(entry.channel), entry.generation);
			residency_identity = _mix_transport_identity(residency_identity, query.resident_key.generation);
			residency_identity = _mix_transport_identity(residency_identity, uint64_t(query.state));
			residency_identity = _mix_transport_identity(residency_identity, query.slot);
			visibility_residency_entries.push_back(residency_identity);
		}
		const uint32_t channel = static_cast<uint32_t>(entry.channel);
		const uint64_t plan_key = (uint64_t(entry.rid.get_id()) << 8u) | uint64_t(entry.channel);
		if (texture_resident && texture_slot < uint32_t(work->material_textures.size()) && entry.texture) {
			work->material_textures.write[texture_slot] = entry.texture;
			resident_texture_slots.insert(plan_key, texture_slot);
			if (!stable_residency_plan_hit) r_result.material_texture_resident[channel]++;
			sampled_texture_resources.push_back(entry.rid);
		} else {
			if (!stable_residency_plan_hit) {
				r_result.material_texture_misses[channel]++;
				r_result.texture_fallbacks++;
				if (cache->residency_planner.query(key).state == HybridResidencyState::STALE) cache->material_generation_rejects++;
			}
		}
	}
	// Texture residency is resolved after the commit and material binding table;
	// publish the completed revision only after those states are represented.
	if (!stable_residency_plan_hit) r_result.visibility_residency_generation = _transport_set_identity(visibility_residency_entries, 0xa6e4d25bc1493f07ULL);
	r_result.residency_complete = visibility_residency_complete;
	r_result.material_texture_capacity = cache->material_texture_capacity;
	r_result.material_texture_tier2 = cache->bindless_material_textures;
	r_result.residency_complete = visibility_residency_complete;
	if (!stable_residency_plan_hit) {
		MetalFluxStableResidencyPlan &plan = cache->stable_residency_plan;
		plan.valid = visibility_residency_complete && residency_commit.retirements.is_empty();
		plan.complete = visibility_residency_complete;
		plan.identity = residency_plan_identity;
		plan.visibility_generation = r_result.visibility_residency_generation;
		plan.visible_keys = stable_residency_keys;
		plan.textures = texture_plan;
		plan.material_textures = work->material_textures;
		memcpy(plan.requested, r_result.material_texture_requested, sizeof(plan.requested));
		memcpy(plan.resident, r_result.material_texture_resident, sizeof(plan.resident));
		memcpy(plan.misses, r_result.material_texture_misses, sizeof(plan.misses));
		plan.texture_fallbacks = r_result.texture_fallbacks;
	}
	const uint64_t cpu_residency_end = mach_absolute_time();
	r_result.cpu_scene_preparation_milliseconds = cpu_milliseconds_since(cpu_render_begin, cpu_residency_begin);
	r_result.cpu_residency_planning_milliseconds = cpu_milliseconds_since(cpu_residency_begin, cpu_residency_end);
	r_result.cpu_residency_plan_cached_milliseconds = stable_residency_plan_hit ? r_result.cpu_residency_planning_milliseconds : 0.0;
	r_result.cpu_residency_plan_full_milliseconds = stable_residency_plan_hit ? 0.0 : r_result.cpu_residency_planning_milliseconds;
	r_result.residency_plan_cache_hit_count = cache->stable_residency_plan_hits;
	r_result.residency_plan_cache_miss_count = cache->stable_residency_plan_misses;
	r_result.residency_plan_cache_rebuild_count = cache->stable_residency_plan_rebuilds;
	const uint64_t table_update_begin = mach_absolute_time();
	if (cache->bindless_material_textures) {
		work->material_texture_argument_buffer = NS::TransferPtr(device->newBuffer(cache->material_texture_argument_encoder->encodedLength(), MTL::ResourceStorageModeShared));
		if (!work->material_texture_argument_buffer) {
			memdelete(work);
			return _hybrid_fail(ERR_CANT_CREATE, "Metal hybrid material texture argument buffer could not be created.", r_error);
		}
		cache->material_texture_argument_encoder->setArgumentBuffer(work->material_texture_argument_buffer.get(), 0);
		for (uint32_t slot = 0; slot < uint32_t(work->material_textures.size()); slot++) {
			if (work->material_textures[slot]) {
				cache->material_texture_argument_encoder->setTexture(work->material_textures[slot], slot);
			}
		}
	}
	if (work->alpha_intersection_enabled) {
		work->alpha_material_texture_argument_buffer = NS::TransferPtr(device->newBuffer(cache->alpha_material_texture_argument_encoder->encodedLength(), MTL::ResourceStorageModeShared));
		if (!work->alpha_material_texture_argument_buffer) {
			// Alpha traversal is optional. Keep masked cards visible as opaque when
			// its private argument-buffer binding cannot be allocated.
			work->alpha_intersection_enabled = false;
			r_result.alpha_traversal_fallbacks += requested_alpha_mask_instances;
			WARN_PRINT_ONCE("Metal hybrid alpha intersection texture argument buffer is unavailable; alpha-mask instances use opaque fail-open traversal.");
		} else {
			cache->alpha_material_texture_argument_encoder->setArgumentBuffer(work->alpha_material_texture_argument_buffer.get(), 0);
			for (uint32_t slot = 0; slot < uint32_t(work->material_textures.size()); slot++) {
				if (work->material_textures[slot]) {
					cache->alpha_material_texture_argument_encoder->setTexture(work->material_textures[slot], slot);
				}
			}
		}
	}
	mach_timebase_info_data_t timebase;
	mach_timebase_info(&timebase);
	r_result.material_table_update_milliseconds = double(mach_absolute_time() - table_update_begin) * double(timebase.numer) / double(timebase.denom) / 1000000.0;
	r_result.material_generation_rejects = uint32_t(MIN(cache->material_generation_rejects, uint64_t(UINT32_MAX)));
	auto resident_texture_slot = [&](RID p_rid, HybridResidencyTextureChannel p_channel) -> uint32_t {
		if (!p_rid.is_valid()) {
			return RendererPathTracing::HYBRID_RESIDENCY_INVALID_SLOT;
		}
		const uint64_t plan_key = (uint64_t(p_rid.get_id()) << 8u) | uint64_t(p_channel);
		const uint32_t *slot = resident_texture_slots.getptr(plan_key);
		return slot && *slot < cache->material_texture_capacity ? *slot : RendererPathTracing::HYBRID_RESIDENCY_INVALID_SLOT;
	};
	for (const Instance &instance : p_request.instances) {
		const uint32_t *surface_index = surface_indices.getptr(instance.surface_id);
		const Surface *const *surface_ptr = surface_records.getptr(instance.surface_id);
		if (!surface_index || !surface_ptr) {
			continue;
		}
		MTL::AccelerationStructureUserIDInstanceDescriptor native = {};
		const Vector3 x = instance.transform.basis.get_column(0);
		const Vector3 y = instance.transform.basis.get_column(1);
		const Vector3 z = instance.transform.basis.get_column(2);
		native.transformationMatrix = MTL::PackedFloat4x3(MTL::PackedFloat3(x.x, x.y, x.z), MTL::PackedFloat3(y.x, y.y, y.z), MTL::PackedFloat3(z.x, z.y, z.z), MTL::PackedFloat3(instance.transform.origin.x, instance.transform.origin.y, instance.transform.origin.z));
		native.options = instance.alpha_mode == Instance::ALPHA_MASK && work->alpha_intersection_enabled ? MTL::AccelerationStructureInstanceOptionNone : MTL::AccelerationStructureInstanceOptionOpaque;
		native.mask = instance.visibility_mask & 0xff;
		native.accelerationStructureIndex = *surface_index;
		const uint32_t instance_id = metal_instances.size();
		native.userID = instance_id;
		metal_instances.push_back(native);
		// The opaque domain must not contain a masked proxy: accepting one here
		// would turn a rejected alpha texel into an opaque hit. Exact primary,
		// reflection, and visibility traversals retain the mixed TLAS and execute
		// the alpha intersection function.
		MTL::AccelerationStructureUserIDInstanceDescriptor secondary_native = native;
		secondary_native.options = MTL::AccelerationStructureInstanceOptionOpaque;
		if (instance.alpha_mode == Instance::ALPHA_OPAQUE) {
			opaque_metal_instances.push_back(secondary_native);
		} else if (instance.alpha_mode == Instance::ALPHA_MASK && work->alpha_intersection_enabled) {
			// Retain the original non-opaque option: this domain must execute the
			// authored alpha intersection function rather than treating its proxy as
			// solid. userID remains the full material-record index.
			alpha_metal_instances.push_back(native);
		}
		MetalFluxMaterial material = {};
		material.albedo_metallic = simd_make_float4(instance.albedo.r, instance.albedo.g, instance.albedo.b, instance.metallic);
		material.emission_roughness = simd_make_float4(instance.emission.r, instance.emission.g, instance.emission.b, instance.roughness);
		material.uv_scale_offset = simd_make_float4(instance.uv_scale.x, instance.uv_scale.y, instance.uv_offset.x, instance.uv_offset.y);
		material.metallic_texture_channel = simd_make_float4(instance.metallic_texture_channel.r, instance.metallic_texture_channel.g, instance.metallic_texture_channel.b, instance.metallic_texture_channel.a);
		material.roughness_texture_channel = simd_make_float4(instance.roughness_texture_channel.r, instance.roughness_texture_channel.g, instance.roughness_texture_channel.b, instance.roughness_texture_channel.a);
		material.ao_texture_channel = simd_make_float4(instance.ambient_occlusion_texture_channel.r, instance.ambient_occlusion_texture_channel.g, instance.ambient_occlusion_texture_channel.b, instance.ambient_occlusion_texture_channel.a);
		material.material_factors = simd_make_float4(instance.normal_scale, instance.ambient_occlusion_strength, instance.alpha_cutoff, instance.emission_texture_scale);
		material.albedo_alpha = instance.albedo.a;
		material.specular = CLAMP(instance.specular, 0.0f, 1.0f);
		material.thin_transmission_ior = simd_make_float2(CLAMP(instance.thin_transmission, 0.0f, 1.0f), CLAMP(Math::is_finite(instance.thin_ior) ? instance.thin_ior : 1.5f, 1.0f, 4.0f));
		material.face_flags = instance.face_flags;
		material.visibility_mask = instance.visibility_mask;
		material.flags = (instance.alpha_mode == Instance::ALPHA_MASK ? 1u : 0u) | (instance.orm_packed ? 2u : 0u) | (!instance.emission_multiply ? 4u : 0u) | (instance.thin_transmission > 0.0f && instance.thin_transmission_unsupported_features == 0u ? 8u : 0u);
		material.generation_low = uint32_t(instance.material_generation);
		material.generation_high = uint32_t(instance.material_generation >> 32);
		const Surface &surface = **surface_ptr;
		const bool has_uv_source = surface.has_uv && *surface_index < uint32_t(work->attribute_buffers.size()) && work->attribute_buffers[*surface_index];
		if (has_uv_source) {
			material.albedo_texture_index = resident_texture_slot(instance.albedo_texture, HybridResidencyTextureChannel::ALBEDO);
			material.normal_texture_index = resident_texture_slot(instance.normal_texture, HybridResidencyTextureChannel::NORMAL);
			material.orm_texture_index = resident_texture_slot(instance.orm_texture, HybridResidencyTextureChannel::ORM);
			material.metallic_texture_index = resident_texture_slot(instance.metallic_texture, HybridResidencyTextureChannel::ORM);
			material.roughness_texture_index = resident_texture_slot(instance.roughness_texture, HybridResidencyTextureChannel::ORM);
			material.ao_texture_index = resident_texture_slot(instance.ambient_occlusion_texture, HybridResidencyTextureChannel::ORM);
			material.emission_texture_index = resident_texture_slot(instance.emission_texture, HybridResidencyTextureChannel::EMISSIVE);
			material.opacity_texture_index = resident_texture_slot(instance.opacity_texture, HybridResidencyTextureChannel::OPACITY);
			material.alpha_occupancy_texture_index = resident_texture_slot(instance.alpha_occupancy_texture, HybridResidencyTextureChannel::ALPHA_OCCUPANCY);
			if (material.albedo_texture_index != RendererPathTracing::HYBRID_RESIDENCY_INVALID_SLOT) {
				r_result.textured_materials++;
			}
		} else if (instance.albedo_texture.is_valid() || instance.normal_texture.is_valid() || instance.orm_texture.is_valid() || instance.metallic_texture.is_valid() || instance.roughness_texture.is_valid() || instance.ambient_occlusion_texture.is_valid() || instance.emission_texture.is_valid() || instance.opacity_texture.is_valid() || instance.alpha_occupancy_texture.is_valid()) {
			r_result.texture_fallbacks++;
		}
		if (!instance.canonical_material) {
			r_result.unsupported_materials++;
		}
		if (instance.alpha_mode == Instance::ALPHA_MASK) {
			r_result.alpha_mask_instances++;
		}
		metal_materials.push_back(material);
		MetalFluxGeometry geometry = {};
		geometry.vertex_address = work->vertex_buffers[*surface_index]->gpuAddress() + surface.vertex_buffer_offset;
		geometry.index_address = work->index_buffers[*surface_index] ? work->index_buffers[*surface_index]->gpuAddress() + surface.index_buffer_offset : 0;
		geometry.attribute_address = surface.has_uv && work->attribute_buffers[*surface_index] ? work->attribute_buffers[*surface_index]->gpuAddress() + surface.attribute_buffer_offset : 0;
		geometry.vertex_count = surface.vertex_count;
		geometry.index_count = surface.index_count;
		geometry.index_type = surface.index_stride == 2 || (surface.index_stride == 0 && surface.vertex_count <= 65536) ? 16 : 32;
		geometry.position_stride = surface.vertex_stride;
		geometry.normal_offset = surface.normal_offset;
		geometry.normal_stride = surface.normal_stride;
		geometry.has_normals = surface.has_normals ? 1u : 0u;
		geometry.compressed = surface.compressed ? 1u : 0u;
		geometry.attribute_stride = surface.attribute_stride;
		geometry.uv_offset = surface.uv_offset;
		geometry.has_uv = geometry.attribute_address != 0 ? 1u : 0u;
		geometry.instance_identity_low = uint32_t(instance.stable_id);
		geometry.instance_identity_high = uint32_t(instance.stable_id >> 32);
		geometry.has_tangents = surface.has_tangents && surface.has_normals ? 1u : 0u;
		const Basis normal_basis = instance.transform.basis.inverse().transposed();
		geometry.normal_from_object = _metal_matrix(Transform3D(normal_basis, Vector3()));
		geometry.world_from_object = _metal_matrix(instance.transform);
		geometry.position_scale = simd_make_float4(surface.compressed_aabb.size.x, surface.compressed_aabb.size.y, surface.compressed_aabb.size.z, 0.0f);
		geometry.position_offset = simd_make_float4(surface.compressed_aabb.position.x, surface.compressed_aabb.position.y, surface.compressed_aabb.position.z, 0.0f);
		metal_geometries.push_back(geometry);
		// Texture-driven metallic/roughness can vary across a triangle, so this
		// exact-delta prototype rejects it at extraction rather than accepting a
		// possibly glossy mirror. The shader repeats all current material checks.
		const bool mirror_factor_only = !instance.orm_packed && !instance.metallic_texture.is_valid() && !instance.roughness_texture.is_valid();
		const bool mirror_supported = p_request.bidirectional_caustics && instance.canonical_material && instance.alpha_mode == Instance::ALPHA_OPAQUE && !surface.dynamic && mirror_factor_only && instance.metallic >= 0.999f && instance.roughness <= p_request.bidirectional_caustic_delta_roughness_threshold && instance.stable_id != 0 && instance.material_stable_id != 0 && instance.material_generation != 0 && (instance.visibility_mask & 0xffu) != 0u;
		if (mirror_supported) {
			const uint32_t triangle_count = (surface.index_count ? surface.index_count : surface.vertex_count) / 3u;
			for (uint32_t primitive_id = 0; primitive_id < triangle_count; primitive_id++) {
				MetalFluxBidirectionalCausticMirrorTriangle mirror = {};
				mirror.instance_id = instance_id;
				mirror.primitive_id = primitive_id;
				mirror.instance_identity_low = uint32_t(instance.stable_id);
				mirror.instance_identity_high = uint32_t(instance.stable_id >> 32);
				mirror.material_generation_low = uint32_t(instance.material_generation);
				mirror.material_generation_high = uint32_t(instance.material_generation >> 32);
				mirror.visibility_mask = instance.visibility_mask & 0xffu;
				metal_bidirectional_caustic_mirrors.push_back(mirror);
			}
		}
		if (instance.emission.r > 0.0001f || instance.emission.g > 0.0001f || instance.emission.b > 0.0001f) {
			MetalFluxEmissive emitter;
			emitter.instance_id = instance_id;
			emitter.source_identity = uint32_t(instance.stable_id) ^ uint32_t(instance.stable_id >> 32);
			emitter.triangle_count = (surface.index_count ? surface.index_count : surface.vertex_count) / 3;
			if (emitter.triangle_count > 0) {
				const Vector3 extent_x = instance.transform.basis.xform(Vector3(surface.compressed_aabb.size.x, 0.0f, 0.0f));
				const Vector3 extent_y = instance.transform.basis.xform(Vector3(0.0f, surface.compressed_aabb.size.y, 0.0f));
				const Vector3 extent_z = instance.transform.basis.xform(Vector3(0.0f, 0.0f, surface.compressed_aabb.size.z));
				const float extent_area = 2.0f * (extent_x.cross(extent_y).length() + extent_y.cross(extent_z).length() + extent_z.cross(extent_x).length());
				const float luminance = instance.emission.r * 0.2126f + instance.emission.g * 0.7152f + instance.emission.b * 0.0722f;
				emissive_weights.push_back(RendererPathTracing::light_sampling_triangle_power_weight(luminance, extent_area));
				metal_emissives.push_back(emitter);
			}
		}
	}
	// Derive this after BLAS selection/deferment and exact TLAS/material packing.
	// The sequence is sorted by stable content, so request traversal order and
	// camera movement cannot perturb it.
	Vector<uint64_t> admitted_geometry_entries;
	for (const Instance &instance : p_request.instances) {
		const uint32_t *surface_index = surface_indices.getptr(instance.surface_id);
		const Surface *const *surface_ptr = surface_records.getptr(instance.surface_id);
		if (!surface_index || !surface_ptr) {
			continue;
		}
		const Surface &surface = **surface_ptr;
		uint64_t identity = _mix_transport_identity(instance.stable_id, surface.stable_id);
		identity = _mix_transport_identity(identity, surface.topology_revision);
		identity = _mix_transport_identity(identity, surface.deformation_revision);
		identity = _mix_transport_identity(identity, uint64_t(surface.vertex_count) << 32 | surface.index_count);
		identity = _mix_transport_identity(identity, uint64_t(surface.vertex_stride) << 32 | surface.index_stride);
		identity = _mix_transport_identity(identity, instance.material_stable_id);
		identity = _mix_transport_identity(identity, instance.material_generation);
		identity = _mix_transport_identity(identity, uint64_t(instance.alpha_mode) << 32 | instance.face_flags);
		identity = _mix_transport_identity(identity, instance.visibility_mask & 0xffu);
		for (uint32_t column = 0; column < 3; column++) {
			const Vector3 axis = instance.transform.basis.get_column(column);
			identity = _mix_transport_identity(identity, _transport_float_bits(axis.x));
			identity = _mix_transport_identity(identity, _transport_float_bits(axis.y));
			identity = _mix_transport_identity(identity, _transport_float_bits(axis.z));
		}
		identity = _mix_transport_identity(identity, _transport_float_bits(instance.transform.origin.x));
		identity = _mix_transport_identity(identity, _transport_float_bits(instance.transform.origin.y));
		identity = _mix_transport_identity(identity, _transport_float_bits(instance.transform.origin.z));
		admitted_geometry_entries.push_back(identity);
	}
	r_result.admitted_geometry_generation = _transport_set_identity(admitted_geometry_entries, 0x43dca1f6057ebc29ULL);

	if (!metal_emissives.is_empty()) {
		double total_weight = 0.0;
		for (float weight : emissive_weights) {
			total_weight += weight;
		}
		if (!(total_weight > 0.0) || !Math::is_finite(total_weight)) {
			total_weight = metal_emissives.size();
			for (int i = 0; i < emissive_weights.size(); i++) {
				emissive_weights.write[i] = 1.0f;
			}
		}
		double cumulative = 0.0;
		for (int i = 0; i < metal_emissives.size(); i++) {
			MetalFluxEmissive &emitter = metal_emissives.write[i];
			emitter.selection_pdf = (float)(emissive_weights[i] / total_weight);
			cumulative += emitter.selection_pdf;
			emitter.cdf = i + 1 == metal_emissives.size() ? 1.0f : (float)cumulative;
		}
	}
	if (work->blas.is_empty() || metal_instances.is_empty()) {
		memdelete(work);
		return _hybrid_fail(ERR_INVALID_DATA, "No supported triangle surfaces were available to the Metal hybrid renderer.", r_error);
	}
	// Preserve a stable proposal table independent of traversal/BLAS ordering.
	for (int i = 1; i < metal_bidirectional_caustic_mirrors.size(); i++) {
		MetalFluxBidirectionalCausticMirrorTriangle value = metal_bidirectional_caustic_mirrors[i];
		int j = i - 1;
		while (j >= 0) {
			const MetalFluxBidirectionalCausticMirrorTriangle &previous = metal_bidirectional_caustic_mirrors[j];
			const bool ordered = previous.instance_identity_high < value.instance_identity_high || (previous.instance_identity_high == value.instance_identity_high && (previous.instance_identity_low < value.instance_identity_low || (previous.instance_identity_low == value.instance_identity_low && previous.primitive_id <= value.primitive_id)));
			if (ordered) break;
			metal_bidirectional_caustic_mirrors.write[j + 1] = previous;
			j--;
		}
		metal_bidirectional_caustic_mirrors.write[j + 1] = value;
	}
	const uint32_t caustic_mirror_capacity = CLAMP(p_request.bidirectional_caustic_max_mirror_triangles, 1u, 1048576u);
	const uint32_t caustic_mirror_total = metal_bidirectional_caustic_mirrors.size();
	if (metal_bidirectional_caustic_mirrors.size() > int(caustic_mirror_capacity)) {
		metal_bidirectional_caustic_mirrors.resize(caustic_mirror_capacity);
	}
	r_result.bidirectional_caustic_enabled = p_request.bidirectional_caustics;
	r_result.bidirectional_caustic_mirror_triangle_count = metal_bidirectional_caustic_mirrors.size();
	r_result.bidirectional_caustic_mirror_triangle_capacity = caustic_mirror_capacity;
	r_result.bidirectional_caustic_mirror_triangle_overflow = caustic_mirror_total > caustic_mirror_capacity ? caustic_mirror_total - caustic_mirror_capacity : 0u;
	// These records are needed by the intersection function table. Avoid a new
	// shared upload allocation when the admitted material/geometry packet is
	// byte-identical to the previous stable scene packet.
	const uint64_t material_packet_bytes = uint64_t(metal_materials.size()) * sizeof(MetalFluxMaterial);
	const uint64_t geometry_packet_bytes = uint64_t(metal_geometries.size()) * sizeof(MetalFluxGeometry);
	const bool material_packet_matches = cache->cached_material_records && cache->cached_material_record_bytes.size() == material_packet_bytes && (metal_materials.is_empty() || memcmp(cache->cached_material_record_bytes.ptr(), metal_materials.ptr(), material_packet_bytes) == 0);
	const bool geometry_packet_matches = cache->cached_geometry_records && cache->cached_geometry_record_bytes.size() == geometry_packet_bytes && (metal_geometries.is_empty() || memcmp(cache->cached_geometry_record_bytes.ptr(), metal_geometries.ptr(), geometry_packet_bytes) == 0);
	if (material_packet_matches) {
		work->materials = cache->cached_material_records;
	} else {
		cache->cached_material_records = NS::TransferPtr(device->newBuffer(metal_materials.ptr(), metal_materials.size() * sizeof(MetalFluxMaterial), MTL::ResourceStorageModeShared));
		cache->cached_material_record_bytes.resize(material_packet_bytes);
		if (!metal_materials.is_empty()) memcpy(cache->cached_material_record_bytes.ptrw(), metal_materials.ptr(), material_packet_bytes);
		work->materials = cache->cached_material_records;
	}
	if (geometry_packet_matches) {
		work->geometries = cache->cached_geometry_records;
	} else {
		cache->cached_geometry_records = NS::TransferPtr(device->newBuffer(metal_geometries.ptr(), metal_geometries.size() * sizeof(MetalFluxGeometry), MTL::ResourceStorageModeShared));
		cache->cached_geometry_record_bytes.resize(geometry_packet_bytes);
		if (!metal_geometries.is_empty()) memcpy(cache->cached_geometry_record_bytes.ptrw(), metal_geometries.ptr(), geometry_packet_bytes);
		work->geometries = cache->cached_geometry_records;
	}
	if (work->alpha_intersection_enabled) {
		NS::SharedPtr<MTL::IntersectionFunctionTableDescriptor> descriptor = NS::TransferPtr(MTL::IntersectionFunctionTableDescriptor::alloc()->init());
		if (descriptor) {
			descriptor->setFunctionCount(1);
		}
		auto make_intersection_table = [&](MTL::ComputePipelineState *p_pipeline) -> NS::SharedPtr<MTL::IntersectionFunctionTable> {
			if (!descriptor || !p_pipeline || !work->alpha_intersection_function || !work->materials || !work->geometries || !work->material_diagnostic || !work->material_diagnostic->values || !work->alpha_material_texture_argument_buffer) {
				return nullptr;
			}
			NS::SharedPtr<MTL::IntersectionFunctionTable> table = NS::TransferPtr(p_pipeline->newIntersectionFunctionTable(descriptor.get()));
			if (!table) {
				return nullptr;
			}
			MTL::FunctionHandle *function_handle = p_pipeline->functionHandle(work->alpha_intersection_function.get());
			if (!function_handle) {
				return nullptr;
			}
			table->setFunction(function_handle, 0);
			table->setBuffer(work->materials.get(), 0, 0);
			table->setBuffer(work->geometries.get(), 0, 1);
			table->setBuffer(work->material_diagnostic->values.get(), 0, 2);
			table->setBuffer(work->alpha_material_texture_argument_buffer.get(), 0, 3);
			return table;
		};
		work->alpha_trace_intersection_table = make_intersection_table(work->alpha_trace_pipeline.get());
		work->alpha_shadow_intersection_table = make_intersection_table(work->alpha_shadow_pipeline.get());
		if (!work->alpha_trace_intersection_table || !work->alpha_shadow_intersection_table) {
			// The TLAS has not been built yet, so changing the native descriptors
			// here guarantees a complete opaque fail-open traversal for this work.
			work->alpha_trace_intersection_table = nullptr;
			work->alpha_shadow_intersection_table = nullptr;
			work->alpha_intersection_enabled = false;
			r_result.alpha_traversal_fallbacks += r_result.alpha_mask_instances;
			for (uint32_t instance_index = 0; instance_index < uint32_t(metal_instances.size()); instance_index++) {
				if ((metal_materials[instance_index].flags & 1u) != 0u) {
					metal_instances.write[instance_index].options = MTL::AccelerationStructureInstanceOptionOpaque;
				}
			}
			WARN_PRINT_ONCE("Metal hybrid alpha intersection function table binding is unavailable; alpha-mask instances use opaque fail-open traversal.");
		}
	}
	const bool tlas_topology_dirty = !cache->tlas || cache->tlas_blas_order.size() != blas_ptrs.size() || cache->tlas_instances.size() != metal_instances.size() || (!blas_ptrs.is_empty() && memcmp(cache->tlas_blas_order.ptr(), blas_ptrs.ptr(), blas_ptrs.size() * sizeof(MTL::AccelerationStructure *)) != 0);
	const bool tlas_transform_dirty = !tlas_topology_dirty && !metal_instances.is_empty() && memcmp(cache->tlas_instances.ptr(), metal_instances.ptr(), metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor)) != 0;
	if (tlas_topology_dirty || tlas_transform_dirty) {
		work->tlas_instances = NS::TransferPtr(device->newBuffer(metal_instances.ptr(), metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor), MTL::ResourceStorageModeShared));
		work->tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		work->tlas_descriptor->setInstancedAccelerationStructures(work->blas_array.get());
		work->tlas_descriptor->setInstanceDescriptorBuffer(work->tlas_instances.get());
		work->tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
		work->tlas_descriptor->setInstanceCount(metal_instances.size());
		work->tlas_descriptor->setUsage(MTL::AccelerationStructureUsageRefit | MTL::AccelerationStructureUsagePreferFastIntersection);
		MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(work->tlas_descriptor.get());
		if (tlas_topology_dirty) {
			cache->tlas = NS::TransferPtr(device->newAccelerationStructure(tlas_sizes.accelerationStructureSize));
			work->tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), tlas_sizes.buildScratchBufferSize), MTL::ResourceStorageModePrivate));
			work->tlas_build = true;
			r_result.tlas_rebuilds = 1;
		} else {
			work->tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), tlas_sizes.refitScratchBufferSize), MTL::ResourceStorageModePrivate));
			work->tlas_refit = true;
			r_result.tlas_refits = 1;
		}
		cache->tlas_blas_order = blas_ptrs;
		cache->tlas_instances = metal_instances;
		r_result.scene.tlas_updated = 1;
	} else {
		r_result.tlas_reuses = 1;
	}
	work->tlas = cache->tlas;
	// Exact primary/reflection/visibility rays retain the mixed TLAS above.
	// This domain intentionally excludes masked geometry. The mixed TLAS remains
	// authoritative until a paired alpha-only domain is available; binding an
	// opaque-only TLAS as the sole answer would otherwise miss valid cutouts.
	if (work->alpha_intersection_enabled && !opaque_metal_instances.is_empty()) {
		const bool opaque_topology_dirty = !cache->opaque_tlas || cache->opaque_tlas_blas_order.size() != blas_ptrs.size() || cache->opaque_tlas_instances.size() != opaque_metal_instances.size() || (!blas_ptrs.is_empty() && memcmp(cache->opaque_tlas_blas_order.ptr(), blas_ptrs.ptr(), blas_ptrs.size() * sizeof(MTL::AccelerationStructure *)) != 0);
		const bool opaque_transform_dirty = !opaque_topology_dirty && memcmp(cache->opaque_tlas_instances.ptr(), opaque_metal_instances.ptr(), opaque_metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor)) != 0;
		if (opaque_topology_dirty || opaque_transform_dirty) {
			work->opaque_tlas_instances = NS::TransferPtr(device->newBuffer(opaque_metal_instances.ptr(), opaque_metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor), MTL::ResourceStorageModeShared));
			work->opaque_tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
			work->opaque_tlas_descriptor->setInstancedAccelerationStructures(work->blas_array.get());
			work->opaque_tlas_descriptor->setInstanceDescriptorBuffer(work->opaque_tlas_instances.get());
			work->opaque_tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
			work->opaque_tlas_descriptor->setInstanceCount(opaque_metal_instances.size());
			work->opaque_tlas_descriptor->setUsage(MTL::AccelerationStructureUsageRefit | MTL::AccelerationStructureUsagePreferFastIntersection);
			const MTL::AccelerationStructureSizes opaque_sizes = device->accelerationStructureSizes(work->opaque_tlas_descriptor.get());
			if (opaque_topology_dirty) {
				cache->opaque_tlas = NS::TransferPtr(device->newAccelerationStructure(opaque_sizes.accelerationStructureSize));
				work->opaque_tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), opaque_sizes.buildScratchBufferSize), MTL::ResourceStorageModePrivate));
				work->opaque_tlas_build = true;
			} else {
				work->opaque_tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), opaque_sizes.refitScratchBufferSize), MTL::ResourceStorageModePrivate));
				work->opaque_tlas_refit = true;
			}
			cache->opaque_tlas_blas_order = blas_ptrs;
			cache->opaque_tlas_instances = opaque_metal_instances;
		}
		work->opaque_tlas = cache->opaque_tlas;
	} else {
		work->opaque_tlas = work->tlas;
	}
	// The alpha domain is deliberately independent from the opaque domain. It
	// contains only alpha-mask instances and keeps the exact intersection
	// function enabled, so a rejected texel can never occlude a ray.
	if (work->alpha_intersection_enabled && !alpha_metal_instances.is_empty()) {
		const bool alpha_topology_dirty = !cache->alpha_tlas || cache->alpha_tlas_blas_order.size() != blas_ptrs.size() || cache->alpha_tlas_instances.size() != alpha_metal_instances.size() || (!blas_ptrs.is_empty() && memcmp(cache->alpha_tlas_blas_order.ptr(), blas_ptrs.ptr(), blas_ptrs.size() * sizeof(MTL::AccelerationStructure *)) != 0);
		const bool alpha_transform_dirty = !alpha_topology_dirty && memcmp(cache->alpha_tlas_instances.ptr(), alpha_metal_instances.ptr(), alpha_metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor)) != 0;
		if (alpha_topology_dirty || alpha_transform_dirty) {
			work->alpha_tlas_instances = NS::TransferPtr(device->newBuffer(alpha_metal_instances.ptr(), alpha_metal_instances.size() * sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor), MTL::ResourceStorageModeShared));
			work->alpha_tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
			work->alpha_tlas_descriptor->setInstancedAccelerationStructures(work->blas_array.get());
			work->alpha_tlas_descriptor->setInstanceDescriptorBuffer(work->alpha_tlas_instances.get());
			work->alpha_tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
			work->alpha_tlas_descriptor->setInstanceCount(alpha_metal_instances.size());
			work->alpha_tlas_descriptor->setUsage(MTL::AccelerationStructureUsageRefit | MTL::AccelerationStructureUsagePreferFastIntersection);
			const MTL::AccelerationStructureSizes alpha_sizes = device->accelerationStructureSizes(work->alpha_tlas_descriptor.get());
			if (alpha_topology_dirty) {
				cache->alpha_tlas = NS::TransferPtr(device->newAccelerationStructure(alpha_sizes.accelerationStructureSize));
				work->alpha_tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), alpha_sizes.buildScratchBufferSize), MTL::ResourceStorageModePrivate));
				work->alpha_tlas_build = true;
			} else {
				work->alpha_tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), alpha_sizes.refitScratchBufferSize), MTL::ResourceStorageModePrivate));
				work->alpha_tlas_refit = true;
			}
			cache->alpha_tlas_blas_order = blas_ptrs;
			cache->alpha_tlas_instances = alpha_metal_instances;
		}
		work->alpha_tlas = cache->alpha_tlas;
	}
	// Never bind a partial split. A scene with only opaque or only alpha content
	// is already fast through its single authoritative domain; mixed scenes need
	// both exact structures before they may replace the mixed TLAS.
	work->split_alpha_domains_exact = work->alpha_intersection_enabled && work->opaque_tlas && work->alpha_tlas && !opaque_metal_instances.is_empty() && !alpha_metal_instances.is_empty();
	const uint32_t emissive_count = metal_emissives.size();
	if (metal_emissives.is_empty()) {
		metal_emissives.push_back(MetalFluxEmissive());
	}
	Vector<MetalFluxPunctualLight> metal_punctual_lights;
	metal_punctual_lights.reserve(p_request.punctual_lights.size());
	for (const PunctualLight &light : p_request.punctual_lights) {
		MetalFluxPunctualLight punctual = {};
		punctual.position_range = simd_make_float4(light.position.x, light.position.y, light.position.z, light.range);
		punctual.radiance_attenuation = simd_make_float4(light.radiance.r, light.radiance.g, light.radiance.b, light.attenuation);
		punctual.direction_spot_outer = simd_make_float4(light.direction.x, light.direction.y, light.direction.z, light.spot_cos_outer);
		punctual.area_u_spot_attenuation = simd_make_float4(light.area_u.x, light.area_u.y, light.area_u.z, light.spot_attenuation);
		punctual.area_v = simd_make_float4(light.area_v.x, light.area_v.y, light.area_v.z, 0.0f);
		punctual.type = light.type;
		punctual.cull_mask = light.cull_mask;
		punctual.shadow_caster_mask = light.shadow_caster_mask;
		punctual.stable_identity = simd_make_uint2(uint32_t(light.stable_id), uint32_t(light.stable_id >> 32));
		punctual.flags = (light.shadow_enabled ? 1u : 0u) | (light.negative ? 2u : 0u);
		punctual.shadow_opacity = CLAMP(light.shadow_opacity, 0.0f, 1.0f);
		punctual.specular_amount = MAX(light.specular_amount, 0.0f);
		punctual.indirect_energy = MAX(light.indirect_energy, 0.0f);
		uint64_t identity = _mix_transport_identity(light.stable_id, uint64_t(light.type));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.position.x));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.position.y));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.position.z));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.direction.x));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.direction.y));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.direction.z));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.radiance.r));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.radiance.g));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.radiance.b));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.range));
		identity = _mix_transport_identity(identity, _transport_float_bits(light.indirect_energy));
		punctual.source_identity = uint32_t(identity) ^ uint32_t(identity >> 32);
		metal_punctual_lights.push_back(punctual);
	}
	const uint32_t punctual_light_count = metal_punctual_lights.size();
	if (metal_punctual_lights.is_empty()) {
		metal_punctual_lights.push_back(MetalFluxPunctualLight());
	}
	Vector<MetalFluxPortal> metal_portals;
	metal_portals.reserve(p_request.environment.portals.size());
	for (const RendererPathTracing::EnvironmentPortal &portal : p_request.environment.portals) {
		if (portal.abi_version != RendererPathTracing::INDOOR_LIGHTING_ABI_VERSION || portal.portal_id == 0 || portal.weight <= 0.0f || !portal.center.is_finite() || !portal.axis_u.is_finite() || !portal.axis_v.is_finite()) {
			continue;
		}
		MetalFluxPortal record = {};
		record.center_weight = simd_make_float4(portal.center.x, portal.center.y, portal.center.z, portal.weight);
		record.axis_u = simd_make_float4(portal.axis_u.x, portal.axis_u.y, portal.axis_u.z, 0.0f);
		record.axis_v = simd_make_float4(portal.axis_v.x, portal.axis_v.y, portal.axis_v.z, 0.0f);
		metal_portals.push_back(record);
	}
	const uint32_t portal_count = metal_portals.size();
	if (metal_portals.is_empty()) {
		metal_portals.push_back(MetalFluxPortal());
	}
	uint64_t light_distribution_identity = 0x7a5f4c3b219d8e60ULL;
	for (const MetalFluxEmissive &emitter : metal_emissives) {
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, uint64_t(emitter.instance_id) << 32 | emitter.triangle_count);
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(emitter.selection_pdf));
	}
	for (const PunctualLight &light : p_request.punctual_lights) {
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, light.stable_id);
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, uint64_t(light.type));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.position.x));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.position.y));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.position.z));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.direction.x));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.direction.y));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.direction.z));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.area_u.x));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.area_u.y));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.area_u.z));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.area_v.x));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.area_v.y));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.area_v.z));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.radiance.r));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.radiance.g));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.radiance.b));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.range));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.attenuation));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.spot_cos_outer));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.spot_attenuation));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, uint64_t(light.cull_mask) << 32 | light.shadow_caster_mask);
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.shadow_opacity));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.specular_amount));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(light.indirect_energy));
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, light.shadow_enabled ? 1u : 0u);
		light_distribution_identity = _mix_transport_identity(light_distribution_identity, light.negative ? 1u : 0u);
	}
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, p_request.directional_light_active ? 1u : 0u);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_direction.x));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_direction.y));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_direction.z));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_radiance.r));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_radiance.g));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_radiance.b));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, uint64_t(p_request.directional_light_cull_mask) << 32 | p_request.directional_shadow_caster_mask);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, p_request.directional_shadow_enabled ? 1u : 0u);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, p_request.directional_negative ? 1u : 0u);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_shadow_opacity));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_specular_amount));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(p_request.directional_light_angular_radius));
	const FrameRequest::SolarLobe &solar_identity = p_request.environment.solar_lobe;
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.active ? 1u : 0u);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.source_id);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.sample_id);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.profile_version);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.partition_version);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.state_generation);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, solar_identity.history_epoch);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(solar_identity.current_direction.x));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(solar_identity.current_direction.y));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(solar_identity.current_direction.z));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(solar_identity.perpendicular_irradiance.r));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(solar_identity.perpendicular_irradiance.g));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, _transport_float_bits(solar_identity.perpendicular_irradiance.b));
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, p_request.environment.portal_generation);
	light_distribution_identity = _mix_transport_identity(light_distribution_identity, p_request.environment.radiance_content_generation);
	// The prior camera-relative volume had neither stable world-cell ownership
	// nor synchronized surface-domain validation. Do not allocate, sample, or
	// update it until a complete world-space cache contract exists.
	const uint64_t diffuse_cache_revision = 0;
	// The triangle table is intentionally bounded. An overflow is visible in
	// FrameResult rather than silently falling back to the old extent proxy.
	static constexpr uint32_t EMISSIVE_TRIANGLE_CAPACITY = 32768u;
	uint64_t total_emissive_triangle_slots = 0;
	for (uint32_t emissive_index = 0; emissive_index < emissive_count; emissive_index++) {
		total_emissive_triangle_slots += metal_emissives[emissive_index].triangle_count;
	}
	const uint32_t emissive_triangle_count = uint32_t(MIN(total_emissive_triangle_slots, uint64_t(EMISSIVE_TRIANGLE_CAPACITY)));
	const uint32_t emissive_triangle_capacity = MAX(emissive_triangle_count, 1u);
	const uint32_t emissive_triangle_block_count = (emissive_triangle_capacity + 255u) / 256u;
	r_result.emissive_triangle_count = emissive_triangle_count;
	r_result.emissive_triangle_capacity = EMISSIVE_TRIANGLE_CAPACITY;
	r_result.emissive_triangle_overflow = total_emissive_triangle_slots > EMISSIVE_TRIANGLE_CAPACITY ? uint32_t(MIN(total_emissive_triangle_slots - EMISSIVE_TRIANGLE_CAPACITY, uint64_t(UINT32_MAX))) : 0u;
	work->emissives = NS::TransferPtr(device->newBuffer(metal_emissives.ptr(), metal_emissives.size() * sizeof(MetalFluxEmissive), MTL::ResourceStorageModeShared));
	work->emissive_triangles = NS::TransferPtr(device->newBuffer(uint64_t(emissive_triangle_capacity) * sizeof(MetalFluxEmissiveTriangle), MTL::ResourceStorageModePrivate));
	work->emissive_triangle_block_sums = NS::TransferPtr(device->newBuffer(uint64_t(emissive_triangle_block_count) * sizeof(float), MTL::ResourceStorageModePrivate));
	work->emissive_triangle_total = NS::TransferPtr(device->newBuffer(sizeof(float), MTL::ResourceStorageModePrivate));
	const bool bidirectional_caustic_active = p_request.bidirectional_caustics && !p_request.shadow_only && !metal_bidirectional_caustic_mirrors.is_empty() && emissive_triangle_count > 0u;
	if (bidirectional_caustic_active) {
		work->bidirectional_caustic_mirror_triangles = NS::TransferPtr(device->newBuffer(metal_bidirectional_caustic_mirrors.ptr(), metal_bidirectional_caustic_mirrors.size() * sizeof(MetalFluxBidirectionalCausticMirrorTriangle), MTL::ResourceStorageModeShared));
		if (!work->bidirectional_caustic_mirror_triangles) {
			memdelete(work);
			return _hybrid_fail(ERR_CANT_CREATE, "Metal Flux bidirectional caustic mirror table allocation failed.", r_error);
		}
	}
	work->emissive_triangle_build_parameters.emissive_count = emissive_count;
	work->emissive_triangle_build_parameters.triangle_capacity = emissive_triangle_count;
	work->emissive_triangle_build_parameters.block_count = emissive_triangle_block_count;
	r_result.bidirectional_caustic_source_triangle_count = emissive_triangle_count;
	r_result.bidirectional_caustic_active = bidirectional_caustic_active;
	r_result.bidirectional_caustic_complete = bidirectional_caustic_active;
	r_result.bidirectional_caustic_backend_prototype = p_request.bidirectional_caustics;
	work->punctual_lights = NS::TransferPtr(device->newBuffer(metal_punctual_lights.ptr(), metal_punctual_lights.size() * sizeof(MetalFluxPunctualLight), MTL::ResourceStorageModeShared));
	work->portals = NS::TransferPtr(device->newBuffer(metal_portals.ptr(), metal_portals.size() * sizeof(MetalFluxPortal), MTL::ResourceStorageModeShared));
	r_result.light_distribution_identity = light_distribution_identity;
	r_result.light_distribution_generation = light_distribution_identity;
	r_result.environment_generation = p_request.environment.active ? p_request.environment.radiance_content_generation : 0;
	work->material_diagnostic->light_revision = r_result.light_distribution_generation;
	r_result.transport_revisions_valid = !p_request.shadow_only && r_result.residency_complete;
	r_result.diffuse_cache_bytes = 0;
	Vector<RD::CallbackResource> resources;
	for (const Surface &surface : p_request.surfaces) {
		if (surface.vertex_buffer.is_valid()) {
			resources.push_back({ .rid = surface.vertex_buffer, .type = RD::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RD::CALLBACK_RESOURCE_USAGE_VERTEX_BUFFER_READ });
		}
		if (surface.index_buffer.is_valid()) {
			resources.push_back({ .rid = surface.index_buffer, .type = RD::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RD::CALLBACK_RESOURCE_USAGE_INDEX_BUFFER_READ });
		}
		if (surface.has_uv && surface.attribute_buffer.is_valid()) {
			resources.push_back({ .rid = surface.attribute_buffer, .type = RD::CALLBACK_RESOURCE_TYPE_BUFFER, .usage = RD::CALLBACK_RESOURCE_USAGE_VERTEX_BUFFER_READ });
		}
	}
	for (const RID &texture : sampled_texture_resources) {
		resources.push_back({ .rid = texture, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
	}
	for (const View &view : p_request.views) {
		if (view.primary_material.is_valid()) {
			resources.push_back({ .rid = view.primary_material, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		}
		if (view.primary_identity.is_valid()) {
			resources.push_back({ .rid = view.primary_identity, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		}
		if (view.primary_geometry.is_valid()) {
			resources.push_back({ .rid = view.primary_geometry, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		}
		if (view.primary_flags.is_valid()) {
			resources.push_back({ .rid = view.primary_flags, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		}
		MTL::Texture *color = view.color.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.color)) : nullptr;
		MTL::Texture *depth = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.depth));
		MTL::Texture *normal = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.normal_roughness));
		MTL::Texture *primary_material = view.primary_material.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.primary_material)) : nullptr;
		MTL::Texture *primary_identity = view.primary_identity.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.primary_identity)) : nullptr;
		MTL::Texture *primary_geometry = view.primary_geometry.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.primary_geometry)) : nullptr;
		MTL::Texture *primary_flags = view.primary_flags.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.primary_flags)) : nullptr;
		MTL::Texture *effect_output = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.effect_output));
		MTL::Texture *filtered_output = reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.filtered_output));
		MTL::Texture *velocity = view.velocity.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.velocity)) : nullptr;
		MTL::Texture *history_input = view.history_input.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.history_input)) : nullptr;
		MTL::Texture *history_output = view.history_output.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.history_output)) : nullptr;
		MTL::Texture *depth_history_input = view.depth_history_input.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.depth_history_input)) : nullptr;
		MTL::Texture *depth_history_output = view.depth_history_output.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.depth_history_output)) : nullptr;
		MTL::Texture *normal_history_input = view.normal_history_input.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.normal_history_input)) : nullptr;
		MTL::Texture *normal_history_output = view.normal_history_output.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.normal_history_output)) : nullptr;
		MTL::Texture *guide_normal = view.guide_normal.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_normal)) : nullptr;
		MTL::Texture *guide_diffuse = view.guide_diffuse.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_diffuse)) : nullptr;
		MTL::Texture *guide_specular = view.guide_specular.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_specular)) : nullptr;
		MTL::Texture *guide_roughness = view.guide_roughness.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_roughness)) : nullptr;
		MTL::Texture *guide_denoise_strength = view.guide_denoise_strength.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_denoise_strength)) : nullptr;
		MTL::Texture *guide_reactive = view.guide_reactive.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_reactive)) : nullptr;
		MTL::Texture *guide_specular_distance = view.guide_specular_distance.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_specular_distance)) : nullptr;
		MTL::Texture *guide_transparency = view.guide_transparency.is_valid() ? reinterpret_cast<MTL::Texture *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, view.guide_transparency)) : nullptr;
		const bool any_temporal = velocity || history_input || history_output || depth_history_input || depth_history_output || normal_history_input || normal_history_output;
		const bool all_temporal = velocity && history_input && history_output && depth_history_input && depth_history_output && normal_history_input && normal_history_output;
		const bool complete_guides = guide_normal && guide_diffuse && guide_specular && guide_roughness && guide_denoise_strength && guide_reactive && guide_specular_distance && guide_transparency;
		const bool any_primary_surface = primary_material || primary_identity || primary_geometry || primary_flags;
		const bool complete_primary_surface = primary_material && primary_identity && primary_geometry && primary_flags;
		if ((!p_request.shadow_only && (!color || !filtered_output || (work->metalfx_denoiser && !complete_guides) || (any_primary_surface && !complete_primary_surface))) || (any_temporal && !all_temporal) || !depth || !normal || !effect_output) {
			memdelete(work);
			return _hybrid_fail(ERR_INVALID_PARAMETER, "A hybrid view texture or MetalFX guide is invalid.", r_error);
		}
		bool compact_view = work->trace_compaction_active && !p_request.shadow_only;
		uint32_t compact_capacity = 0;
		NS::SharedPtr<MTL::Buffer> compact_queue;
		NS::SharedPtr<MTL::Buffer> compact_counts;
		NS::SharedPtr<MTL::Buffer> compact_arguments;
		if (compact_view) {
			const uint64_t pixel_count = uint64_t(p_request.shadow_only ? effect_output->width() : color->width()) * uint64_t(p_request.shadow_only ? effect_output->height() : color->height());
			const uint64_t entry_count = pixel_count * 5u;
			const uint64_t entry_bytes = entry_count * sizeof(MetalFluxTraceCompactEntry);
			if (pixel_count == 0u || pixel_count > uint64_t(UINT32_MAX) / 5u || entry_bytes / sizeof(MetalFluxTraceCompactEntry) != entry_count) {
				compact_view = false;
			} else {
				compact_capacity = uint32_t(pixel_count);
				compact_queue = NS::TransferPtr(device->newBuffer(entry_bytes, MTL::ResourceStorageModePrivate));
				compact_counts = NS::TransferPtr(device->newBuffer(sizeof(uint32_t) * 5u, MTL::ResourceStorageModeShared));
				compact_arguments = NS::TransferPtr(device->newBuffer(sizeof(MTL::DispatchThreadgroupsIndirectArguments) * 5u, MTL::ResourceStorageModePrivate));
				if (!compact_queue || !compact_counts || !compact_arguments) {
					compact_view = false;
					compact_queue.reset();
					compact_counts.reset();
					compact_arguments.reset();
				} else {
					memset(compact_counts->contents(), 0, sizeof(uint32_t) * 5u);
				}
			}
		}
		work->trace_queues.push_back(compact_queue);
		work->trace_queue_counts.push_back(compact_counts);
		work->trace_indirect_arguments.push_back(compact_arguments);
		work->trace_queue_capacity.push_back(compact_capacity);
		work->trace_compaction_view_active.push_back(compact_view ? 1u : 0u);
		work->trace_compaction_active &= compact_view;
		work->trace_compaction_fallback |= !compact_view;
		bool transport_history_valid = false;
		// ReGIR is intentionally a single-view cache. Screen-space history remains
		// per-view, while these cell keys are derived only from world position.
		if (METAL_FLUX_REGIR_SUPPORTED && !p_request.shadow_only && p_request.regir_direct_reuse && p_request.views.size() == 1 && view.world_from_view.origin.is_finite()) {
			constexpr uint32_t REGIR_DIM_X = 4;
			constexpr uint32_t REGIR_DIM_Y = 2;
			constexpr uint32_t REGIR_DIM_Z = 4;
			constexpr int32_t REGIR_CELL_SIZE_METERS = 16;
			const uint32_t cell_count = REGIR_DIM_X * REGIR_DIM_Y * REGIR_DIM_Z;
			const uint32_t staging_blocks = ((color->width() + 7u) / 8u) * ((color->height() + 7u) / 8u);
			const int32_t center_x = int32_t(Math::floor(view.world_from_view.origin.x / float(REGIR_CELL_SIZE_METERS)));
			const int32_t center_y = int32_t(Math::floor(view.world_from_view.origin.y / float(REGIR_CELL_SIZE_METERS)));
			const int32_t center_z = int32_t(Math::floor(view.world_from_view.origin.z / float(REGIR_CELL_SIZE_METERS)));
			const uint64_t revisions[4] = { r_result.admitted_geometry_generation, r_result.visibility_residency_generation, r_result.light_distribution_generation, r_result.environment_generation };
			bool revisions_match = cache->regir_valid;
			for (uint32_t revision = 0; revision < 4; revision++) revisions_match &= cache->regir_revision[revision] == revisions[revision];
			if (!cache->regir_cells[0] || !cache->regir_cells[1] || !cache->regir_staging || cache->regir_stage_width != color->width() || cache->regir_stage_height != color->height()) {
				cache->regir_cells[0] = NS::TransferPtr(device->newBuffer(uint64_t(cell_count) * sizeof(MetalFluxRegirCell), MTL::ResourceStorageModePrivate));
				cache->regir_cells[1] = NS::TransferPtr(device->newBuffer(uint64_t(cell_count) * sizeof(MetalFluxRegirCell), MTL::ResourceStorageModePrivate));
				cache->regir_staging = NS::TransferPtr(device->newBuffer(uint64_t(staging_blocks) * cell_count * sizeof(MetalFluxRegirCandidate), MTL::ResourceStorageModePrivate));
				cache->regir_stage_width = color->width();
				cache->regir_stage_height = color->height();
				cache->regir_valid = false;
				revisions_match = false;
			}
			if (cache->regir_cells[0] && cache->regir_cells[1] && cache->regir_staging) {
				MetalFluxRegirHeader header = {};
				header.center_and_cell_size = simd_make_int4(center_x, center_y, center_z, REGIR_CELL_SIZE_METERS);
				header.previous_center_and_flags = simd_make_int4(cache->regir_center_x, cache->regir_center_y, cache->regir_center_z, revisions_match ? 1 : 0);
				header.dimensions_and_blocks = simd_make_uint4(REGIR_DIM_X, REGIR_DIM_Y, REGIR_DIM_Z, staging_blocks);
				header.revisions = simd::ulong4{ revisions[0], revisions[1], revisions[2], revisions[3] };
				work->regir_header = NS::TransferPtr(device->newBuffer(&header, sizeof(header), MTL::ResourceStorageModeShared));
				if (work->regir_header) {
					const uint32_t previous_grid = cache->regir_current;
					const uint32_t next_grid = previous_grid ^ 1u;
					work->regir_input = cache->regir_cells[previous_grid];
					work->regir_output = cache->regir_cells[next_grid];
					work->regir_staging = cache->regir_staging;
					work->regir_cell_count = cell_count;
					work->regir_staging_blocks = staging_blocks;
					work->regir_enabled = true;
					r_result.regir_enabled = true;
					r_result.regir_valid = revisions_match;
					r_result.regir_complete = true;
					r_result.regir_cells = cell_count;
					r_result.regir_bytes = uint64_t(cell_count) * sizeof(MetalFluxRegirCell) * 2u + uint64_t(staging_blocks) * cell_count * sizeof(MetalFluxRegirCandidate);
					cache->regir_current = next_grid;
					cache->regir_center_x = center_x;
					cache->regir_center_y = center_y;
					cache->regir_center_z = center_z;
					for (uint32_t revision = 0; revision < 4; revision++) cache->regir_revision[revision] = revisions[revision];
					cache->regir_valid = true;
				}
				}
			}
			// Secondary-hit reuse is intentionally separate from ReGIR and diffuse
			// radiance. It is a bounded ABI-v3 world cache; changing a camera merely
			// recenters the grid and retains overlapping exact world cells.
			if (METAL_FLUX_REUSABLE_PATH_SUPPORTED && !p_request.shadow_only && p_request.reusable_path_reuse && p_request.views.size() == 1 && view.world_from_view.origin.is_finite()) {
				constexpr uint32_t PATH_DIM_X = 4;
				constexpr uint32_t PATH_DIM_Y = 2;
				constexpr uint32_t PATH_DIM_Z = 4;
				constexpr int32_t PATH_CELL_SIZE_METERS = 16;
				const uint32_t path_cell_count = PATH_DIM_X * PATH_DIM_Y * PATH_DIM_Z;
				const int32_t center_x = int32_t(Math::floor(view.world_from_view.origin.x / float(PATH_CELL_SIZE_METERS)));
				const int32_t center_y = int32_t(Math::floor(view.world_from_view.origin.y / float(PATH_CELL_SIZE_METERS)));
				const int32_t center_z = int32_t(Math::floor(view.world_from_view.origin.z / float(PATH_CELL_SIZE_METERS)));
				const uint64_t revisions[2] = { r_result.admitted_geometry_generation, r_result.visibility_residency_generation };
				bool revisions_match = cache->reusable_path_valid;
				for (uint32_t revision = 0; revision < 2; revision++) revisions_match &= cache->reusable_path_revision[revision] == revisions[revision];
				if (!cache->reusable_path_cells[0] || !cache->reusable_path_cells[1] || !cache->reusable_path_staging || !cache->reusable_path_staging_claims) {
					cache->reusable_path_cells[0] = NS::TransferPtr(device->newBuffer(uint64_t(path_cell_count) * sizeof(RendererPathTracing::ReusablePathSampleGpuRecord), MTL::ResourceStorageModePrivate));
					cache->reusable_path_cells[1] = NS::TransferPtr(device->newBuffer(uint64_t(path_cell_count) * sizeof(RendererPathTracing::ReusablePathSampleGpuRecord), MTL::ResourceStorageModePrivate));
					cache->reusable_path_staging = NS::TransferPtr(device->newBuffer(uint64_t(path_cell_count) * sizeof(RendererPathTracing::ReusablePathSampleGpuRecord), MTL::ResourceStorageModePrivate));
					cache->reusable_path_staging_claims = NS::TransferPtr(device->newBuffer(uint64_t(path_cell_count) * sizeof(uint32_t), MTL::ResourceStorageModePrivate));
					cache->reusable_path_valid = false;
					revisions_match = false;
				}
				if (cache->reusable_path_cells[0] && cache->reusable_path_cells[1] && cache->reusable_path_staging && cache->reusable_path_staging_claims) {
					MetalFluxReusablePathHeader header = {};
					header.center_and_cell_size = simd_make_int4(center_x, center_y, center_z, PATH_CELL_SIZE_METERS);
					header.previous_center_and_flags = simd_make_int4(cache->reusable_path_center_x, cache->reusable_path_center_y, cache->reusable_path_center_z, revisions_match ? 1 : 0);
					header.dimensions_and_flags = simd_make_uint4(PATH_DIM_X, PATH_DIM_Y, PATH_DIM_Z, 1u);
					header.geometry_and_residency_revisions = simd::ulong2{ revisions[0], revisions[1] };
					header.frame = p_request.frame_index;
					work->reusable_path_header = NS::TransferPtr(device->newBuffer(&header, sizeof(header), MTL::ResourceStorageModeShared));
					if (work->reusable_path_header) {
						const uint32_t previous_grid = cache->reusable_path_current;
						const uint32_t next_grid = previous_grid ^ 1u;
						work->reusable_path_previous = cache->reusable_path_cells[previous_grid];
						work->reusable_path_next = cache->reusable_path_cells[next_grid];
						work->reusable_path_staging = cache->reusable_path_staging;
						work->reusable_path_staging_claims = cache->reusable_path_staging_claims;
						work->reusable_path_cell_count = path_cell_count;
						work->reusable_path_enabled = true;
						r_result.reusable_path_cache_enabled = true;
						r_result.reusable_path_cache_valid = revisions_match;
						r_result.reusable_path_cache_complete = true;
						r_result.reusable_path_cache_cells = path_cell_count;
						r_result.reusable_path_cache_bytes = uint64_t(path_cell_count) * sizeof(RendererPathTracing::ReusablePathSampleGpuRecord) * 3u + uint64_t(path_cell_count) * sizeof(uint32_t);
						r_result.restir_gi_valid = revisions_match;
						r_result.restir_gi_complete = true;
						// One deterministic reduction lane is submitted for every cell.
						r_result.reusable_path_updates = path_cell_count;
						cache->reusable_path_current = next_grid;
						cache->reusable_path_center_x = center_x;
						cache->reusable_path_center_y = center_y;
						cache->reusable_path_center_z = center_z;
						for (uint32_t revision = 0; revision < 2; revision++) cache->reusable_path_revision[revision] = revisions[revision];
						cache->reusable_path_valid = true;
					}
				}
			}
			if (!p_request.shadow_only) {
			const uint64_t transport_owner_identity = _mix_transport_identity(view.history_owner_id, view.eye_index);
			int transport_view_index = -1;
			for (int state_index = 0; state_index < cache->transport_views.size(); state_index++) {
				if (cache->transport_views[state_index].owner_identity == transport_owner_identity) {
					transport_view_index = state_index;
					break;
				}
			}
			if (transport_view_index < 0) {
				cache->transport_views.push_back(MetalFluxEffectCache::PerViewTransportState());
				transport_view_index = cache->transport_views.size() - 1;
				cache->transport_views.write[transport_view_index].owner_identity = transport_owner_identity;
			}
			MetalFluxEffectCache::PerViewTransportState &transport = cache->transport_views.write[transport_view_index];
			const uint32_t width = color->width();
			const uint32_t height = color->height();
			if (transport.width != width || transport.height != height || !transport.reservoir[0] || !transport.diffuse_transport_sample[0] || !transport.specular_transport_sample[0]) {
				transport = MetalFluxEffectCache::PerViewTransportState();
				transport.owner_identity = transport_owner_identity;
				transport.width = width;
				transport.height = height;
				for (uint32_t ping = 0; ping < 2; ping++) {
					transport.reservoir[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA32Uint);
					transport.reservoir_sample[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA32Float);
					transport.reservoir_surface[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.reservoir_metadata[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA32Float);
					transport.primary_identity[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRG32Uint);
					transport.primary_shading[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.primary_world_position[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA32Float);
					transport.diffuse_history[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.specular_history[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.diffuse_transport_sample[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.specular_transport_sample[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.diffuse_moments[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
					transport.specular_moments[ping] = _make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float);
				}
			}
			const uint32_t previous = transport.current;
			const uint32_t next = previous ^ 1u;
			uint32_t history_invalid_reasons = 0u;
			if (!p_request.history_valid) history_invalid_reasons |= 1u;
			if (!r_result.transport_revisions_valid) history_invalid_reasons |= 2u;
			if (transport.admitted_geometry_generation != r_result.admitted_geometry_generation || transport.visibility_residency_generation != r_result.visibility_residency_generation) history_invalid_reasons |= 4u;
			if (transport.light_distribution_generation != r_result.light_distribution_generation || transport.distribution_identity != light_distribution_identity) history_invalid_reasons |= 8u;
			if (transport.environment_generation != r_result.environment_generation) history_invalid_reasons |= 16u;
			if (transport.reset_identity != diffuse_cache_revision) history_invalid_reasons |= 32u;
			const bool geometry_history_valid = p_request.history_valid && r_result.transport_revisions_valid &&
					transport.admitted_geometry_generation == r_result.admitted_geometry_generation &&
					transport.visibility_residency_generation == r_result.visibility_residency_generation;
			// Direct reservoirs/visibility must refresh immediately after a light or
			// environment revision. Geometry-valid secondary records remain allocated
			// and queryable, but cannot become screen-space radiance history until
			// their current lighting has been evaluated.
			transport_history_valid = geometry_history_valid &&
					transport.distribution_identity == light_distribution_identity &&
					transport.reset_identity == diffuse_cache_revision &&
					transport.light_distribution_generation == r_result.light_distribution_generation &&
					transport.environment_generation == r_result.environment_generation;
			r_result.transport_history_invalid_reasons |= transport_history_valid ? 0u : history_invalid_reasons;
			r_result.transport_history_valid_views += transport_history_valid ? 1u : 0u;
			transport.distribution_identity = light_distribution_identity;
			transport.reset_identity = diffuse_cache_revision;
			transport.admitted_geometry_generation = r_result.admitted_geometry_generation;
			transport.visibility_residency_generation = r_result.visibility_residency_generation;
			transport.light_distribution_generation = r_result.light_distribution_generation;
			transport.environment_generation = r_result.environment_generation;
			transport.current = next;
			work->reservoir_input.push_back(transport.reservoir[previous].get());
			work->reservoir_output.push_back(transport.reservoir[next].get());
			work->reservoir_surface_input.push_back(transport.reservoir_surface[previous].get());
			work->reservoir_surface_output.push_back(transport.reservoir_surface[next].get());
			work->reservoir_metadata_input.push_back(transport.reservoir_metadata[previous].get());
			work->reservoir_metadata_output.push_back(transport.reservoir_metadata[next].get());
			work->reservoir_sample_input.push_back(transport.reservoir_sample[previous].get());
			work->reservoir_sample_output.push_back(transport.reservoir_sample[next].get());
			work->reservoir_primary_identity_input.push_back(transport.primary_identity[previous].get());
			work->reservoir_primary_identity_output.push_back(transport.primary_identity[next].get());
			work->primary_shading_input.push_back(transport.primary_shading[previous].get());
			work->primary_shading_output.push_back(transport.primary_shading[next].get());
			work->primary_world_position_output.push_back(transport.primary_world_position[next].get());
			work->diffuse_history_input.push_back(transport.diffuse_history[previous].get());
			work->diffuse_history_output.push_back(transport.diffuse_history[next].get());
			work->specular_history_input.push_back(transport.specular_history[previous].get());
			work->specular_history_output.push_back(transport.specular_history[next].get());
			work->diffuse_transport_sample_input.push_back(transport.diffuse_transport_sample[previous].get());
			work->diffuse_transport_sample_output.push_back(transport.diffuse_transport_sample[next].get());
			work->specular_transport_sample_input.push_back(transport.specular_transport_sample[previous].get());
			work->specular_transport_sample_output.push_back(transport.specular_transport_sample[next].get());
			work->diffuse_moments_input.push_back(transport.diffuse_moments[previous].get());
			work->diffuse_moments_output.push_back(transport.diffuse_moments[next].get());
			work->specular_moments_input.push_back(transport.specular_moments[previous].get());
			work->specular_moments_output.push_back(transport.specular_moments[next].get());
			work->split_diffuse_owned.push_back(_make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float));
			work->split_specular_owned.push_back(_make_transport_texture(device, width, height, MTL::PixelFormatRGBA16Float));
			work->split_diffuse.push_back(work->split_diffuse_owned[work->split_diffuse_owned.size() - 1].get());
			work->split_specular.push_back(work->split_specular_owned[work->split_specular_owned.size() - 1].get());
		}
		work->color.push_back(color);
		work->depth.push_back(depth);
		work->normal_roughness.push_back(normal);
		work->primary_material.push_back(primary_material);
		work->primary_identity.push_back(primary_identity);
		work->primary_geometry.push_back(primary_geometry);
		work->primary_flags.push_back(primary_flags);
		work->effect_output.push_back(effect_output);
		work->filtered_output.push_back(filtered_output);
		work->velocity.push_back(velocity);
		work->history_input.push_back(history_input);
		work->history_output.push_back(history_output);
		work->depth_history_input.push_back(depth_history_input);
		work->depth_history_output.push_back(depth_history_output);
		work->normal_history_input.push_back(normal_history_input);
		work->normal_history_output.push_back(normal_history_output);
		work->guide_normal.push_back(guide_normal);
		work->guide_diffuse.push_back(guide_diffuse);
		work->guide_specular.push_back(guide_specular);
		work->guide_roughness.push_back(guide_roughness);
		work->guide_denoise_strength.push_back(guide_denoise_strength);
		work->guide_reactive.push_back(guide_reactive);
		work->guide_specular_distance.push_back(guide_specular_distance);
		work->guide_transparency.push_back(guide_transparency);
		if (p_request.contact_visibility && p_request.contact_visibility_samples >= 2u && p_request.contact_visibility_distance > 0.0f && p_request.contact_visibility_strength > 0.0f) {
			r_result.world_space_diffuse_contact_visibility_views++;
		}
		work->temporal_enabled |= all_temporal && !work->metalfx_denoiser;
		MetalFluxParameters parameters = {};
		parameters.world_from_view = _metal_matrix(view.world_from_view);
		parameters.view_from_clip = simd_inverse(_metal_matrix(view.clip_from_view));
		parameters.clip_from_world = simd_mul(_metal_matrix(view.clip_from_view), simd_inverse(parameters.world_from_view));
		parameters.prev_clip_from_world = _metal_matrix(view.prev_clip_from_world);
		parameters.light_direction_and_reflection_strength = simd_make_float4(p_request.directional_light_direction.x, p_request.directional_light_direction.y, p_request.directional_light_direction.z, p_request.reflection_strength);
		parameters.directional_light_radiance_enabled = simd_make_float4(p_request.directional_light_radiance.r, p_request.directional_light_radiance.g, p_request.directional_light_radiance.b, p_request.directional_light_active ? 1.0f : 0.0f);
		parameters.ao_distance_strength_roughness_flags = simd_make_float4(p_request.bounded_transport ? MIN(p_request.ambient_occlusion_distance, p_request.transport_max_distance) : p_request.ambient_occlusion_distance, p_request.ambient_occlusion_strength, p_request.reflection_roughness_cutoff, float((p_request.reflections ? 1u : 0u) | (p_request.ambient_occlusion ? 2u : 0u)));
		parameters.ao_distance_strength_roughness_flags.w = float((p_request.reflections ? 1u : 0u) | (p_request.ambient_occlusion ? 2u : 0u) | (p_request.global_illumination ? 4u : 0u));
		parameters.contact_visibility_info = simd_make_float4(p_request.bounded_transport ? MIN(p_request.contact_visibility_distance, p_request.transport_max_distance) : p_request.contact_visibility_distance, p_request.contact_visibility_strength, float(CLAMP(p_request.contact_visibility_samples, 2u, 4u)), p_request.contact_visibility ? 1.0f : 0.0f);
		parameters.transport_max_distance = p_request.bounded_transport ? p_request.transport_max_distance : 0.0f;
		parameters.dimensions = p_request.shadow_only ? simd_make_uint2(effect_output->width(), effect_output->height()) : simd_make_uint2(color->width(), color->height());
		parameters.shadow_sample_count = MAX(1u, p_request.shadow_sample_count);
		parameters.directional_light_angular_radius = p_request.directional_light_angular_radius;
		parameters.gi_sample_count = MAX(1u, p_request.global_illumination_samples);
		parameters.frame_index = p_request.frame_index;
		parameters.gi_strength = p_request.global_illumination_strength;
		// MetalFX consumes the current raw ray radiance and owns image-space
		// denoising/history. Until the raw secondary replay contract has a
		// frame-matched validation record, fail closed for that path: allowing a
		// stale transport sample into the MetalFX input produces wall smearing
		// after a camera move. Non-MetalFX Flux retains its validated transport
		// history path.
		parameters.history_valid = transport_history_valid && !work->metalfx_denoiser && !p_request.fresh_ray_oracle ? 1u : 0u;
		parameters.emissive_count = emissive_count;
		parameters.emissive_triangle_count = emissive_triangle_count;
		parameters.punctual_light_count = punctual_light_count;
		const RendererPathTracing::EnvironmentImportanceMetadata &environment = p_request.environment.metadata;
		parameters.world_from_radiance = _metal_matrix(Transform3D(environment.world_from_radiance, Vector3()));
		parameters.radiance_from_world = _metal_matrix(Transform3D(environment.world_from_radiance.inverse(), Vector3()));
		const float environment_state = p_request.environment.active ? (p_request.environment.primary_replacement ? 2.0f : 3.0f) : (p_request.environment.legacy_miss_fallback ? 0.0f : 1.0f);
		parameters.environment_info = simd_make_float4(environment.border, 1.0f - environment.border * 2.0f, float(MAX(1u, cache->environment_mip_count)), environment_state);
		const Vector2i importance_resolution = _environment_importance_resolution(environment.width, environment.height);
		parameters.environment_dimensions = simd_make_uint2(importance_resolution.x, importance_resolution.y);
		const RendererPathTracing::EnvironmentImportancePaddedExtent extent = RendererPathTracing::environment_importance_padded_extent(importance_resolution.x, importance_resolution.y);
		parameters.environment_importance_dimensions = simd_make_uint2(MAX(1u, extent.width), MAX(1u, extent.height));
		const FrameRequest::SolarLobe &solar = p_request.environment.solar_lobe;
		parameters.solar_current_direction_radius = simd_make_float4(solar.current_direction.x, solar.current_direction.y, solar.current_direction.z, solar.angular_radius);
		parameters.solar_previous_direction_transmittance = simd_make_float4(solar.previous_direction.x, solar.previous_direction.y, solar.previous_direction.z, solar.cloud_transmittance);
		parameters.solar_perpendicular_irradiance_enabled = simd_make_float4(solar.perpendicular_irradiance.r, solar.perpendicular_irradiance.g, solar.perpendicular_irradiance.b, solar.active ? 1.0f : 0.0f);
		parameters.solar_identity = simd_make_uint4(uint32_t(solar.source_id), uint32_t(solar.sample_id), uint32_t(solar.source_id >> 32), uint32_t(solar.sample_id >> 32));
		parameters.solar_generations = simd_make_uint4(uint32_t(solar.profile_version), uint32_t(solar.partition_version), uint32_t(solar.state_generation), uint32_t(solar.history_epoch));
		parameters.light_distribution_identity = simd_make_uint2(uint32_t(light_distribution_identity), uint32_t(light_distribution_identity >> 32));
		parameters.cache_revision = simd_make_uint2(uint32_t(diffuse_cache_revision), uint32_t(diffuse_cache_revision >> 32));
		parameters.admitted_geometry_generation = simd_make_uint2(uint32_t(r_result.admitted_geometry_generation), uint32_t(r_result.admitted_geometry_generation >> 32));
		parameters.visibility_residency_generation = simd_make_uint2(uint32_t(r_result.visibility_residency_generation), uint32_t(r_result.visibility_residency_generation >> 32));
		parameters.light_distribution_generation = simd_make_uint2(uint32_t(r_result.light_distribution_generation), uint32_t(r_result.light_distribution_generation >> 32));
		parameters.environment_generation = simd_make_uint2(uint32_t(r_result.environment_generation), uint32_t(r_result.environment_generation >> 32));
		parameters.portal_count = portal_count;
		parameters.portal_generation = uint32_t(p_request.environment.portal_generation);
		parameters.adaptive_min_samples = CLAMP(p_request.transport_adaptive_min_samples, 1u, 4u);
		parameters.adaptive_max_samples = CLAMP(MAX(parameters.adaptive_min_samples, p_request.transport_adaptive_max_samples), parameters.adaptive_min_samples, 8u);
		parameters.adaptive_variance_reference = MAX(p_request.transport_adaptive_variance_reference, 0.0001f);
		parameters.diffuse_cache_cell_size = MAX(p_request.diffuse_cache_cell_size, 0.25f);
		parameters.alpha_mask_instance_count = work->alpha_intersection_enabled ? r_result.alpha_mask_instances : 0u;
		parameters.material_texture_capacity = cache->material_texture_capacity;
		parameters.geometry_record_count = metal_geometries.size();
		parameters.raster_primary_surface = complete_primary_surface ? 1u : 0u;
		// Bit 3 authorizes validated secondary *sampling metadata*, not an image
		// reconstruction path. It remains valid with MetalFX, which receives only
		// the current raw ray sample plus guides and owns all image denoising.
		parameters.reconstruction_flags = (p_request.collect_metalfx_reactive_telemetry ? 2u : 0u) | (p_request.collect_stage_probe ? 4u : 0u) | (transport_history_valid ? 8u : 0u) | (work->metalfx_denoiser ? 16u : 0u);
		parameters.directional_light_cull_mask = p_request.directional_light_cull_mask;
		parameters.directional_shadow_caster_mask = p_request.directional_shadow_caster_mask;
		parameters.directional_shadow_opacity = CLAMP(p_request.directional_shadow_opacity, 0.0f, 1.0f);
		parameters.directional_specular_amount = MAX(p_request.directional_specular_amount, 0.0f);
		parameters.directional_flags = (p_request.directional_shadow_enabled ? 1u : 0u) | (p_request.directional_negative ? 2u : 0u);
		parameters.experimental_feature_flags = (!p_request.fresh_ray_oracle && METAL_FLUX_RESTIR_DI_TEMPORAL_SPATIAL_REUSE_SUPPORTED && p_request.restir_di_reuse ? 1u : 0u) |
				(work->regir_enabled ? 2u : 0u) |
				(work->reusable_path_enabled ? 4u : 0u) |
				(!p_request.fresh_ray_oracle && p_request.unified_finite_light_reservoir ? 8u : 0u) |
				(work->split_alpha_domains_exact ? 16u : 0u) |
				// A shader-visible oracle bit keeps every reuse gate fail-closed even
				// if a future host call accidentally enables one of its request flags.
				(p_request.fresh_ray_oracle ? 0x80000000u : 0u);
		const bool stbn_active = stbn_requested && work->stbn_scalar_volume;
		parameters.sampling_sequence_mode = stbn_active ? 1u : 0u;
		parameters.sampling_tile_width = stbn_active ? RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_WIDTH : 0u;
		parameters.sampling_tile_height = stbn_active ? RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_HEIGHT : 0u;
		parameters.sampling_tile_depth = stbn_active ? RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH : 0u;
		parameters.sampling_tile_channels = stbn_active ? RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT : 0u;
		parameters.bidirectional_caustic_mirror_count = bidirectional_caustic_active ? metal_bidirectional_caustic_mirrors.size() : 0u;
		parameters.bidirectional_caustic_source_count = bidirectional_caustic_active ? emissive_triangle_count : 0u;
		parameters.bidirectional_caustic_delta_roughness_threshold = CLAMP(p_request.bidirectional_caustic_delta_roughness_threshold, 0.0f, 0.1f);
		parameters.bidirectional_caustic_flags = (p_request.bidirectional_caustics ? 1u : 0u) | (bidirectional_caustic_active ? 2u : 0u);
		if (parameters.raster_primary_surface != 0u) {
			r_result.raster_primary_surface_views++;
		}
		work->parameters.push_back(parameters);
		if (view.color.is_valid()) {
			resources.push_back({ .rid = view.color, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		resources.push_back({ .rid = view.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		resources.push_back({ .rid = view.normal_roughness, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE });
		resources.push_back({ .rid = view.effect_output, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		if (view.filtered_output.is_valid()) {
			resources.push_back({ .rid = view.filtered_output, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		const RID guide_resources[] = { view.guide_normal, view.guide_diffuse, view.guide_specular, view.guide_roughness, view.guide_denoise_strength, view.guide_reactive, view.guide_specular_distance, view.guide_transparency };
		for (const RID &resource : guide_resources) {
			if (resource.is_valid()) {
				resources.push_back({ .rid = resource, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
			}
		}
		const RID temporal_resources[] = { view.velocity, view.history_input, view.history_output, view.depth_history_input, view.depth_history_output, view.normal_history_input, view.normal_history_output };
		for (const RID &resource : temporal_resources) {
			if (resource.is_valid()) {
				resources.push_back({ .rid = resource, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
			}
		}
	}
	work->material_diagnostic->trace_compaction_active = work->trace_compaction_active;
	work->material_diagnostic->trace_compaction_fallback = work->trace_compaction_fallback;
	r_result.trace_compaction_active = work->trace_compaction_active;
	r_result.trace_compaction_fallback = work->trace_compaction_fallback;
	if (p_request.collect_gpu_timings && cache->timing_captures.is_empty() && device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary)) {
		MTL::CounterSet *timestamp_set = _timestamp_counter_set(device);
		if (timestamp_set) {
			NS::SharedPtr<MTL::CounterSampleBufferDescriptor> descriptor = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
			descriptor->setCounterSet(timestamp_set);
			descriptor->setSampleCount(4 + p_request.views.size() * 16);
			descriptor->setStorageMode(MTL::StorageModeShared);
			NS::Error *counter_error = nullptr;
			std::shared_ptr<MetalFluxTimingCapture> timing = std::make_shared<MetalFluxTimingCapture>();
			timing->samples = NS::TransferPtr(device->newCounterSampleBuffer(descriptor.get(), &counter_error));
			timing->view_count = p_request.views.size();
			timing->shadow_only = p_request.shadow_only;
			timing->diagnostics_owner_id = p_request.diagnostics_owner_id;
			timing->diagnostics_frame = p_request.diagnostics_frame;
			if (timing->samples) {
				work->timing = timing;
				cache->timing_captures.push_back(timing);
				r_result.gpu_timing_capture_submitted = true;
			}
		}
	}
	const uint64_t cpu_submission_begin = mach_absolute_time();
	r_result.cpu_metal_preparation_milliseconds = cpu_milliseconds_since(cpu_residency_end, cpu_submission_begin);
	rd->capture_timestamp("Hybrid BLAS TLAS Ray Effects Begin");
	Error callback_error = rd->driver_callback_add((RDD::DriverCallback)_hybrid_callback, work, VectorView<RD::CallbackResource>(resources.ptr(), resources.size()));
	rd->capture_timestamp("Hybrid BLAS TLAS Ray Effects End");
	if (callback_error != OK) {
		memdelete(work);
		return _hybrid_fail(callback_error, "Metal hybrid work could not be scheduled.", r_error);
	}
	r_result.cpu_submission_milliseconds = cpu_milliseconds_since(cpu_submission_begin, mach_absolute_time());
	r_result.rendered_views = p_request.views.size();
	r_result.punctual_lights = punctual_light_count;
	r_result.punctual_light_overflow = p_request.punctual_light_overflow;
	r_result.unsupported_punctual_lights = p_request.unsupported_punctual_lights;
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error MetalFluxEffect::collect_completed_timings(Vector<StageTiming> &r_timings, String *r_error) {
	r_timings.clear();
	for (int capture_index = cache->timing_captures.size() - 1; capture_index >= 0; capture_index--) {
		const std::shared_ptr<MetalFluxTimingCapture> &capture = cache->timing_captures[capture_index];
		if (!capture->complete.load(std::memory_order_acquire)) {
			continue;
		}
		const uint32_t sample_count = 4 + capture->view_count * 16;
		NS::Data *data = capture->samples->resolveCounterRange(NS::Range::Make(0, sample_count));
		if (data && capture->gpu_end > capture->gpu_begin && capture->cpu_end > capture->cpu_begin) {
			const MTL::CounterResultTimestamp *samples = static_cast<const MTL::CounterResultTimestamp *>(data->bytes());
			mach_timebase_info_data_t timebase = {};
			mach_timebase_info(&timebase);
			const double nanoseconds_per_gpu_tick = (double(capture->cpu_end - capture->cpu_begin) * double(timebase.numer) / double(timebase.denom)) / double(capture->gpu_end - capture->gpu_begin);
			auto append_timing = [&](const StringName &p_stage, uint64_t p_begin, uint64_t p_end) {
				if (p_end > p_begin) {
					r_timings.push_back({ p_stage, double(p_end - p_begin) * nanoseconds_per_gpu_tick / 1000000.0, capture->diagnostics_owner_id, capture->diagnostics_frame });
				}
			};
			append_timing("blas", samples[0].timestamp, samples[1].timestamp);
			append_timing("tlas", samples[2].timestamp, samples[3].timestamp);
			double trace_ms = 0.0;
			double queue_ms[5] = {};
			double reconstruction_ms = 0.0;
			double temporal_ms = 0.0;
			double composition_ms = 0.0;
			for (uint32_t view = 0; view < capture->view_count; view++) {
				const uint32_t base = 4 + view * 16;
				for (uint32_t queue = 0; queue < 5; queue++) {
					const uint32_t queue_base = base + queue * 2;
					if (samples[queue_base + 1].timestamp > samples[queue_base].timestamp) {
						queue_ms[queue] += double(samples[queue_base + 1].timestamp - samples[queue_base].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
					}
				}
				trace_ms += queue_ms[0] + queue_ms[1] + queue_ms[2] + queue_ms[3] + queue_ms[4];
				if (samples[base + 11].timestamp > samples[base + 10].timestamp) reconstruction_ms += double(samples[base + 11].timestamp - samples[base + 10].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
				if (samples[base + 13].timestamp > samples[base + 12].timestamp) temporal_ms += double(samples[base + 13].timestamp - samples[base + 12].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
				if (samples[base + 15].timestamp > samples[base + 14].timestamp) composition_ms += double(samples[base + 15].timestamp - samples[base + 14].timestamp) * nanoseconds_per_gpu_tick / 1000000.0;
			}
			r_timings.push_back({ capture->shadow_only ? StringName("ray_shadows") : StringName("ray_effects"), trace_ms, capture->diagnostics_owner_id, capture->diagnostics_frame });
			if (!capture->shadow_only) {
				r_timings.push_back({ "ray_direct", queue_ms[0], capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "ray_gi", queue_ms[1], capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "ray_reflection", queue_ms[2], capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "ray_exact_alpha", queue_ms[3], capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "ray_complex", queue_ms[4], capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "spatial_reconstruction", reconstruction_ms, capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "temporal_reconstruction", temporal_ms, capture->diagnostics_owner_id, capture->diagnostics_frame });
				r_timings.push_back({ "composition", composition_ms, capture->diagnostics_owner_id, capture->diagnostics_frame });
			}
		} else {
			// Do not leave the matching diagnostics frame pending indefinitely when
			// the driver cannot resolve this counter range. RenderFlux publishes this
			// explicit frame-matched unavailable marker, then schedules a later retry.
			r_timings.push_back({ "gpu_timing_unavailable", 0.0, capture->diagnostics_owner_id, capture->diagnostics_frame });
		}
		cache->timing_captures.remove_at(capture_index);
	}
	if (r_timings.is_empty()) {
		return _hybrid_fail(ERR_BUSY, "No completed Metal hybrid GPU counter capture is available.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

Error MetalFluxEffect::collect_completed_work_attribution(Vector<WorkAttribution> &r_attribution, String *r_error) {
	r_attribution = cache->completed_work_attribution;
	cache->completed_work_attribution.clear();
	if (r_attribution.is_empty()) {
		return _hybrid_fail(ERR_BUSY, "No completed Metal Flux work-attribution capture is available.", r_error);
	}
	if (r_error) {
		r_error->clear();
	}
	return OK;
}

} // namespace RendererRD

#endif
