/**************************************************************************/
/*  test_flux_diagnostics.cpp                                             */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_flux_diagnostics)

#include "servers/rendering/rendering_server_types.h"

namespace TestFluxDiagnostics {

using RenderingServerTypes::FluxDiagnostics;

TEST_CASE("[RenderingServer][FluxDiagnostics] invalid snapshot has a stable complete schema") {
	const Dictionary diagnostics = FluxDiagnostics().to_dictionary();
	CHECK_FALSE(bool(diagnostics["valid"]));
	CHECK_EQ(int64_t(diagnostics["frame"]), 0);
	CHECK_EQ(int(diagnostics["effective_mode"]), 0);
	CHECK_FALSE(bool(diagnostics["ray_effects_active"]));
	CHECK_EQ(String(diagnostics["denoiser"]), "none");
	CHECK_FALSE(bool(diagnostics["flux_image_reconstruction"]));
	CHECK_FALSE(bool(diagnostics["environment_active"]));
	CHECK_EQ(String(diagnostics["environment_status"]), "disabled");
	CHECK_EQ(int(diagnostics["primary_surface_version"]), 0);
	CHECK_FALSE(bool(diagnostics["ray_owned_shading"]));
	CHECK_FALSE(bool(diagnostics["transport_complete"]));
	CHECK(diagnostics.has("viewport"));
	CHECK(diagnostics.has("reuse"));
	CHECK_FALSE(bool(diagnostics["residency_complete"]));
	const Dictionary viewport = diagnostics["viewport"];
	for (const char *key : { "internal_width", "internal_height", "target_width", "target_height", "scaling_3d_mode", "scaling_3d_scale", "preview_admission_active", "preview_admission_blas_build_limit", "preview_admission_blas_triangle_limit" }) CHECK(viewport.has(key));
	const Dictionary reuse = diagnostics["reuse"];
	for (const char *key : { "stbn_sampling_enabled", "restir_di_enabled", "regir_enabled", "reusable_path_enabled", "unified_finite_light_enabled" }) CHECK(reuse.has(key));

	const Dictionary admitted = diagnostics["admitted"];
	CHECK(admitted.has("geometry_count"));
	CHECK(admitted.has("surface_count"));
	CHECK(admitted.has("base_triangle_count"));
	CHECK(admitted.has("selected_triangle_count"));
	CHECK(admitted.has("canonical_material_count"));

	const Dictionary transport = diagnostics["transport"];
	CHECK(transport.has("retained_non_primary_geometry_count"));
	CHECK(transport.has("retains_non_primary_geometry"));

	const Dictionary materials = diagnostics["materials"];
	CHECK(materials.has("tier2"));
	CHECK(materials.has("capacity"));
	CHECK(materials.has("albedo"));
	CHECK(materials.has("normal"));
	CHECK(materials.has("orm"));
	CHECK(materials.has("emissive"));
	CHECK(materials.has("opacity"));
	CHECK(materials.has("alpha_occupancy"));
	CHECK_EQ(int(materials["supported_thin_transmission_count"]), 0);
	CHECK_EQ(int(materials["unsupported_transmission_texture_count"]), 0);
	CHECK_EQ(int(materials["unsupported_transmission_volume_count"]), 0);
	CHECK(diagnostics.has("work_attribution_valid"));
	CHECK_FALSE(bool(diagnostics["work_attribution_valid"]));
	const Dictionary trace_compaction = diagnostics["trace_compaction"];
	for (const char *key : { "active", "fallback", "active_pixel_count", "inactive_pixels", "direct_only", "gi", "reflection", "exact_alpha", "complex_light", "invariants" }) CHECK(trace_compaction.has(key));
	const Dictionary trace_compaction_invariants = trace_compaction["invariants"];
	for (const char *key : { "sum_enqueued_equals_active", "sum_dispatched_equals_active", "inactive_excluded_from_trace" }) CHECK(trace_compaction_invariants.has(key));
	for (const char *group : { "acceleration_structure", "alpha", "primary_analytic", "direct_reservoir", "emissive", "diffuse_cache", "regir", "reusable_path_cache", "restir_gi", "bidirectional_caustic", "punctual_lights" }) {
		CHECK(diagnostics.has(group));
	}
	CHECK(diagnostics.has("light_distribution_identity"));
	const Dictionary light_revisions = diagnostics["light_revisions"];
	for (const char *key : { "requested", "submitted", "completed" }) CHECK(light_revisions.has(key));
	CHECK(diagnostics.has("transport_revisions"));
	const Dictionary transport_revisions = diagnostics["transport_revisions"];
	for (const char *key : { "valid", "admitted_geometry_generation", "visibility_residency_generation", "residency_completion_token", "residency_complete", "light_distribution_generation", "environment_generation" }) {
		CHECK(transport_revisions.has(key));
	}
	const Dictionary history_validity = transport_revisions["history_validity"];
	for (const char *key : { "camera_or_screen", "residency_ready", "geometry", "light_distribution", "environment", "reconstruction_reset" }) CHECK(history_validity.has(key));
	const Dictionary ray_work = diagnostics["ray_work"];
	for (const char *key : { "gi_fresh_ray_count", "gi_converged_skip_count", "reflection_ray_count", "reflection_converged_skip_count" }) CHECK(ray_work.has(key));
	const Dictionary cpu_phases = diagnostics["cpu_phases_ms"];
	for (const char *key : { "scene_preparation", "residency_planning", "residency_plan_cached", "residency_plan_full", "metal_preparation", "submission" }) CHECK(cpu_phases.has(key));
	const Dictionary residency_plan = diagnostics["residency_plan"];
	for (const char *key : { "cache_hit_count", "cache_miss_count", "rebuild_count" }) CHECK(residency_plan.has(key));
	const Dictionary submission_completion = diagnostics["submission_completion"];
	for (const char *key : { "submitted_frame", "observed_frame", "completion_frame_age", "pending_capture_count", "ray_shadow_timing_available", "residency_complete" }) {
		CHECK(submission_completion.has(key));
	}
	CHECK_EQ(int64_t(submission_completion["submitted_frame"]), 0);
	CHECK_EQ(int64_t(submission_completion["observed_frame"]), 0);
	CHECK_EQ(int64_t(submission_completion["completion_frame_age"]), 0);
	CHECK_EQ(int(submission_completion["pending_capture_count"]), 0);
	CHECK_FALSE(bool(submission_completion["ray_shadow_timing_available"]));
	CHECK_FALSE(bool(submission_completion["residency_complete"]));
	const Dictionary alpha = diagnostics["alpha"];
	for (const char *ray_class : { "primary", "visibility", "reflection", "indirect" }) {
		const Dictionary ray_counts = alpha[ray_class];
		CHECK(ray_counts.has("candidate_count"));
		CHECK(ray_counts.has("rejection_count"));
		CHECK_EQ(int(ray_counts["candidate_count"]), 0);
		CHECK_EQ(int(ray_counts["rejection_count"]), 0);
	}
	CHECK_FALSE(bool(diagnostics["timings_valid"]));
	CHECK_EQ(int64_t(diagnostics["timings_frame"]), 0);
	const Dictionary regir = diagnostics["regir"];
	for (const char *key : { "enabled", "reason", "valid", "complete", "cell_count", "bytes" }) CHECK(regir.has(key));
	CHECK_FALSE(bool(regir["enabled"]));
	CHECK_EQ(String(regir["reason"]), "single_reservoir_cell_correlation_unvalidated");
	const Dictionary reusable_path_cache = diagnostics["reusable_path_cache"];
	for (const char *key : { "enabled", "valid", "complete", "cell_count", "occupied_cell_count", "occupancy_valid", "bytes", "staged_count", "update_count", "query_count", "valid_candidate_count", "reused_candidate_count", "rejection_count", "reevaluation_count", "reconnection_visibility_count", "lighting_reevaluation_count", "environment_reevaluation_count" }) CHECK(reusable_path_cache.has(key));
	CHECK_FALSE(bool(reusable_path_cache["enabled"]));
	const Dictionary restir_gi = diagnostics["restir_gi"];
	for (const char *key : { "valid", "complete", "backend_prototype", "current_candidate_count", "reused_candidate_count", "selected_reuse_count" }) CHECK(restir_gi.has(key));
	CHECK_FALSE(bool(restir_gi["complete"]));
	const Dictionary bidirectional_caustic = diagnostics["bidirectional_caustic"];
	for (const char *key : { "enabled", "active", "complete", "backend_prototype", "mirror_triangle_count", "mirror_triangle_capacity", "mirror_triangle_overflow", "source_triangle_count", "candidate_count", "valid_count", "contributed_count", "visibility_ray_count", "rejection_count", "nonfinite_or_pdf_failure_count" }) CHECK(bidirectional_caustic.has(key));
	CHECK_FALSE(bool(bidirectional_caustic["active"]));
}

TEST_CASE("[RenderingServer][FluxDiagnostics] exact mapping and conservative retention derivation") {
	FluxDiagnostics source;
	source.reset_for_frame(91, 2);
	source.ray_effects_active = true;
	source.denoiser = "metalfx";
	source.flux_image_reconstruction = false;
	source.environment_active = true;
	source.environment_status = "active";
	source.primary_surface_version = 1;
	source.ray_owned_shading = true;
	source.primary_surface_view_count = 2;
	source.transport_complete = true;
	source.residency_complete = true;
	source.viewport_internal_width = 1088;
	source.viewport_internal_height = 612;
	source.viewport_target_width = 1280;
	source.viewport_target_height = 720;
	source.viewport_scaling_3d_mode = 3;
	source.viewport_scaling_3d_scale = 0.85f;
	source.preview_admission_active = true;
	source.preview_admission_blas_build_limit = 128;
	source.preview_admission_blas_triangle_limit = 10000000;
	source.stbn_sampling_enabled = true;
	source.restir_di_enabled = true;
	source.regir_reuse_enabled = true;
	source.regir_reuse_reason = "explicit_validation_override";
	source.reusable_path_reuse_enabled = true;
	source.unified_finite_light_reuse_enabled = true;
	source.transport_incomplete_reason = "complete";
	source.admitted_geometry_count = 12;
	source.admitted_surface_count = 9;
	source.admitted_base_triangle_count = 45000;
	source.admitted_selected_triangle_count = 23146;
	source.admitted_canonical_material_count = 16;
	source.transport_state = "bounded";
	source.transport_reason = "active";
	source.transport_max_distance = 150.0f;
	source.set_transport_counts(3, 8, 10);
	source.transport_selected_light_count = 4;
	source.transport_eligible_light_count = 7;
	source.material_tier2 = true;
	source.material_capacity = 2048;
	source.material_albedo = { 4, 4, 0 };
	source.material_normal = { 5, 4, 1 };
	source.material_orm = { 3, 3, 0 };
	source.material_emissive = { 2, 2, 0 };
	source.material_opacity = { 1, 1, 0 };
	source.material_alpha_occupancy = { 1, 1, 0 };
	source.work_attribution_valid = true;
	source.trace_compaction_active = true;
	source.trace_compaction_active_pixel_count = 7;
	source.trace_compaction_inactive_pixel_count = 3;
	source.trace_compaction_need_counts[0] = 7;
	source.trace_compaction_need_counts[1] = 4;
	source.trace_compaction_enqueued_counts[0] = 7;
	source.trace_compaction_dispatched_counts[0] = 7;
	source.acceleration_structure_deferred_build_count = 2;
	source.acceleration_structure_deferred_triangle_count = 9000000000ULL;
	source.alpha_candidate_count = 3;
	source.alpha_primary = { 5, 7 };
	source.alpha_split_opaque_query_counts[1] = 19;
	source.alpha_split_alpha_hit_counts[1] = 23;
	source.alpha_split_alpha_rejection_counts[1] = 29;
	source.alpha_split_mixed_fallback_counts[1] = 31;
	source.alpha_occupancy_mixed_sample_count = 11;
	source.primary_analytic_selected_count = 13;
	source.primary_analytic_contributed_count = 17;
	source.primary_analytic_visibility_test_count = 19;
	source.direct_reservoir_candidate_count = 23;
	source.emissive_triangle_count = 29;
	source.emissive_triangle_capacity = 31;
	source.emissive_triangle_overflow = 37;
	source.diffuse_cache_bytes = 8000000000ULL;
	source.reusable_path_cache_enabled = true;
	source.reusable_path_cache_valid = true;
	source.reusable_path_cache_complete = true;
	source.reusable_path_cache_cell_count = 32;
	source.reusable_path_cache_bytes = 30720;
	source.reusable_path_cache_query_count = 27;
	source.reusable_path_cache_invalid_record_count = 37;
	source.reusable_path_cache_endpoint_blocked_count = 41;
	source.reusable_path_cache_shading_invalid_count = 43;
	source.reusable_path_cache_zero_target_count = 47;
	source.reusable_path_cache_invalid_weight_count = 53;
	source.reusable_path_cache_considered_count = 59;
	source.reusable_path_cache_accepted_count = 61;
	source.reusable_path_cache_selected_count = 67;
	source.reusable_path_cache_lighting_reevaluation_count = 67;
	source.reusable_path_cache_environment_reevaluation_count = 71;
	source.restir_gi_valid = true;
	source.restir_gi_complete = true;
	source.restir_gi_backend_prototype = true;
	source.restir_gi_current_candidate_count = 53;
	source.restir_gi_reused_candidate_count = 59;
	source.restir_gi_selected_reuse_count = 61;
	source.punctual_light_count = 41;
	source.punctual_light_overflow_count = 43;
	source.unsupported_punctual_light_count = 47;
	source.light_distribution_identity = 123456789012345ULL;
	source.light_revision_requested = 13;
	source.light_revision_submitted = 13;
	source.light_revision_completed = 13;
	source.admitted_geometry_generation = 11;
	source.visibility_residency_generation = 12;
	source.light_distribution_generation = 13;
	source.environment_generation = 14;
	source.residency_completion_token = 19;
	source.residency_complete = true;
	source.gi_converged_skip_count = 73;
	source.reflection_converged_skip_count = 79;
	source.cpu_scene_preparation_milliseconds = 1.25;
	source.cpu_residency_planning_milliseconds = 2.5;
	source.cpu_residency_plan_cached_milliseconds = 0.25;
	source.cpu_residency_plan_full_milliseconds = 2.25;
	source.residency_plan_cache_hit_count = 73;
	source.residency_plan_cache_miss_count = 79;
	source.residency_plan_cache_rebuild_count = 83;
	source.cpu_metal_preparation_milliseconds = 3.75;
	source.cpu_submission_milliseconds = 5.0;
	source.transport_revisions_valid = true;
	source.transport_history_invalid_reasons = 8u | 16u;
	source.raw_shadow_timing_available = true;
	source.set_completion_observation(96, 3);
	source.refresh_shadow_residency_completeness();

	const Dictionary diagnostics = source.to_dictionary();
	CHECK(bool(diagnostics["valid"]));
	CHECK_EQ(int64_t(diagnostics["frame"]), 91);
	CHECK_EQ(int(diagnostics["effective_mode"]), 2);
	CHECK_EQ(int(diagnostics["primary_surface_version"]), 1);
	CHECK(bool(diagnostics["ray_owned_shading"]));
	CHECK_EQ(String(diagnostics["denoiser"]), "metalfx");
	CHECK_FALSE(bool(diagnostics["flux_image_reconstruction"]));
	CHECK(bool(diagnostics["transport_complete"]));
	CHECK(bool(diagnostics["residency_complete"]));
	const Dictionary viewport = diagnostics["viewport"];
	CHECK_EQ(int(viewport["internal_width"]), 1088);
	CHECK_EQ(int(viewport["target_height"]), 720);
	CHECK_EQ(int(viewport["scaling_3d_mode"]), 3);
	CHECK(bool(viewport["preview_admission_active"]));
	CHECK_EQ(int(viewport["preview_admission_blas_build_limit"]), 128);
	CHECK_EQ(int64_t(viewport["preview_admission_blas_triangle_limit"]), 10000000);
	const Dictionary reuse = diagnostics["reuse"];
	CHECK(bool(reuse["stbn_sampling_enabled"]));
	CHECK(bool(reuse["restir_di_enabled"]));
	CHECK(bool(reuse["regir_enabled"]));
	CHECK_EQ(String(reuse["regir_reason"]), "explicit_validation_override");
	CHECK(bool(reuse["reusable_path_enabled"]));
	CHECK(bool(reuse["unified_finite_light_enabled"]));
	const Dictionary admitted = diagnostics["admitted"];
	CHECK_EQ(int64_t(admitted["selected_triangle_count"]), 23146);
	CHECK_EQ(int(admitted["canonical_material_count"]), 16);
	const Dictionary transport = diagnostics["transport"];
	CHECK_EQ(int(transport["retained_non_primary_geometry_count"]), 5);
	CHECK(bool(transport["retains_non_primary_geometry"]));
	const Dictionary materials = diagnostics["materials"];
	const Dictionary normal = materials["normal"];
	CHECK_EQ(int(normal["requested"]), 5);
	CHECK_EQ(int(normal["resident"]), 4);
	CHECK_EQ(int(normal["misses"]), 1);
	CHECK(bool(diagnostics["work_attribution_valid"]));
	const Dictionary trace_compaction = diagnostics["trace_compaction"];
	CHECK(bool(trace_compaction["active"]));
	CHECK_EQ(int(trace_compaction["active_pixel_count"]), 7);
	CHECK_EQ(int(trace_compaction["inactive_pixels"]), 3);
	const Dictionary direct_only = trace_compaction["direct_only"];
	CHECK_EQ(int(direct_only["need_count"]), 7);
	CHECK_EQ(int(direct_only["enqueued_count"]), 7);
	CHECK_EQ(int(direct_only["dispatched_count"]), 7);
	const Dictionary compaction_invariants = trace_compaction["invariants"];
	CHECK(bool(compaction_invariants["sum_enqueued_equals_active"]));
	CHECK(bool(compaction_invariants["sum_dispatched_equals_active"]));
	const Dictionary acceleration_structure = diagnostics["acceleration_structure"];
	CHECK_EQ(int(acceleration_structure["deferred_build_count"]), 2);
	CHECK_EQ(int64_t(acceleration_structure["deferred_triangle_count"]), 9000000000LL);
	const Dictionary alpha = diagnostics["alpha"];
	CHECK_EQ(int(alpha["candidate_count"]), 3);
	const Dictionary alpha_primary = alpha["primary"];
	CHECK_EQ(int(alpha_primary["candidate_count"]), 5);
	CHECK_EQ(int(alpha_primary["rejection_count"]), 7);
	const Dictionary alpha_split_domains = alpha["split_domains"];
	const Dictionary alpha_visibility_split = alpha_split_domains["visibility"];
	CHECK_EQ(int(alpha_visibility_split["opaque_query_count"]), 19);
	CHECK_EQ(int(alpha_visibility_split["alpha_hit_count"]), 23);
	CHECK_EQ(int(alpha_visibility_split["alpha_rejection_count"]), 29);
	CHECK_EQ(int(alpha_visibility_split["mixed_fallback_count"]), 31);
	const Dictionary primary_analytic = diagnostics["primary_analytic"];
	CHECK_EQ(int(primary_analytic["selected_count"]), 13);
	CHECK_EQ(int(primary_analytic["contributed_count"]), 17);
	CHECK_EQ(int(primary_analytic["visibility_test_count"]), 19);
	const Dictionary emissive = diagnostics["emissive"];
	CHECK_EQ(int(emissive["triangle_overflow"]), 37);
	const Dictionary diffuse_cache = diagnostics["diffuse_cache"];
	CHECK_EQ(int64_t(diffuse_cache["bytes"]), 8000000000LL);
	const Dictionary reusable_path_cache = diagnostics["reusable_path_cache"];
	CHECK(bool(reusable_path_cache["enabled"]));
	CHECK_EQ(int(reusable_path_cache["cell_count"]), 32);
	CHECK_EQ(int64_t(reusable_path_cache["bytes"]), 30720LL);
	CHECK_EQ(int(reusable_path_cache["query_count"]), 27);
	const Dictionary reusable_path_post_validation = reusable_path_cache["post_validation"];
	CHECK_EQ(int(reusable_path_post_validation["invalid_record_count"]), 37);
	CHECK_EQ(int(reusable_path_post_validation["endpoint_blocked_count"]), 41);
	CHECK_EQ(int(reusable_path_post_validation["shading_invalid_count"]), 43);
	CHECK_EQ(int(reusable_path_post_validation["zero_target_count"]), 47);
	CHECK_EQ(int(reusable_path_post_validation["invalid_weight_count"]), 53);
	CHECK_EQ(int(reusable_path_post_validation["considered_count"]), 59);
	CHECK_EQ(int(reusable_path_post_validation["accepted_count"]), 61);
	CHECK_EQ(int(reusable_path_post_validation["selected_count"]), 67);
	CHECK_EQ(int(reusable_path_cache["lighting_reevaluation_count"]), 67);
	CHECK_EQ(int(reusable_path_cache["environment_reevaluation_count"]), 71);
	const Dictionary restir_gi = diagnostics["restir_gi"];
	CHECK(bool(restir_gi["valid"]));
	CHECK(bool(restir_gi["complete"]));
	CHECK(bool(restir_gi["backend_prototype"]));
	CHECK_EQ(int(restir_gi["current_candidate_count"]), 53);
	CHECK_EQ(int(restir_gi["reused_candidate_count"]), 59);
	CHECK_EQ(int(restir_gi["selected_reuse_count"]), 61);
	const Dictionary punctual_lights = diagnostics["punctual_lights"];
	CHECK_EQ(int(punctual_lights["unsupported_count"]), 47);
	CHECK_EQ(int64_t(diagnostics["light_distribution_identity"]), 123456789012345LL);
	const Dictionary light_revisions = diagnostics["light_revisions"];
	CHECK_EQ(int64_t(light_revisions["requested"]), 13);
	CHECK_EQ(int64_t(light_revisions["submitted"]), 13);
	CHECK_EQ(int64_t(light_revisions["completed"]), 13);
	const Dictionary transport_revisions = diagnostics["transport_revisions"];
	CHECK(bool(transport_revisions["valid"]));
	CHECK_EQ(int64_t(transport_revisions["admitted_geometry_generation"]), 11);
	CHECK_EQ(int64_t(transport_revisions["visibility_residency_generation"]), 12);
	CHECK_EQ(int64_t(transport_revisions["residency_completion_token"]), 19);
	CHECK(bool(transport_revisions["residency_complete"]));
	CHECK_EQ(int64_t(transport_revisions["light_distribution_generation"]), 13);
	CHECK_EQ(int64_t(transport_revisions["environment_generation"]), 14);
	const Dictionary history_validity = transport_revisions["history_validity"];
	CHECK(bool(history_validity["camera_or_screen"]));
	CHECK_FALSE(bool(history_validity["light_distribution"]));
	CHECK_FALSE(bool(history_validity["environment"]));
	const Dictionary ray_work = diagnostics["ray_work"];
	CHECK_EQ(int(ray_work["gi_converged_skip_count"]), 73);
	CHECK_EQ(int(ray_work["reflection_converged_skip_count"]), 79);
	const Dictionary cpu_phases = diagnostics["cpu_phases_ms"];
	CHECK_EQ(double(cpu_phases["scene_preparation"]), 1.25);
	CHECK_EQ(double(cpu_phases["residency_planning"]), 2.5);
	CHECK_EQ(double(cpu_phases["residency_plan_cached"]), 0.25);
	CHECK_EQ(double(cpu_phases["residency_plan_full"]), 2.25);
	CHECK_EQ(double(cpu_phases["metal_preparation"]), 3.75);
	CHECK_EQ(double(cpu_phases["submission"]), 5.0);
	const Dictionary residency_plan = diagnostics["residency_plan"];
	CHECK_EQ(int64_t(residency_plan["cache_hit_count"]), 73);
	CHECK_EQ(int64_t(residency_plan["cache_miss_count"]), 79);
	CHECK_EQ(int64_t(residency_plan["rebuild_count"]), 83);
	const Dictionary submission_completion = diagnostics["submission_completion"];
	CHECK_EQ(int64_t(submission_completion["submitted_frame"]), 91);
	CHECK_EQ(int64_t(submission_completion["observed_frame"]), 96);
	CHECK_EQ(int64_t(submission_completion["completion_frame_age"]), 5);
	CHECK_EQ(int(submission_completion["pending_capture_count"]), 3);
	CHECK(bool(submission_completion["ray_shadow_timing_available"]));
	CHECK_FALSE(bool(submission_completion["residency_complete"]));
}

TEST_CASE("[RenderingServer][FluxDiagnostics] completion age and shadow residency stay conservative") {
	CHECK_EQ(FluxDiagnostics::saturating_frame_age(40, 39), 0);
	CHECK_EQ(FluxDiagnostics::saturating_frame_age(40, 40), 0);
	CHECK_EQ(FluxDiagnostics::saturating_frame_age(40, 47), 7);

	FluxDiagnostics diagnostics;
	diagnostics.reset_for_frame(40, 2);
	diagnostics.set_completion_observation(39, 4);
	CHECK_EQ(diagnostics.observed_frame, 39);
	CHECK_EQ(diagnostics.completion_frame_age, 0);
	CHECK_EQ(diagnostics.pending_capture_count, 4);

	diagnostics.transport_complete = true;
	diagnostics.refresh_shadow_residency_completeness();
	CHECK(diagnostics.shadow_residency_complete);
	diagnostics.acceleration_structure_deferred_triangle_count = 1;
	diagnostics.refresh_shadow_residency_completeness();
	CHECK_FALSE(diagnostics.shadow_residency_complete);
	diagnostics.acceleration_structure_deferred_triangle_count = 0;
	diagnostics.transport_complete = false;
	diagnostics.refresh_shadow_residency_completeness();
	CHECK_FALSE(diagnostics.shadow_residency_complete);
}

TEST_CASE("[RenderingServer][FluxDiagnostics] work-only snapshots retain and identify the latest timing sample") {
	FluxDiagnostics timed_snapshot;
	timed_snapshot.reset_for_frame(41, 2);
	timed_snapshot.timings_valid = true;
	timed_snapshot.timings_frame = 41;
	timed_snapshot.timings_ms = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 };

