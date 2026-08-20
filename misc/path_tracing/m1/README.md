# Godot path-tracing M1 — Mac reference renderer

M1 is a usable, provisional Mac reference path tracer. It is not a Vulkan implementation, a dual-backend renderer, an XR runtime, or evidence of RTX 5080 performance.

## Implemented boundary

- `servers/rendering/path_tracing`: frozen schema-1 scene packet, self-contained capture v1, deterministic compiler/validator, guide validator, and capability-driven backend interface.
- `scene/resources/path_tracing_*`: deterministic live Godot scene extraction, current/previous cameras and deformation, scalar StandardMaterial3D conversion, point/directional/environment lights, and visible diagnostics for unsupported features.
- `servers/rendering/renderer_rd/effects/metal_path_tracing.*`: native Metal BLAS/TLAS/intersection-query renderer, cached pipeline, bounded scalar diffuse/specular/emissive closure, analytic shadows, environment, progressive accumulation, stereo views, complete guide output, static reuse, dynamic refit, and stale-cache eviction.
- `servers/rendering/renderer_rd/effects/metal_fx.*`: temporal denoised MetalFX adapter with explicit optional-guide enables, matrices, reversed depth, motion scale, reset, and per-eye context support.
- `editor/plugins/path_tracing_editor_plugin.*`: bottom-panel interactive/progressive renderer, resolution and bounce controls, history controls, MetalFX, guide inspection, capability diagnostics, and deterministic command-line validation.
- `editor/export/remote_windows_launch_plan.*`: validated credential-free manifest and argv previews plus a strict mocked endpoint lifecycle. It performs no network side effects and is not live deployment evidence.

The accepted M1 material subset is scalar base color, continuous metallic/roughness in a bounded approximate diffuse/specular model, and scalar emission. Textures, alpha, normal mapping, transmission, custom shaders, and SpotLight3D produce an explicit visible fallback or error. The paired closure compile gate proves the accepted record/evaluation expression compiles as MSL and Vulkan GLSL/SPIR-V; Vulkan execution remains M3 work.

## Deterministic acceptance commands

```sh
misc/path_tracing/m1/run_scalar_closure_compile_gate.sh
bin/godot.macos.editor.dev.arm64 --headless --test --test-case='*PathTracing*'
bin/godot.macos.editor.dev.arm64 --display-driver macos --rendering-driver metal --audio-driver Dummy --path misc/path_tracing/m1/validation_project -- --validate-deformation
bin/godot.macos.editor.dev.arm64 --display-driver macos --rendering-driver metal --audio-driver Dummy --editor --path misc/path_tracing/m1/validation_project res://animated_silhouette_mirror.tscn -- --validate-path-tracing-editor
```

Run the Metal commands with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` for the recorded gate. The editor automation verifies nonzero reconstructed radiance, mirror secondary hits, animation-dependent output, dynamic BLAS refit, no fallback material, and temporal denoised MetalFX use.

Godot's Metal RenderingDevice timestamp-query methods currently return zero by design. The adapter rejects those zero values instead of reporting false measurements. Individual GPU stages remain measured by the M0 standalone Metal harness; production in-engine Metal counter sampling is an explicit follow-up and does not alter renderer correctness evidence.
