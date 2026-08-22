#!/usr/bin/env bash
# Validates source-level wiring for the optional macOS MetalFX temporal-denoised
# runtime path. This intentionally does not claim a Windows/Vulkan equivalent.
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
forward_h="$root/servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
forward_cpp="$root/servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp"
hybrid_h="$root/servers/rendering/renderer_rd/effects/metal_hybrid_effect.h"
hybrid_cpp="$root/servers/rendering/renderer_rd/effects/metal_hybrid_effect.cpp"
sky_cpp="$root/servers/rendering/renderer_rd/environment/sky.cpp"
editor_viewport_cpp="$root/editor/scene/3d/node_3d_editor_viewport.cpp"
editor_plugin_cpp="$root/editor/scene/3d/node_3d_editor_plugin.cpp"
editor_validation="$root/misc/path_tracing/m2_5/validation_project/validation.gd"
renderer_viewport_cpp="$root/servers/rendering/renderer_viewport.cpp"
scene_cull_cpp="$root/servers/rendering/renderer_scene_cull.cpp"
scene_cull_h="$root/servers/rendering/renderer_scene_cull.h"
scene_render_rd_h="$root/servers/rendering/renderer_rd/renderer_scene_render_rd.h"
render_data_rd_h="$root/servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
transport_culling_h="$root/servers/rendering/path_tracing/transport_culling.h"
transport_culling_cpp="$root/servers/rendering/path_tracing/transport_culling.cpp"
transport_culling_test="$root/tests/servers/rendering/test_transport_culling.cpp"

require() {
	local pattern="$1"
	local file="$2"
	if ! rg -Fq -- "$pattern" "$file"; then
		echo "missing required MetalFX denoised runtime wiring: $pattern ($file)" >&2
		exit 1
	fi
}

# The denoised scaler must receive real, separate semantic guides. A packed
# normal/roughness target or an effect-history texture is not a substitute.
for guide in NORMAL DIFFUSE SPECULAR ROUGHNESS DENOISE_STRENGTH REACTIVE SPECULAR_DISTANCE TRANSPARENCY; do
	require "RB_TEX_HYBRID_GUIDE_${guide}" "$forward_h"
done
require "ensure_mfx_denoised" "$forward_h"
require "get_mfx_denoised_context" "$forward_h"
require "mfx_denoised_effect" "$forward_h"

# Each request must carry all guides and explicitly ask for the denoised
# adapter; the native custom temporal history must be bypassed on that path.
require "use_metalfx_denoiser" "$hybrid_h"
for guide in normal diffuse specular roughness denoise_strength reactive specular_distance transparency; do
	require "guide_${guide}" "$hybrid_h"
	require "guide_${guide}" "$forward_cpp"
	require "guide_${guide}.write" "$hybrid_cpp"
done
require "use_metalfx_denoiser" "$hybrid_cpp"
require "temporal_enabled" "$hybrid_cpp"
require "all_temporal && !work->metalfx_denoiser" "$hybrid_cpp"
require "if (!p_work->metalfx_denoiser)" "$hybrid_cpp"

# Reflection guides must describe the sampled reflection, not merely the
# mirror material's normal-incidence F0. Primary-surface replacement reuses
# the already traced hit without issuing an additional ray query.
require "float3 reflection_guide_normal = world_normal;" "$hybrid_cpp"
require "float3 reflection_guide_diffuse = primary_diffuse;" "$hybrid_cpp"
require "float3 reflection_guide_f0 = primary_f0;" "$hybrid_cpp"
require "float reflection_guide_roughness = roughness;" "$hybrid_cpp"
require "reflection_guide_normal = hit_normal;" "$hybrid_cpp"
require "reflection_guide_diffuse = hit_diffuse;" "$hybrid_cpp"
require "float3 hit_albedo = material_albedo(material" "$hybrid_cpp"
require "reflection_guide_f0 = mix(float3(0.04f), hit_albedo, material.albedo_metallic.a);" "$hybrid_cpp"
require "reflection_guide_roughness = material.emission_roughness.a;" "$hybrid_cpp"
require "float3 specular_albedo = reflection_guide_f0 +" "$hybrid_cpp"
require "guide_specular.write(float4(clamp(specular_albedo" "$hybrid_cpp"
require "guide_denoise_strength.write(float4(0.0f), pixel);" "$hybrid_cpp"
require "guide_reactive.write(float4(0.0f), pixel);" "$hybrid_cpp"
if rg -Fq -- "guide_specular.write(float4(clamp(primary_f0" "$hybrid_cpp"; then
	echo "MetalFX specular guide must use view-angle Fresnel, not raw F0." >&2
	exit 1