	FluxDiagnostics work_only_snapshot;
	work_only_snapshot.reset_for_frame(47, 2);
	work_only_snapshot.carry_forward_timings_from(timed_snapshot);
	CHECK(work_only_snapshot.timings_valid);
	CHECK_EQ(work_only_snapshot.frame, 47);
	CHECK_EQ(work_only_snapshot.timings_frame, 41);
	CHECK_EQ(work_only_snapshot.timings_ms.ray_effects, 4.0);

	FluxDiagnostics replacement_timing_snapshot;
	replacement_timing_snapshot.reset_for_frame(53, 2);
	replacement_timing_snapshot.timings_valid = true;
	replacement_timing_snapshot.timings_frame = 53;
	replacement_timing_snapshot.timings_ms.ray_effects = 9.0;
	replacement_timing_snapshot.carry_forward_timings_from(timed_snapshot);
	CHECK_EQ(replacement_timing_snapshot.timings_frame, 53);
	CHECK_EQ(replacement_timing_snapshot.timings_ms.ray_effects, 9.0);
}

TEST_CASE("[RenderingServer][FluxDiagnostics] pending completion waits for every expected asynchronous capture") {
	RenderingServerTypes::FluxDiagnosticsPendingState pending;
	pending.timing_expected = true;
	pending.work_attribution_expected = true;
	CHECK_FALSE(pending.ready_to_publish());
	pending.timing_ready = true;
	CHECK_FALSE(pending.ready_to_publish());
	pending.work_attribution_ready = true;
	CHECK(pending.ready_to_publish());
	pending = {};
	pending.work_attribution_expected = true;
	pending.work_attribution_ready = true;
	CHECK(pending.ready_to_publish());
}

