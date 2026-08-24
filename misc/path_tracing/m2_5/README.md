# M2.5 — runtime flux renderer

This milestone moves ray effects from the M1 reference panel into the normal Forward+ editor/game frame graph. Work in this directory is evidence and deterministic validation support; production code remains under the product-neutral rendering server.

The milestone is incomplete until the recorded macOS editor/game parity gate passes. Windows Vulkan/RTX and DLSS parity remain M3/M4 gates and cannot be inferred from the Mac result.

## Current provisional runtime slice

Enable `rendering/flux/ray_tracing/enabled` in Project Settings:

- `Standard`: ordinary raster Forward+.
- `Flux`: adds emissive-triangle direct lighting with finite visibility rays, provisional diffuse transport, GGX-distributed ray reflections, and ambient occlusion. Primary and secondary intersections evaluate renderer-owned scalar albedo/metallic/roughness/emission records and interpolated triangle normals. Primary hit identity currently costs an extra ray until the thin material guide is written. Textures, UVs, alpha, transmission, and the full supported closure set remain open gates, so this mode does not claim full material parity yet.

On a supported macOS Metal device, `MetalFX (Temporal)` first selects the full-guide `MTLFXTemporalDenoisedScaler` for Flux. The runtime allocates and binds separate normal, diffuse-albedo, specular-albedo, roughness, denoise-strength, reactive-mask, specular-hit-distance, and transparency-overlay guides per view. That path bypasses the effect-local custom temporal accumulation: running two independent temporal filters on the same noisy ray term is invalid and was the source of the slow, darkening convergence seen in the Cornell fixture. If the device or guide contract cannot support the denoised scaler, the runtime explicitly falls back to ordinary MetalFX Temporal; it does not bind invented guides.

This is a macOS Metal reference implementation only. Windows Vulkan/RTX, DLSS, and the required cross-backend parity suite remain blocked on the Windows milestone. The bottom editor dock is named **Reference Renderer** because it is an offline correctness harness; Flux output appears directly in both the 3D editor viewport and the running game.

Run the deterministic native and game checks with:

```sh
bin/godot.macos.editor.dev.arm64 --headless --test --test-case='*[PathTracing][Metal]*'
bin/godot.macos.editor.dev.arm64 --rendering-driver metal --rendering-method forward_plus --path misc/path_tracing/m2_5/validation_project
bin/godot.macos.editor.dev.arm64 --editor --rendering-driver metal --rendering-method forward_plus --path misc/path_tracing/m2_5/validation_project res://main.tscn -- --validate-flux-editor
bin/godot.macos.editor.dev.arm64 --rendering-driver metal --rendering-method forward_plus --path misc/path_tracing/m2_5/validation_project -- --benchmark-flux
bash misc/path_tracing/m2_5/test_metal_denoised_runtime.sh
```

The source check fails if a complete denoised guide allocation/wiring path, the ordinary temporal fallback, or the custom-history bypass disappears. The game fixture captures raster and flux frames after separate warmups and fails unless their mean absolute RGB difference exceeds the fixed threshold. It is intentionally small and deterministic; it is correctness evidence, not the M4 performance profile.