fi
if rg -Fq -- "guide_denoise_strength.write(float4(reflection_valid" "$hybrid_cpp" || rg -Fq -- "guide_reactive.write(float4(reflection_valid" "$hybrid_cpp"; then
	echo "Stable traced reflections must remain eligible for denoising and temporal history." >&2
	exit 1
fi

# MetalFX denoised reconstruction is selected before ordinary MetalFX Temporal
# under SCALE_MFX, but ordinary temporal remains the capability fallback.
require "SCALE_MFX" "$forward_cpp"
require "mfx_denoised_effect" "$forward_cpp"
require "mfx_temporal_effect" "$forward_cpp"
require "get_mfx_denoised_context" "$forward_cpp"
require "mfx_denoised_effect->process" "$forward_cpp"
require "params.normal = rb_data->get_hybrid_guide_normal" "$forward_cpp"
require "params.diffuse = rb_data->get_hybrid_guide_diffuse" "$forward_cpp"
require "params.specular = rb_data->get_hybrid_guide_specular" "$forward_cpp"
require "params.roughness = rb_data->get_hybrid_guide_roughness" "$forward_cpp"
require "params.denoise_strength = rb_data->get_hybrid_guide_denoise_strength" "$forward_cpp"
require "params.reactive = rb_data->get_hybrid_guide_reactive" "$forward_cpp"
require "params.specular_distance = rb_data->get_hybrid_guide_specular_distance" "$forward_cpp"
require "params.transparency = rb_data->get_hybrid_guide_transparency" "$forward_cpp"

# Denoised history must reset on camera cuts and true geometry identity changes,
# not routine refits/TLAS updates, and only clear after a successful dispatch.
require "should_reset_hybrid_mfx_denoised" "$forward_cpp"
require "clear_hybrid_mfx_denoised_reset" "$forward_cpp"
require "camera_cut" "$forward_cpp"
require "result.scene.blas_built" "$forward_cpp"
require "set_hybrid_mfx_denoised_active(true, true)" "$forward_cpp"
if rg -Fq -- "result.scene.blas_refit > 0 || result.scene.tlas_updated > 0" "$forward_cpp"; then
	echo "Routine BLAS refits/TLAS updates must not reset MetalFX history." >&2
	exit 1
fi

# Cached primitive AS objects are indirect TLAS resources. Each trace must
# explicitly keep both hierarchy levels resident, or a static scene can lose
# reflections after the initial frames.
require "trace->useResource(p_work->tlas.get(), MTL::ResourceUsageRead);" "$hybrid_cpp"
require "for (const NS::SharedPtr<MTL::AccelerationStructure> &blas : p_work->blas)" "$hybrid_cpp"
require "trace->useResource(blas.get(), MTL::ResourceUsageRead);" "$hybrid_cpp"