TEST_CASE("[RenderingServer][FluxDiagnostics] timing capture admission is independent per owner") {
	HashMap<uint64_t, uint8_t> submitted_captures;
	auto admit_capture = [&submitted_captures](uint64_t p_owner, bool p_shadow_capture) {
		const uint8_t capture_bit = p_shadow_capture ? 2u : 1u;
		uint8_t *captures = submitted_captures.getptr(p_owner);
		if (captures && (*captures & capture_bit) != 0) {
			return false;
		}
		if (!captures) {
			submitted_captures.insert(p_owner, capture_bit);
		} else {
			*captures |= capture_bit;
		}
		return true;
	};

	CHECK(admit_capture(101, false));
	CHECK_FALSE(admit_capture(101, false));
	CHECK(admit_capture(202, false));
	CHECK(admit_capture(101, true));
	CHECK_FALSE(admit_capture(101, true));
}

TEST_CASE("[RenderingServer][FluxDiagnostics] frame-local invalid samples only downgrade complete transport") {
	FluxDiagnostics diagnostics;
	diagnostics.transport_complete = true;
	diagnostics.transport_incomplete_reason = "complete";
	diagnostics.invalid_pdf_sample_count = 1;
	diagnostics.apply_work_attribution_transport_validation();
	CHECK_FALSE(diagnostics.transport_complete);
	CHECK_EQ(diagnostics.transport_incomplete_reason, "invalid transport sample rejected");

	diagnostics.transport_complete = false;
	diagnostics.transport_incomplete_reason = "acceleration-structure admission deferred; visibility fails open";
	diagnostics.nonfinite_lobe_sample_count = 1;
	diagnostics.apply_work_attribution_transport_validation();
	CHECK_EQ(diagnostics.transport_incomplete_reason, "acceleration-structure admission deferred; visibility fails open");
}

