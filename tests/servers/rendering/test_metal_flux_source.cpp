/**************************************************************************/
/*  test_metal_flux_source.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_metal_flux_source)

#include "core/io/file_access.h"

namespace TestMetalFluxSource {

TEST_CASE("[Flux][Metal][VirtualGeometry] VG4 transport groups are independent of raster cuts") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	Ref<FileAccess> compiler = FileAccess::open("modules/meshoptimizer/virtual_geometry_compiler.cpp", FileAccess::READ);
	REQUIRE(compiler.is_valid());
	const String compiler_text = compiler->get_as_text();
	CHECK(text.contains("Virtual transport is assembled from compiler ray-group hints, never from"));
	CHECK(text.contains("storage->get_ray_group_descriptors()"));
	CHECK(text.contains("VirtualGeometryRequestReason::TRANSPORT"));
	CHECK(text.contains("coarse_complete_by_region"));
	CHECK(text.contains("ray_group.persistent_coarse"));
	CHECK(text.contains("off_screen_influence = true"));
	CHECK(text.contains("off_screen_transport = true"));
	CHECK_FALSE(text.contains("ray_group.stable_id, selected_cluster"));
	CHECK(compiler_text.find("_emit_ray_groups(manifest);") < compiler_text.find("// Deterministic page packing."));
	CHECK(compiler_text.contains("_hash_u64(p_manifest.source_primitive_identity"));
	CHECK(compiler_text.contains("p_manifest.ray_hint_schema_version"));
	CHECK(compiler_text.contains("for (uint64_t cluster_id : p_clusters)"));
}

TEST_CASE("[Flux][Metal][VirtualGeometry] VG4 native AS lifecycle is completion gated and retained") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("writeCompactedAccelerationStructureSize"));
	CHECK(text.contains("copyAndCompactAccelerationStructure"));
	CHECK(text.contains("compaction_copy_serial <= completed_submission_serial"));
	CHECK(text.contains("compaction_query_serial <= completed_submission_serial"));
	CHECK(text.contains("retired_acceleration_structures"));
	CHECK(text.contains("last_use_serial <= completed_submission_serial"));
	CHECK(text.contains("selected_group_by_region"));
	CHECK(text.contains("highest complete tier per stable"));
	CHECK(text.contains("command->retain_resource(readback.get())"));
	CHECK(text.contains("command->retain_resource(destination.get())"));
}

TEST_CASE("[Flux][Metal] Scalar STBN sampling has array binding, frame advancement, dimensions, and fallback") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("texture2d_array<ushort, access::read> stbn_scalar_volume [[texture(57)]]"));
	CHECK(text.contains("descriptor->setTextureType(MTL::TextureType2DArray)"));
	CHECK(text.contains("descriptor->setArrayLength(RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH * RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_CHANNEL_COUNT)"));
	CHECK(text.contains("trace->setTexture(p_work->stbn_scalar_volume.get(), 57)"));
	CHECK(text.contains("FLUX_SAMPLE_DIMENSION_PRIMARY_GI_U"));
	CHECK(text.contains("const uint channel = dimension % parameters.sampling_tile_channels"));
	CHECK(text.contains("static uint stbn_tile_scramble"));
	CHECK(text.contains("const uint tile_scramble = stbn_tile_scramble(stbn_pixel, dimension, parameters)"));
	CHECK(text.contains("const uint layer = channel * parameters.sampling_tile_depth + slice"));
	CHECK(text.contains("stbn_scalar_volume.read(tile_pixel, layer)"));
	CHECK_FALSE(text.contains("state = stbn_rank"));
	CHECK(text.contains("scalar STBN volume is unavailable; using progressive"));
	CHECK(text.contains("parameters.sampling_sequence_mode = stbn_active ? 1u : 0u"));
	CHECK(text.contains("parameters.sampling_tile_depth = stbn_active ? RendererPathTracing::SAMPLE_SEQUENCE_STBN_SCALAR_VOLUME_DEPTH : 0u"));
}

TEST_CASE("[Flux][Metal] thin dielectric transmission is scalar, finite, and bounded") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	Ref<FileAccess> importer = FileAccess::open("modules/gltf/gltf_document.cpp", FileAccess::READ);
	REQUIRE(importer.is_valid());
	const String importer_text = importer->get_as_text();
	CHECK(text.contains("float2 thin_transmission_ior"));
	CHECK(text.contains("static bool thin_dielectric_refraction"));
	CHECK(text.contains("const float sine_t2"));
	CHECK(text.contains("sine_t2 >= 1.0f"));
	CHECK(text.contains("f0_squared + (1.0f - f0_squared) * pow(1.0f - cosine_i, 5.0f)"));
	CHECK(text.contains("static HybridIntersection hybrid_intersect_thin_reconnect"));
	CHECK(text.contains("static float3 thin_visibility_transmittance"));
	CHECK(text.contains("Direct-light connections must remain straight-line proposals"));
	CHECK(text.contains("A third pane is deliberately opaque"));
	CHECK(text.contains("selected_visibility_transmittance = thin_visibility_transmittance"));
	CHECK(text.contains("const float3 thin_visibility = thin_visibility_transmittance"));
	CHECK(text.contains("const float3 visibility_transmittance = thin_visibility_transmittance"));
	CHECK(text.contains("direct_selected_visibility"));
	CHECK(text.contains("direct_candidate_evaluations"));
	CHECK(text.contains("for (uint pane = 0u; pane < 2u; pane++)"));
	CHECK(text.contains("reflection_transmission"));
	CHECK(text.contains("gi_transmission"));
	CHECK(importer_text.contains("KHR_materials_transmission"));
	CHECK(importer_text.contains("KHR_materials_ior"));
	CHECK(importer_text.contains("KHR_materials_volume"));
	CHECK(importer_text.contains("set_thin_transmission_unsupported_features"));
}

TEST_CASE("[Flux][Metal] finite direct reservoirs use stable identities and a supported local/global mixture") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("static uint find_punctual_light"));
	CHECK(text.contains("lights[reservoir.source_index].stable_identity == reservoir.source_identity"));
	CHECK(text.contains("all(lights[index].stable_identity == reservoir.source_identity)"));
	CHECK(text.contains("0.5f * local_pdf + 0.5f / float(parameters.punctual_light_count)"));
	CHECK(text.contains("proposal_pdf /= area"));
	CHECK(text.contains("current visibility"));
	CHECK(text.contains("evaluate_reservoir_source(candidate, world_position"));
	CHECK(text.contains("primary_receiver_mask, true, materials"));
	CHECK(text.contains("evaluate_reservoir_source(reservoir, world_position"));
	CHECK(text.contains("primary_receiver_mask, true, materials"));
	CHECK(text.contains("material_diagnostic.direct_candidate_evaluations"));
	CHECK(text.contains("material_diagnostic.direct_selected_visibility"));
	CHECK(text.contains("primary_shading_input [[texture(66)]]"));
	CHECK(text.contains("Every selected proposal is evaluated"));
	CHECK(text.contains("current destination-visibility ray"));
	CHECK_FALSE(text.contains("replay_direct_visibility"));
	CHECK_FALSE(text.contains("cached_visibility"));
	CHECK_FALSE(text.contains("direct_visibility_sample_input"));
}

TEST_CASE("[Flux][Metal] MetalFX transport replay has a bounded exact-reflection refresh") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("const uint reflection_refresh_period = glossy ? 4u : 8u"));
	CHECK(text.contains("const bool reflection_refresh_due"));
	CHECK(text.contains("const bool stable_specular_transport"));
	CHECK(text.contains("sample_surface_compatible && stationary && stable_specular_transport && !reflection_refresh_due"));
	CHECK(text.contains("replay_specular_transport_sample"));
	CHECK(text.contains("one validated raw path sample, never a reconstructed or blended"));
	CHECK(text.contains("prior_specular_transport.a > 0.5f"));
	CHECK(text.contains("Zero-radiance transport is still an exact sample"));
	CHECK(text.contains("sampling validity, not contribution magnitude"));
}

TEST_CASE("[Flux][Metal] raw transport has no non-MetalFX reconstruction substitute") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("work->split_reconstruction_enabled = false"));
	CHECK(text.contains("Raw\n\t// ray transport is composed directly when MetalFX is off"));
	CHECK(text.contains("work->transport_metadata_update_enabled = p_request.use_metalfx_denoiser"));
	CHECK(text.contains("parameters.history_valid = transport_history_valid && !work->metalfx_denoiser && !p_request.fresh_ray_oracle"));
}

TEST_CASE("[Flux][Metal] fresh-ray oracle fails every reuse and reconstruction gate closed") {
	Ref<FileAccess> effect = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	Ref<FileAccess> header = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.h", FileAccess::READ);
	Ref<FileAccess> scheduler = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(effect.is_valid());
	REQUIRE(header.is_valid());
	REQUIRE(scheduler.is_valid());
	const String effect_text = effect->get_as_text();
	const String header_text = header->get_as_text();
	const String scheduler_text = scheduler->get_as_text();
	CHECK(header_text.contains("bool fresh_ray_oracle = false"));
	CHECK(scheduler_text.contains("--flux-fresh-ray-oracle"));
	CHECK(scheduler_text.contains("request.disable_reconstruction = force_reconstruction_disabled || request.fresh_ray_oracle"));
	CHECK(scheduler_text.contains("request.restir_di_reuse = false"));
	CHECK(scheduler_text.contains("request.regir_direct_reuse = false"));
	CHECK(scheduler_text.contains("request.reusable_path_reuse = false"));
	CHECK(scheduler_text.contains("request.unified_finite_light_reservoir = false"));
	CHECK(scheduler_text.contains("request.use_metalfx_denoiser = false"));
	CHECK(scheduler_text.contains("request.history_valid = false"));
	CHECK(effect_text.contains("const bool fresh_ray_oracle = (parameters.experimental_feature_flags & 0x80000000u) != 0u"));
	CHECK(effect_text.contains("p_request.fresh_ray_oracle ? 0x80000000u : 0u"));
	CHECK(effect_text.contains("const float3 fresh_emissive_direct = fresh_ray_oracle"));
	CHECK(effect_text.contains("static constexpr bool METAL_FLUX_RESTIR_DI_TEMPORAL_SPATIAL_REUSE_SUPPORTED = false"));
	CHECK(effect_text.contains("METAL_FLUX_RESTIR_DI_TEMPORAL_SPATIAL_REUSE_SUPPORTED && p_request.restir_di_reuse ? 1u : 0u"));
	CHECK(effect_text.contains("temporal/spatial ReSTIR DI reuse is disabled until receiver-dependent proposal PDFs are validated"));
	CHECK_FALSE(effect_text.contains("!p_request.fresh_ray_oracle && p_request.restir_di_reuse ? 1u : 0u"));
}

TEST_CASE("[Flux][Metal] unsynchronized diffuse cache is absent and transport revisions use radiance content") {
	Ref<FileAccess> effect = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	Ref<FileAccess> header = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.h", FileAccess::READ);
	Ref<FileAccess> scheduler = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(effect.is_valid());
	REQUIRE(header.is_valid());
	REQUIRE(scheduler.is_valid());
	const String effect_text = effect->get_as_text();
	const String header_text = header->get_as_text();
	const String scheduler_text = scheduler->get_as_text();
	CHECK_FALSE(effect_text.contains("diffuse_radiance_cache"));
	CHECK_FALSE(effect_text.contains("diffuse_cache_query_or_update"));
	CHECK_FALSE(effect_text.contains("TextureType3D"));
	CHECK(effect_text.contains("const uint64_t diffuse_cache_revision = 0"));
	CHECK(header_text.contains("uint64_t radiance_content_generation = 0"));
	CHECK(scheduler_text.contains("request.environment.radiance_content_generation = generation"));
	CHECK(effect_text.contains("r_result.environment_generation = p_request.environment.active ? p_request.environment.radiance_content_generation : 0"));
	CHECK(effect_text.contains("p_request.environment.radiance_content_generation"));
}

TEST_CASE("[Flux][Metal] cache ABI mirrors use exact active-SDK sizes and offsets") {
	Ref<FileAccess> effect = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(effect.is_valid());
	const String text = effect->get_as_text();
	CHECK(text.contains("sizeof(MetalFluxRegirHeader) == 80"));
	CHECK(text.contains("sizeof(MetalFluxRegirCell) == 96"));
	CHECK(text.contains("sizeof(MetalFluxRegirCandidate) == 64"));
	CHECK(text.contains("sizeof(MetalFluxReusablePathHeader) == 112"));
	CHECK(text.contains("offsetof(MetalFluxRegirHeader, revisions) == 48"));
	CHECK(text.contains("offsetof(MetalFluxReusablePathHeader, frame) == 80"));
	CHECK(text.contains("offsetof(RendererPathTracing::ReusablePathSampleGpuRecord, incident_radiance_r) == 256"));
}

TEST_CASE("[Flux][Metal] GPU timing snapshots stay attached to their submitted frame") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("A Flux timing belongs only to its submitted diagnostics frame"));
	CHECK_FALSE(text.contains("r_diagnostics.carry_forward_timings_from(*timing_sample)"));
	CHECK(text.contains("state->effect_capture_submitted = false"));
	CHECK(text.contains("state->shadow_capture_submitted = false"));
	CHECK(text.contains("gpu_timing_unavailable"));
}

TEST_CASE("[Flux][Metal] ReGIR prerequisites use committed scene revisions and exact ray primary positions") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("_transport_set_identity(admitted_geometry_entries"));
	CHECK(text.contains("surface.topology_revision"));
	CHECK(text.contains("surface.deformation_revision"));
	CHECK(text.contains("instance.material_generation"));
	CHECK(text.contains("instance.transform.origin.x"));
	CHECK(text.contains("residency_commit = cache->residency_planner.commit()"));
	CHECK(text.contains("reuse_committed_frame(cache->frame, completed_residency_token, cache->stable_residency_plan.visible_keys)"));
	CHECK(text.contains("surface.topology_revision"));
	CHECK(text.contains("instance.material_generation"));
	CHECK(text.contains("instance.alpha_occupancy_texture"));
	CHECK(text.contains("cache->residency_budgets.pools[pool]"));
	CHECK(text.contains("stable_residency_inputs_ready"));
	CHECK(text.contains("resident_texture_slots"));
	CHECK(text.contains("visibility_residency_entries = deferred_blas_entries"));
	CHECK(text.contains("r_result.residency_completion_token = completed_residency_token"));
	CHECK(text.contains("visibility_residency_complete = deferred_blas_entries.is_empty()"));
	CHECK(text.contains("r_result.residency_complete = visibility_residency_complete"));
	CHECK_FALSE(text.contains("visibility_residency_entries.push_back(query.retirement_completion_token)"));
	CHECK(text.contains("transport.admitted_geometry_generation == r_result.admitted_geometry_generation"));
	CHECK(text.contains("transport.visibility_residency_generation == r_result.visibility_residency_generation"));
	CHECK(text.contains("transport.light_distribution_generation == r_result.light_distribution_generation"));
	CHECK(text.contains("transport.environment_generation == r_result.environment_generation"));
	CHECK(text.contains("transport.primary_world_position[ping]"));
	CHECK(text.contains("texture2d<float, access::write> primary_world_position_output [[texture(58)]]"));
	CHECK(text.contains("primary_world_position_output.write(float4(0.0f), pixel)"));
	CHECK(text.contains("primary_world_position_output.write(any(primary_surface_identity != uint2(0u))"));
	CHECK(text.contains("trace->setTexture(p_work->primary_world_position_output[view], 58)"));
	CHECK(text.contains("trace->useResource(p_work->primary_world_position_output[view], MTL::ResourceUsageWrite)"));
}

TEST_CASE("[Flux][Metal] ReGIR remains fail-closed until its proposal PDF is complete") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("struct RegirHeader"));
	CHECK(text.contains("struct RegirCell"));
	CHECK(text.contains("ulong4 revisions"));
	CHECK(text.contains("kernel void regir_scroll"));
	CHECK(text.contains("kernel void regir_classify"));
	CHECK(text.contains("kernel void regir_reduce"));
	CHECK(text.contains("static constexpr bool METAL_FLUX_REGIR_SUPPORTED = false"));
	CHECK(text.contains("METAL_FLUX_REGIR_SUPPORTED && !p_request.shadow_only && p_request.regir_direct_reuse"));
	CHECK(text.contains("work->regir_enabled ? 2u : 0u"));
	CHECK(text.contains("ReGIR is disabled until its complete proposal-PDF contract is validated"));
	CHECK_FALSE(text.contains("(p_request.regir_direct_reuse ? 2u : 0u)"));
	CHECK_FALSE(text.contains("atomic_fetch_add_explicit(&regir"));
}

TEST_CASE("[Flux][Metal] reusable path transport is ABI-checked but fail-closed") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("static constexpr bool METAL_FLUX_REUSABLE_PATH_SUPPORTED = false"));
	CHECK(text.contains("METAL_FLUX_REUSABLE_PATH_SUPPORTED && !p_request.shadow_only && p_request.reusable_path_reuse"));
	CHECK(text.contains("const bool reusable_path_transport_enabled = false"));
	CHECK(text.contains("uint sample_count = max(parameters.gi_sample_count, 1u)"));
	CHECK_FALSE(text.contains("reused_path_replaces_fresh ? 0u"));
	CHECK(text.contains("Flux: reusable path transport is disabled until current-measure endpoint transport is validated"));
	CHECK(text.contains("struct ReusablePathSampleRecord"));
	CHECK(text.contains("float secondary_barycentric_u"));
	CHECK(text.contains("float secondary_barycentric_v"));
	CHECK(text.contains("struct ReusablePathHeader"));
	CHECK(text.contains("ulong2 geometry_and_residency_revisions"));
	CHECK(text.contains("uint64_t reusable_path_revision[2]"));
	CHECK(text.contains("kernel void reusable_path_clear_staging"));
	CHECK(text.contains("kernel void reusable_path_reduce"));
	CHECK(text.contains("device const ReusablePathSampleRecord *reusable_path_previous [[buffer(13)]]"));
	CHECK(text.contains("device ReusablePathSampleRecord *reusable_path_staging [[buffer(15)]]"));
	CHECK(text.contains("trace->setBuffer(p_work->reusable_path_previous.get(), 0, 13)"));
	CHECK(text.contains("trace->setBuffer(p_work->reusable_path_staging.get(), 0, 15)"));
	CHECK(text.contains("device atomic_uint *reusable_path_staging_claims [[buffer(21)]]"));
	CHECK(text.contains("atomic_compare_exchange_weak_explicit(&reusable_path_staging_claims[cell_index]"));
	CHECK(text.contains("for (uint attempt = 0u; attempt < 2u && !claimed_staging_cell; attempt++)"));
	CHECK_FALSE(text.contains("reusable_path_staging_claims[cell_index], &expected_claim, pixel.y * parameters.dimensions.x + pixel.x, memory_order_relaxed, memory_order_relaxed)) {\n\t\t\t\t\t\tcontinue;"));
	CHECK(text.contains("reusable_path_clear_staging"));
	CHECK(text.contains("reusable_path_reduce"));
	CHECK(text.contains("static bool reusable_path_resolve_current_geometry"));
	CHECK(text.contains("reusable_path_resolve_current_geometry(candidate.secondary_geometry_instance_id"));
	CHECK(text.contains("sample_material(current_material, geometry_records, current_geometry_index, candidate.secondary_primitive_id, barycentric"));
	CHECK(text.contains("material_shading_normal(current_material, geometry_records, current_geometry_index, candidate.secondary_primitive_id, barycentric"));
	CHECK(text.contains("hybrid_intersect_split(selected_reconnect"));
	CHECK(text.contains("source_primary_proposal_solid_angle_pdf * source_cosine"));
	CHECK(text.contains("reusable_path_reconnection_visibility"));
	CHECK(text.contains("restir_gi_fresh_valid"));
	CHECK(text.contains("selected_target * (weight_sum / (float(represented_m) * selected_target_scalar))"));
	CHECK(text.contains("material_diagnostic.restir_gi_selected_reuse"));
	CHECK(text.contains("const bool staging_radiance_eligible = all(isfinite(incoming)) && staging_incident_luminance > 0.0f"));
	CHECK(text.contains("source_pdf > 0.0f && staging_radiance_eligible"));
	CHECK(text.contains("reusable_path_lighting_reevaluations"));
	CHECK(text.contains("reusable_path_environment_reevaluations"));
	CHECK(text.contains("if (realtime_path_reuse && sample == 0u"));
	CHECK(text.contains("if (realtime_path_reuse && restir_gi_fresh_valid)"));
	CHECK(text.contains("secondary_native.options = MTL::AccelerationStructureInstanceOptionOpaque"));
	CHECK(text.contains("if (instance.alpha_mode == Instance::ALPHA_OPAQUE) {\n\t\t\topaque_metal_instances.push_back(secondary_native);"));
	CHECK(text.contains("trace->setAccelerationStructure(p_work->opaque_tlas.get(), 17)"));
	CHECK(text.contains("Vector<MTL::AccelerationStructureUserIDInstanceDescriptor> alpha_metal_instances"));
	CHECK(text.contains("alpha_metal_instances.push_back(native)"));
	CHECK(text.contains("cache->alpha_tlas"));
	CHECK(text.contains("work->split_alpha_domains_exact"));
	CHECK(text.contains("raytracing::instance_acceleration_structure alpha_scene [[buffer(22)]]"));
	CHECK(text.contains("trace->setAccelerationStructure(p_work->alpha_tlas ? p_work->alpha_tlas.get() : p_work->tlas.get(), 22)"));
	CHECK(text.contains("static HybridIntersection hybrid_intersect_split"));
	CHECK(text.contains("alpha.distance < opaque.distance ? alpha : opaque"));
	CHECK(text.contains("atomic_uint split_opaque_queries[4]"));
	CHECK(text.contains("atomic_uint split_alpha_rejections[4]"));
	CHECK(text.contains("atomic_uint split_mixed_fallbacks[4]"));
	CHECK(text.contains("MATERIAL_DIAGNOSTIC_SPLIT_OPAQUE_QUERIES_PRIMARY"));
	CHECK(text.contains("MATERIAL_DIAGNOSTIC_SPLIT_MIXED_FALLBACK_GI"));
	CHECK(text.contains("static float3 thin_visibility_transmittance"));
	CHECK(text.contains("hybrid_intersect_split(ray, scene, opaque_scene, alpha_scene"));
	CHECK(text.contains("thin_visibility_transmittance(visibility_ray, scene, opaque_scene, alpha_scene"));
	CHECK(text.contains("thin_visibility_transmittance(visibility, scene, opaque_scene, alpha_scene"));
	CHECK(text.contains("hybrid_intersect_split(ao_ray, scene, opaque_scene, alpha_scene"));
	CHECK(text.contains("max(secondary_distance - 0.004f, 0.001f)"));
	CHECK(text.contains("max(current_distance - 0.004f, 0.001f)"));
	CHECK(text.contains("const float3 biased_to_secondary = candidate.secondary_world_position.xyz - reconnect_origin"));
	CHECK(text.contains("const float3 current_to_y = candidate.secondary_world_position.xyz - reconnect_origin"));
	CHECK(text.contains("raytracing::ray reconnect = { reconnect_origin, reconnect_direction"));
	CHECK(text.contains("raytracing::ray selected_reconnect = { selected_reconnect_origin, selected_reconnect_direction"));
	CHECK(text.contains("reusable_path_endpoint_blocked"));
	CHECK(text.contains("reusable_path_zero_target"));
	CHECK_FALSE(text.contains("const uint64_t revisions[4] = { r_result.admitted_geometry_generation, r_result.visibility_residency_generation, r_result.light_distribution_generation, r_result.environment_generation };\n\t\t\t\tbool revisions_match = cache->reusable_path_valid"));
	CHECK_FALSE(text.contains("atomic_fetch_add_explicit(&reusable_path"));
}

TEST_CASE("[Flux][Metal] planar delta caustics use bounded current-frame virtual-light transport") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	Ref<FileAccess> scheduler_source = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(scheduler_source.is_valid());
	const String scheduler_text = scheduler_source->get_as_text();
	CHECK(text.contains("struct BidirectionalCausticMirrorTriangleRecord"));
	CHECK(text.contains("mirror_factor_only"));
	CHECK(text.contains("instance.alpha_mode == Instance::ALPHA_OPAQUE"));
	CHECK(text.contains("!surface.dynamic"));
	CHECK(text.contains("instance.metallic >= 0.999f"));
	CHECK(text.contains("metal_bidirectional_caustic_mirrors.resize(caustic_mirror_capacity)"));
	CHECK(text.contains("FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_MIRROR"));
	CHECK(text.contains("FLUX_SAMPLE_DIMENSION_BIDIRECTIONAL_CAUSTIC_SOURCE"));
	CHECK(text.contains("sample_bidirectional_planar_caustic"));
	CHECK(text.contains("receiver_to_mirror_distance - 0.006f"));
	CHECK(text.contains("mirror_to_source_distance - 0.006f"));
	CHECK(text.contains("bidirectional_caustic_visibility_rays"));
	CHECK(text.contains("mirror_material.generation_low != mirror.material_generation_low"));
	CHECK(text.contains("const float proposal_pdf = source_area_pdf * mirror_selection_mass"));
	CHECK(text.contains("Ordinary NEE has zero\n// density"));
	CHECK(scheduler_text.contains("--flux-caustic-disabled"));
	CHECK(scheduler_text.contains("force_bidirectional_caustics_disabled"));
	CHECK_FALSE(text.contains("cached caustic visibility"));
}

TEST_CASE("[Flux][Metal] non-shadow transport uses exclusive five-queue GPU compaction") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("struct TraceCompactEntry"));
	CHECK(text.contains("uint pixel_index;"));
	CHECK(text.contains("uint need_mask;"));
	CHECK(text.contains("kernel void trace_classify"));
	CHECK(text.contains("kernel void trace_indirect_finalize"));
	CHECK(text.contains("TRACE_NEED_DIRECT = 1u"));
	CHECK(text.contains("TRACE_NEED_GI = 2u"));
	CHECK(text.contains("TRACE_NEED_REFLECTION = 4u"));
	CHECK(text.contains("TRACE_NEED_EXACT_ALPHA = 8u"));
	CHECK(text.contains("TRACE_NEED_COMPLEX = 16u"));
	CHECK(text.contains("(need_mask & TRACE_NEED_COMPLEX) != 0u ? 4u"));
	CHECK(text.contains("(need_mask & TRACE_NEED_EXACT_ALPHA) != 0u ? 3u"));
	CHECK(text.contains("(need_mask & TRACE_NEED_REFLECTION) != 0u ? 2u"));
	CHECK(text.contains("(need_mask & TRACE_NEED_GI) != 0u ? 1u : 0u"));
	CHECK(text.contains("pixel.y * parameters.dimensions.x + pixel.x"));
	CHECK(text.contains("entry.pixel_index % parameters.dimensions.x"));
	CHECK(text.contains("entry.pixel_index / parameters.dimensions.x"));
	CHECK(text.contains("if (compact_index >= logical_count) return;"));
	CHECK(text.contains("for (uint32_t queue_index = 0; queue_index < 5u; queue_index++)"));
	CHECK(text.contains("dispatchThreadgroups(p_work->trace_indirect_arguments[view].get()"));
	CHECK(text.contains("trace_compaction_fallback"));
	CHECK(text.contains("trace_compaction_view_active"));
	CHECK(text.contains("atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_direct, count"));
	CHECK(text.contains("atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_gi, count"));
	CHECK(text.contains("atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_reflection, count"));
	CHECK(text.contains("atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_exact_alpha, count"));
	CHECK(text.contains("atomic_fetch_add_explicit(&material_diagnostic.trace_compaction_dispatched_complex, count"));
	CHECK_FALSE(text.contains("atomic_store_explicit(&material_diagnostic.trace_compaction_dispatched_direct"));
	CHECK_FALSE(text.contains("atomic_store_explicit(&material_diagnostic.trace_compaction_dispatched_gi"));
	CHECK_FALSE(text.contains("atomic_store_explicit(&material_diagnostic.trace_compaction_dispatched_reflection"));
	CHECK_FALSE(text.contains("atomic_store_explicit(&material_diagnostic.trace_compaction_dispatched_exact_alpha"));
	CHECK_FALSE(text.contains("atomic_store_explicit(&material_diagnostic.trace_compaction_dispatched_complex"));
	CHECK(text.contains("ALPHA_RAY_CLASS_INDIRECT"));
	CHECK(text.contains("diffuse_moments_input [[texture(26)]]"));
	CHECK(text.contains("specular_moments_input [[texture(27)]]"));
	CHECK(text.contains("gi_converged_skips"));
	CHECK(text.contains("reflection_converged_skips"));
	CHECK_FALSE(text.contains("ALPHA_RAY_CLASS_SECONDARY"));
	CHECK(text.contains("pixel_count > uint64_t(UINT32_MAX) / 5u"));
	CHECK(text.contains("memset(compact_counts->contents(), 0, sizeof(uint32_t) * 5u)"));
	CHECK_FALSE(text.contains("trace_classify_pipeline;\n\tNS::SharedPtr<MTL::ComputePipelineState> trace_indirect_finalize_pipeline;\n\tNS::SharedPtr<MTL::Buffer> regir_cells"));
	Ref<FileAccess> validation = FileAccess::open("misc/path_tracing/m2_5/validation_project/validation.gd", FileAccess::READ);
	REQUIRE(validation.is_valid());
	const String validation_text = validation->get_as_text();
	CHECK(validation_text.contains("RenderingServer.viewport_get_flux_diagnostics(viewport_rid)"));
	CHECK(validation_text.contains("work_attribution_valid"));
	CHECK(validation_text.contains("trace_compaction"));
	CHECK(validation_text.contains("sum_enqueued_equals_active"));
	CHECK(validation_text.contains("sum_dispatched_equals_active"));
	CHECK(validation_text.contains("inactive_excluded_from_trace"));
	CHECK(validation_text.contains("FLUX_VIEWPORT_TOGGLE_VALIDATION=PASS"));
}

TEST_CASE("[Flux][Metal] stable scene packets reuse immutable material and geometry uploads") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("cached_material_records"));
	CHECK(text.contains("cached_geometry_records"));
	CHECK(text.contains("material_packet_matches"));
	CHECK(text.contains("geometry_packet_matches"));
	CHECK(text.contains("cache->cached_material_record_bytes.resize(material_packet_bytes)"));
	CHECK(text.contains("cache->cached_geometry_record_bytes.resize(geometry_packet_bytes)"));
}

TEST_CASE("[Flux][Metal] split reconstruction rejects identity and authored shading boundaries") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("NS::SharedPtr<MTL::Texture> primary_shading[2]"));
	CHECK(text.contains("texture2d<float, access::write> primary_shading_output [[texture(59)]]"));
	CHECK(text.contains("transport.primary_shading[ping]"));
	CHECK(text.contains("trace->setTexture(p_work->primary_shading_output[view], 59)"));
	CHECK(text.contains("texture2d<uint, access::read> current_identity [[texture(15)]]"));
	CHECK(text.contains("texture2d<float, access::read> current_shading [[texture(17)]]"));
	CHECK(text.contains("static bool split_shading_compatible"));
	CHECK(text.contains("all(current_identity == prior_identity)"));
	CHECK(text.contains("min(current_roughness, prior_roughness) < 0.20f"));
	CHECK(text.contains("split_surface_compatible(surface, old_surface, current_identity.read(pixel).xy"));
	CHECK(text.contains("split_spatial_filter(diffuse_signal, current_surface, current_identity, current_shading"));
}

TEST_CASE("[Flux][Metal] editor preview admission is local, bounded, and readiness-gated") {
	Ref<FileAccess> viewport = FileAccess::open("servers/rendering/renderer_viewport.cpp", FileAccess::READ);
	REQUIRE(viewport.is_valid());
	const String viewport_text = viewport->get_as_text();
	Ref<FileAccess> flux = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(flux.is_valid());
	const String flux_text = flux->get_as_text();
	CHECK(viewport_text.contains("viewport->render_info.flux_preview_blas_build_limit = 128"));
	CHECK(viewport_text.contains("vp->render_info.flux.ray_effects_active && vp->render_info.flux.residency_complete"));
	CHECK(viewport_text.contains("vp->render_info.flux_preview_blas_build_limit = 0"));
	CHECK(flux_text.contains("request.preview_admission_active = true"));
	CHECK(flux_text.contains("flux_preview_blas_triangle_limit"));
}

TEST_CASE("[Flux][Metal] MetalFX Temporal retains the Flux denoiser admission") {
	Ref<FileAccess> flux = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(flux.is_valid());
	const String flux_text = flux->get_as_text();
	CHECK(flux_text.contains("render_buffers->get_scaling_3d_mode() == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL"));
	CHECK(flux_text.contains("request.use_metalfx_denoiser = p_render_buffer_data->ensure_mfx_denoised"));
	CHECK(flux_text.contains("params.color = rb->get_internal_texture(v)"));
	CHECK(flux_text.contains("params.depth = rb->get_depth_texture(v)"));
	CHECK(flux_text.contains("params.motion = rb->get_velocity_buffer(false, v)"));
	CHECK(flux_text.contains("params.normal = rb_data->get_hybrid_guide_normal(v)"));
	CHECK(flux_text.contains("params.diffuse = rb_data->get_hybrid_guide_diffuse(v)"));
	CHECK(flux_text.contains("params.specular = rb_data->get_hybrid_guide_specular(v)"));
	CHECK(flux_text.contains("params.roughness = rb_data->get_hybrid_guide_roughness(v)"));
	CHECK(flux_text.contains("params.denoise_strength = rb_data->get_hybrid_guide_denoise_strength(v)"));
	CHECK(flux_text.contains("params.reactive = rb_data->get_hybrid_guide_reactive(v)"));
	CHECK(flux_text.contains("params.specular_distance = rb_data->get_hybrid_guide_specular_distance(v)"));
	CHECK(flux_text.contains("params.transparency = rb_data->get_hybrid_guide_transparency(v)"));
	CHECK(flux_text.contains("params.pre_exposure = 1.0f"));
	CHECK(flux_text.contains("p_render_buffer_data->advance_hybrid_history();"));
}

TEST_CASE("[Flux][Metal] MetalFX is the sole image-space denoiser") {
	Ref<FileAccess> source = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(source.is_valid());
	const String text = source->get_as_text();
	CHECK(text.contains("work->split_reconstruction_enabled = false"));
	CHECK(text.contains("work->transport_metadata_update_enabled = p_request.use_metalfx_denoiser && !p_request.shadow_only && !p_request.fresh_ray_oracle"));
	CHECK(text.contains("r_result.metalfx_denoiser = p_request.use_metalfx_denoiser && !p_request.shadow_only"));
	CHECK(text.contains("r_result.flux_image_reconstruction = work->split_reconstruction_enabled"));
	CHECK(text.contains("if (p_work->transport_metadata_update_enabled)"));
	CHECK(text.contains("const bool secondary_transport_metadata_valid = (parameters.reconstruction_flags & 8u) != 0u"));
	CHECK(text.contains("MetalFX is the only image-space denoiser in this mode"));
	CHECK(text.contains("No color history is read, blended, filtered, or emitted here"));
	CHECK(text.contains("diffuse_transport_sample[2]"));
	CHECK(text.contains("specular_transport_sample[2]"));
	CHECK(text.contains("const bool sample_surface_compatible = sample_reprojection_valid"));
	CHECK(text.contains("current_identity == prior_identity"));
	CHECK(text.contains("replay_diffuse_transport_sample = metalfx_image_denoiser"));
	CHECK(text.contains("replay_specular_transport_sample = metalfx_image_denoiser"));
	CHECK(text.contains("Trace owns the immutable raw path-sample outputs"));
	CHECK(text.contains("Validate the current raster-primary surface against the previous exact ray"));
	CHECK(text.contains("metalfx_transport_metadata"));
	CHECK(text.contains("const bool preserve_diffuse_history = !metalfx_image_denoiser"));
	CHECK(text.contains("effect_texture.write(float4(effective_diffuse_signal + effective_specular_signal, 1.0f), pixel)"));
}

TEST_CASE("[Flux][Metal] scene switches reset MetalFX history and static alpha is conservative") {
	Ref<FileAccess> flux = FileAccess::open("servers/rendering/renderer_rd/flux/render_flux.cpp", FileAccess::READ);
	REQUIRE(flux.is_valid());
	const String flux_text = flux->get_as_text();
	CHECK(flux_text.contains("update_hybrid_scene_history_key(scene_history_key)"));
	CHECK(flux_text.contains("request.history_valid = p_render_buffer_data->has_hybrid_history();"));
	CHECK(flux_text.contains("MetalFX active state is not transport history"));
	CHECK(flux_text.contains("get_base_data_format()"));
	CHECK(flux_text.contains("mfx_denoised_context_key == context_key"));
	CHECK(flux_text.contains("MetalFX Temporal Denoised Submission Begin"));
	CHECK(flux_text.contains("GPU duration remains explicitly unavailable"));

	Ref<FileAccess> effect = FileAccess::open("servers/rendering/renderer_rd/flux/metal_flux_effect.cpp", FileAccess::READ);
	REQUIRE(effect.is_valid());
	const String effect_text = effect->get_as_text();
	CHECK_FALSE(effect_text.contains("replay_direct_visibility"));
	CHECK_FALSE(effect_text.contains("direct_visibility_sample_input"));
}

TEST_CASE("[Flux][Editor] MetalFX comparison toggle is viewport-local") {
	Ref<FileAccess> editor = FileAccess::open("editor/scene/3d/node_3d_editor_plugin.cpp", FileAccess::READ);
	REQUIRE(editor.is_valid());
	const String editor_text = editor->get_as_text();
	CHECK(editor_text.contains("hybrid_preview_metalfx_button->set_text(TTRC(\"MetalFX\"))"));
	CHECK(editor_text.contains("scaling_mode = Viewport::SCALING_3D_MODE_BILINEAR"));
	CHECK(editor_text.contains("GLOBAL_GET(\"rendering/scaling_3d/mode\")"));
	CHECK(editor_text.contains("Toggle MetalFX for Flux editor viewports only"));
	CHECK_FALSE(editor_text.contains("set_scaling_3d_scale(1.0"));
}

} // namespace TestMetalFluxSource