# Matte material samples must remain outside the ray-reflection path, while
# direct emitter samples progress through a bounded low-discrepancy sequence.
# A frame-invariant sequence leaves visible fixed 4-spp footprints for MetalFX
# to preserve; a frame-random hash creates travelling illumination instead.
require "roughness <= parameters.ao_distance_strength_roughness_flags.z" "$hybrid_cpp"
require "hammersley_dimension(uint frame_index" "$hybrid_cpp"
require "parameters.frame_index, sample, sample_count" "$hybrid_cpp"
require "float3 emitter_normal = -normalize(area_vector);" "$hybrid_cpp"
require "float light_cosine = max(dot(emitter_normal, -light_direction), 0.0f);" "$hybrid_cpp"
require "ggx_reflection_throughput" "$hybrid_cpp"
# The bounce hit must use the same ray-facing normal convention as the primary
# and glossy paths. Otherwise one-sided area-emitter sampling silently drops
# valid interior-wall transport on meshes whose packed winding is opposite.
require "if (dot(hit_normal, -gi_ray.direction) < 0.0f) hit_normal = -hit_normal;" "$hybrid_cpp"
if rg -Fq -- "float light_cosine = abs(dot(" "$hybrid_cpp"; then
	echo "Emissive area lights must not illuminate both sides of an opaque triangle." >&2
	exit 1
fi

# Opaque StandardMaterial3D base-color textures are an optional, bounded
# secondary-hit capability. The texture identity comes from renderer-owned
# storage, UV0 is interpolated from the mesh attribute buffer, and unsupported
# texture forms remain explicit scalar fallbacks.
require "RID attribute_buffer" "$hybrid_h"
require "RID albedo_texture" "$hybrid_h"
require "mesh_surface_get_attribute_buffer_rd_rid" "$forward_cpp"
require "_metal_hybrid_get_uv0_layout" "$forward_cpp"
require "SNAME(\"texture_albedo\")" "$forward_cpp"
require "texture_get_rd_texture(texture, true)" "$forward_cpp"
require "HYBRID_MAX_ALBEDO_TEXTURES 16u" "$hybrid_cpp"
require "intersection_uv(" "$hybrid_cpp"
require "material_albedo(" "$hybrid_cpp"
require "MTL::TextureType2D" "$hybrid_cpp"
require "texture->sampleCount() == 1" "$hybrid_cpp"
require "LIMIT_MAX_TEXTURES_PER_SHADER_STAGE" "$hybrid_cpp"
require "textured_materials" "$hybrid_cpp"
require "texture_fallbacks" "$hybrid_cpp"
require "_make_opaque_checker_texture" "$editor_validation"
require "OpaqueUV0SecondaryChecker" "$editor_validation"
require "--validate-hybrid-texture-transport" "$editor_validation"
require "HYBRID_UV0_TRANSPORT_FLOOR_ROI_MAE" "$editor_validation"
require "Off-camera texture changed primary raster control" "$editor_validation"
require "--validate-hybrid-diffuse-transport" "$editor_validation"
require "HYBRID_DIFFUSE_TRANSPORT_LEFT_DELTA" "$editor_validation"
require "HYBRID_DIFFUSE_TRANSPORT_RIGHT_DELTA" "$editor_validation"
require "if (dot(hit_normal, -gi_ray.direction) < 0.0f) hit_normal = -hit_normal;" "$hybrid_cpp"

# Secondary punctual transport is intentionally a separate bounded contract:
# it must preserve Forward+'s Omni attenuation and energy conventions rather
# than silently treating a point light as an emissive triangle or a spotlight.
require "PunctualLightRecord" "$hybrid_cpp"
require "sample_punctual_lighting" "$hybrid_cpp"
require "MAX_PUNCTUAL_LIGHTS = 16" "$hybrid_h"
require "light_instance_get_cull_mask" "$forward_cpp"
require "LIGHT_PARAM_INDIRECT_ENERGY" "$forward_cpp"
require "light_is_distance_fade_enabled" "$forward_cpp"
require "if (light_storage->light_is_negative(light))" "$forward_cpp"
require "negative Omni" "$forward_cpp"
require "LIGHT_SPOT || type == RSE::LIGHT_AREA" "$forward_cpp"
require "secondary Omni light(s)" "$forward_cpp"
require "--validate-hybrid-omni-diffuse-transport" "$editor_validation"
require "HYBRID_OMNI_DIFFUSE_TRANSPORT_GI_ON_OFF" "$editor_validation"