TEST_CASE("[RenderingServer][FluxDiagnostics] raster reset clears active ray state and counters") {
	FluxDiagnostics diagnostics;
	diagnostics.reset_for_frame(4, 1);
	diagnostics.ray_effects_active = true;
	diagnostics.environment_active = true;
	diagnostics.admitted_geometry_count = 9;
	diagnostics.material_normal = { 3, 2, 1 };
	diagnostics.work_attribution_valid = true;
	diagnostics.alpha_candidate_count = 3;
	diagnostics.light_distribution_identity = 7;
	diagnostics.raw_shadow_timing_available = true;
	diagnostics.set_completion_observation(8, 2);
	diagnostics.timings_valid = true;
	diagnostics.timings_frame = 4;
	diagnostics.timings_ms.ray_effects = 8.0;

	diagnostics.reset_for_frame(5, 0);
	CHECK(diagnostics.valid);
	CHECK_EQ(diagnostics.effective_mode, 0);
	CHECK_FALSE(diagnostics.ray_effects_active);
	CHECK_FALSE(diagnostics.environment_active);
	CHECK_EQ(diagnostics.admitted_geometry_count, 0);
	CHECK_EQ(diagnostics.material_normal.requested, 0);
	CHECK_FALSE(diagnostics.work_attribution_valid);
	CHECK_EQ(diagnostics.alpha_candidate_count, 0);
	CHECK_EQ(diagnostics.light_distribution_identity, 0);
	CHECK_FALSE(diagnostics.raw_shadow_timing_available);
	CHECK_EQ(diagnostics.observed_frame, 0);
	CHECK_EQ(diagnostics.pending_capture_count, 0);
	CHECK_FALSE(diagnostics.timings_valid);
	CHECK_EQ(diagnostics.timings_frame, 0);
}

TEST_CASE("[RenderingServer][FluxDiagnostics] timing dictionary exposes every stable stage key") {
	FluxDiagnostics source;
	source.timings_valid = true;
	source.timings_ms = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 };
	const Dictionary diagnostics = source.to_dictionary();
	CHECK(bool(diagnostics["timings_valid"]));
	const Dictionary timings = diagnostics["timings_ms"];
	CHECK_EQ(double(timings["blas"]), 1.0);
	CHECK_EQ(double(timings["tlas"]), 2.0);
	CHECK_EQ(double(timings["ray_shadows"]), 3.0);
	CHECK_EQ(double(timings["ray_effects"]), 4.0);
	CHECK_EQ(double(timings["spatial"]), 5.0);
	CHECK_EQ(double(timings["temporal"]), 6.0);
	CHECK_EQ(double(timings["composition"]), 7.0);
}

} // namespace TestFluxDiagnostics