# Transport culling deliberately starts from the raster-primary list but builds
# an independent conservative ray-transport list. It must fail open whenever
# that bounded proof is not valid; raster/HZ visibility remains primary-only.
require "transport_culling.h" "$transport_culling_cpp"
require "TransportCullingInput" "$transport_culling_h"
require "TransportCullingResult" "$transport_culling_h"
require "transport_cull" "$transport_culling_cpp"
require "TRANSPORT_CULLING_REASON_INVALID_DISTANCE" "$transport_culling_h"
require "TRANSPORT_CULLING_REASON_EMPTY_PRIMARY" "$transport_culling_h"
require "TRANSPORT_CULLING_REASON_INVALID_BOUNDS" "$transport_culling_h"
require "invalid-distance" "$transport_culling_cpp"
require "empty-primary" "$transport_culling_cpp"
require "invalid-bounds" "$transport_culling_cpp"
require "!p_input.enabled" "$transport_culling_cpp"
require "!Math::is_finite(p_input.max_distance)" "$transport_culling_cpp"
require "result.primary_geometry_count == 0" "$transport_culling_cpp"
require "_finite_aabb" "$transport_culling_cpp"
require "disabled and empty-primary fail open" "$transport_culling_test"
require "invalid bounds fail open" "$transport_culling_test"

# Geometry and lights each have their own transport subset and both cross the
# SceneCull -> render_scene -> RenderDataRD boundary.
require "hybrid_geometry_instances" "$scene_cull_h"
require "hybrid_light_instances" "$scene_cull_h"
require "transport_cull(transport_input)" "$scene_cull_cpp"
require "hybrid_geometry_instances.push_back" "$scene_cull_cpp"
require "hybrid_light_instances.push_back" "$scene_cull_cpp"
require "p_hybrid_instances" "$scene_render_rd_h"
require "p_hybrid_lights" "$scene_render_rd_h"
require "p_transport_culling" "$scene_render_rd_h"
require "hybrid_instances" "$render_data_rd_h"
require "hybrid_lights" "$render_data_rd_h"
require "hybrid_transport_bounded" "$render_data_rd_h"
require "render_data.hybrid_instances = &p_hybrid_instances" "$root/servers/rendering/renderer_rd/renderer_scene_render_rd.cpp"
require "render_data.hybrid_lights = &p_hybrid_lights" "$root/servers/rendering/renderer_rd/renderer_scene_render_rd.cpp"

# Every currently supported secondary transport segment is distance-bounded
# when the conservative subset is active. The primary validation ray remains
# deliberately unbounded (100000 m) because raster visibility owns primary
# culling and validation, not this transport optimization.
require "raytracing::ray primary_ray" "$hybrid_cpp"
require "not a secondary transport segment" "$hybrid_cpp"
require "raytracing::ray primary_ray = { camera_position, primary_direction / primary_distance, 0.001f, 100000.0f };" "$hybrid_cpp"
require "sample_cone(light_direction" "$hybrid_cpp"
require "parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 100000.0f" "$hybrid_cpp"
require "parameters.transport_max_distance > 0.0f ? parameters.transport_max_distance : 10000.0f" "$hybrid_cpp"
require "sample_environment_lighting" "$hybrid_cpp"
require "sample_emissive_lighting" "$hybrid_cpp"
require "sample_punctual_lighting" "$hybrid_cpp"
require "if (transport_max_distance > 0.0f && distance_to_light > transport_max_distance) continue;" "$hybrid_cpp"
require "MIN(p_request.ambient_occlusion_distance, p_request.transport_max_distance)" "$hybrid_cpp"
require "MIN(p_request.contact_visibility_distance, p_request.transport_max_distance)" "$hybrid_cpp"

# Punctual lights never inherit the raster list: use the transport subset,
# retain the existing eight-bit ray mask, and serialize a deterministic,
# score-sorted maximum of sixteen positive Omni candidates.
require "const PagedArray<RID> *hybrid_lights = p_render_data->hybrid_lights" "$forward_cpp"
require "MAX_PUNCTUAL_LIGHTS = 16" "$hybrid_h"
require "candidate.score" "$forward_cpp"
require "value.score > omni_candidates" "$forward_cpp"
require "value.stable_id < omni_candidates" "$forward_cpp"
require "MAX_PUNCTUAL_LIGHTS" "$forward_cpp"
require "request.punctual_light_overflow = omni_candidates.size() - serialized_omnis" "$forward_cpp"
require "light.cull_mask & 0xffu" "$hybrid_cpp"

# Keep the one-shot scene diagnostic and the off-camera fixture witnesses
# stable enough for runtime harnesses without reaching into private diagnostics.
require "Hybrid transport culling: state=%s reason=%s distance=%.2f m primary=%d geometry=%d/%d lights=%d/%d." "$forward_cpp"
require "--validate-hybrid-transport-culling" "$editor_validation"
require "func _validate_transport_culling()" "$editor_validation"
require "HYBRID_TRANSPORT_CULLING_FIXTURE_OFF_CAMERA_OMNI" "$editor_validation"
require "HYBRID_TRANSPORT_CULLING_FIXTURE_FAR_MESH" "$editor_validation"
require "HYBRID_TRANSPORT_CULLING_GI_ON_OFF" "$editor_validation"
require "HYBRID_TRANSPORT_CULLING_CAPTURE_PREFIX" "$editor_validation"

# Environment NEE is paired with the primary cosine/BSDF proposal only while
# GI is active. The NEE and BSDF-miss estimators must then use complementary
# balance weights; without GI, NEE remains the only estimator and keeps full
# weight. Secondary/reflection-hit paths have no paired continuation proposal.
require "const bool primary_environment_has_bsdf_proposal = (flags & 4u) != 0u;" "$hybrid_cpp"
require "parameters.frame_index, state, 8u, primary_environment_has_bsdf_proposal, parameters" "$hybrid_cpp"
require "reflection += sample_punctual_lighting(hit_position, hit_normal, hit_diffuse" "$hybrid_cpp"
require "parameters.frame_index, state, 14u, false, parameters" "$hybrid_cpp"
require "parameters.frame_index, state, 11u, false, parameters" "$hybrid_cpp"
require "bsdf_pdf / max(bsdf_pdf + env_pdf, 0.000001f)" "$hybrid_cpp"
if rg -Fq -- "parameters.frame_index, state, 8u, false, parameters" "$hybrid_cpp" || rg -Fq -- "parameters.frame_index, state, 8u, true, parameters" "$hybrid_cpp"; then
	echo "Primary environment NEE MIS must follow the conditional paired-BSDF proposal." >&2
	exit 1
fi

# Full Hybrid already traces exact directional Sky visibility and a paired
# cosine continuation. The bounded world-space contact estimator may modulate
# only low-frequency primary diffuse environment/GI transport. It must not
# modify physical HDR sampling/visibility, reflection, emission, punctual
# direct light, material albedo, guides, or final composition.
full_hybrid_kernel="$(sed -n '/kernel void trace_hybrid(/,/kernel void composite_hybrid(/p' "$hybrid_cpp")"
require "static float sample_diffuse_contact_visibility(" "$hybrid_cpp"
require "sample_count = clamp(sample_count, 2u, 4u);" "$hybrid_cpp"
require "raytracing::ray contact_ray = { world_position + world_normal * 0.003f, direction, 0.001f, maximum_distance };" "$hybrid_cpp"
require "occlusion += 1.0f - smoothstep(maximum_distance * 0.75f, maximum_distance, contact_hit.distance);" "$hybrid_cpp"
require "return clamp(1.0f - strength * occlusion / float(sample_count), 0.0f, 1.0f);" "$hybrid_cpp"
require "float world_space_diffuse_visibility = 1.0f;" "$hybrid_cpp"
require "const float3 diffuse_environment_transport = (environment_direct + indirect) * world_space_diffuse_visibility;" "$hybrid_cpp"
require "emissive_direct + reflection * reflection_weight + diffuse_environment_transport" "$hybrid_cpp"
require "rendering/hybrid_renderer/contact_visibility/sample_count" "$forward_cpp"
require "world-space diffuse contact visibility" "$forward_cpp"
if rg -q 'screen_space_ambient_occlusion|screen_space_diffuse_visibility' "$hybrid_cpp" "$hybrid_h" "$forward_cpp"; then
	echo "Full Hybrid world-space contact visibility must not retain the failed SSAO seam." >&2
	exit 1
fi
if grep -Eq '(emissive_direct|reflection|sample_punctual_lighting)[^;]*\* world_space_diffuse_visibility|world_space_diffuse_visibility[^;]*\*[^;]*(emissive_direct|reflection|sample_punctual_lighting)' <<<"$full_hybrid_kernel"; then
	echo "Full Hybrid world-space contact visibility must remain isolated from emission, punctual direct light, and reflection." >&2
	exit 1
fi
composite_kernel="$(sed -n '/kernel void composite_hybrid(/,/kernel void filter_hybrid(/p' "$hybrid_cpp")"
if grep -Fq -- "world_space_diffuse_visibility" <<<"$composite_kernel"; then
	echo "Full Hybrid world-space contact visibility must not become a final-composition multiplier." >&2
	exit 1
fi

# Explicit hybrid environment transport must never consume the half-float
# raster-reflection octmap. Finite float source values above 65504 otherwise
# become Inf and the fail-safe importance builder correctly assigns them zero
# probability, deleting compact HDR emitters from direct-light sampling.
require "RD::DATA_FORMAT_R32G32B32A32_SFLOAT" "$sky_cpp"
require "Hybrid Environment Sharp Radiance RGBA32F" "$sky_cpp"
require "return sky->hybrid_environment_radiance;" "$sky_cpp"
require "format.format != RD::DATA_FORMAT_R32G32B32A32_SFLOAT" "$forward_cpp"
require "environment_importance_diagnostic" "$hybrid_cpp"
require "post_sky_nonfinite_texels" "$hybrid_cpp"
require "one-shot distribution-rebuild readback" "$hybrid_cpp"

# Only camera-visible transparent triangles require a MetalFX transparency
# overlay. The all-scenario AS list includes off-camera/editor helper geometry
# for ray visibility and must not force the ordinary-temporal fallback.
require "camera_visible_alpha_surfaces" "$forward_cpp"
require "p_render_data->instances->size()" "$forward_cpp"
require "off_camera_alpha_surfaces" "$forward_cpp"
require "camera-visible transparent triangle surface(s)" "$forward_cpp"
require "off-camera/all-scene alpha surface(s) were ignored" "$forward_cpp"

# Grid/origin/transform gizmos are editor-owned 3D primitives, not material
# radiance. Keep them out of the temporal-reconstruction input and composite a
# native-resolution transparent editor overlay after the scene viewport.
require "editor_overlay_viewport->set_transparent_background(true)" "$editor_viewport_cpp"
require "editor_overlay_viewport->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_BILINEAR)" "$editor_viewport_cpp"
require "editor_overlay_viewport->set_scaling_3d_scale(1.0f)" "$editor_viewport_cpp"
require "editor_overlay_texture->set_texture(editor_overlay_viewport->get_texture())" "$editor_viewport_cpp"
require "editor_overlay_texture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE)" "$editor_viewport_cpp"
require "camera->set_cull_mask((1 << 20) - 1)" "$editor_viewport_cpp"
require "editor_overlay_camera->set_cull_mask((1 << (GIZMO_BASE_LAYER + p_index)) | (1 << GIZMO_EDIT_LAYER) | (1 << GIZMO_GRID_LAYER) | (1 << MISC_TOOL_LAYER))" "$editor_viewport_cpp"
require "Camera3D *source_camera = previewing ? previewing : camera;" "$editor_viewport_cpp"
require "editor_overlay_camera->set_global_transform(source_camera->get_camera_transform())" "$editor_viewport_cpp"
if rg -Fq -- "camera->set_cull_mask(((1 << 20) - 1) | (1 << (GIZMO_BASE_LAYER" "$editor_viewport_cpp"; then
	echo "Editor overlay layers must not enter the reconstructed scene viewport." >&2
	exit 1
fi

# The per-viewport flag must override the project Hybrid Renderer mode without
# changing it. Transitioning either way invalidates hybrid temporal state.
require "viewport_set_hybrid_renderer_enabled" "$renderer_viewport_cpp"
require "viewport_is_hybrid_renderer_enabled" "$renderer_viewport_cpp"
require "hybrid_renderer_enabled = p_hybrid_renderer_enabled" "$root/servers/rendering/renderer_rd/renderer_scene_render_rd.cpp"
require "hybrid_renderer_enabled && int(GLOBAL_GET_CACHED(int, \"rendering/hybrid_renderer/mode\")) > 0" "$scene_cull_cpp"
require "p_render_data->hybrid_renderer_enabled ? GLOBAL_GET_CACHED(int, \"rendering/hybrid_renderer/mode\") : 0" "$forward_cpp"
require "set_hybrid_renderer_enabled(p_render_data->hybrid_renderer_enabled)" "$forward_cpp"
require "invalidate_hybrid_history()" "$forward_h"
require "rb_data->invalidate_hybrid_history();" "$forward_cpp"

# A real WorldEnvironment is the lighting source of truth only while Full
# Hybrid is effective for the editor viewports. Turning editor Hybrid Preview
# off must immediately restore Preview Sun ownership without touching the scene.
require "_apply_hybrid_preview_enabled" "$editor_plugin_cpp"
require "viewport_set_hybrid_renderer_enabled(viewports[i]->get_viewport_node()->get_viewport_rid(), p_enabled)" "$editor_plugin_cpp"
require "hybrid_preview_button->set_visible(int(GLOBAL_GET(\"rendering/hybrid_renderer/mode\")) > 0);" "$editor_plugin_cpp"
require "Toggle Hybrid Preview for the 3D editor viewports only. This does not change the project Hybrid Renderer setting." "$editor_plugin_cpp"
require "hybrid_preview_button->is_visible() && hybrid_preview_button->is_pressed()" "$editor_plugin_cpp"
require "directional_light_count > 0 || full_hybrid_environment_owns_preview || !sun_button->is_pressed()" "$editor_plugin_cpp"
require "sun_button->set_disabled(directional_light_count > 0 || full_hybrid_environment_owns_preview);" "$editor_plugin_cpp"
require "Full Hybrid environment\\nlighting owns preview light.\\nPreview Sun disabled." "$editor_plugin_cpp"
require "Scene contains\\nDirectionalLight3D.\\nPreview disabled." "$editor_plugin_cpp"
require "Preview disabled." "$editor_plugin_cpp"
require "hybrid_preview_enabled" "$editor_plugin_cpp"
require "--validate-hybrid-editor-overlay" "$editor_validation"
require "--validate-hybrid-viewport-toggle" "$editor_validation"
require "RenderingServer.viewport_set_hybrid_renderer_enabled" "$editor_validation"
require "get_tree().root.get_texture().get_image()" "$editor_validation"
require "\"name\": \"native\"" "$editor_validation"
require "\"name\": \"metalfx_temporal\"" "$editor_validation"
require "\"name\": \"metalfx_denoised\"" "$editor_validation"
require "editor_viewport.scaling_3d_scale = 0.67" "$editor_validation"
require "ORBIT_LINE_WIDTH_DELTA" "$editor_validation"

denoised_line="$(rg -n "mfx_denoised_effect" "$forward_cpp" | head -1 | cut -d: -f1)"
temporal_line="$(rg -n "mfx_temporal_effect" "$forward_cpp" | head -1 | cut -d: -f1)"
if [[ -z "$denoised_line" || -z "$temporal_line" || "$denoised_line" -ge "$temporal_line" ]]; then
	echo "MetalFX denoised path must be considered before ordinary temporal fallback." >&2
	exit 1
fi

echo "MetalFX temporal-denoised runtime source wiring: PASS"
