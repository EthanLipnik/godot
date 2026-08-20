# Godot path-tracing M1 implementation journal

## 2026-08-20 — Mac reference path tracer gate

### Decisions and evidence

- Kept schema-1 frame packets byte-compatible with M0 and added capture v1 for canonical mesh streams, including current/previous deformed positions. Material references are one-based; zero selects the visible fallback.
- Live extraction uses stable node-path ordering and captures actual Godot cameras, transforms, meshes, eight-weight skinning, blend shapes, analytic lights, and color environments. Unsupported materials are magenta with diagnostics. Spot lights are rejected because schema 1 cannot encode spot direction; silently treating them as point lights was incorrect.
- The Metal backend writes Godot textures directly, uses one BLAS per geometry and a TLAS, rebuilds topology changes, refits dynamic content, reuses unchanged geometry, evicts absent geometry, and caches its runtime-compiled pipeline. Three ordered render-graph callbacks encode BLAS, TLAS, and tracing on the same Metal command-buffer timeline.
- The renderer emits depth, current-to-previous normalized motion, world normal, diffuse/specular albedo, roughness, denoise strength, reactive mask, specular hit distance, and transparency guides for each view. Progressive and interactive histories are explicit.
- A native MetalFX temporal-denoised adapter binds and enables the complete guide set. A measured SDK prototype accepted canonical writable `R32Float` depth directly, so a conversion pass was removed. The denoise-strength guide is zero: Apple defines one as ignoring the denoiser.
- The first native callback in a frame exposed a nil Metal command buffer. `MDCommandBuffer::get_command_buffer()` now creates it lazily. Callback-written textures now resolve pending clears before native access, preventing a later lazy clear from erasing path-traced output.
- Remote Windows plans are deterministic argv vectors, not shell command strings. Their manifest contains commit, asset hash, preset, backend, and artifact name but no host, user, password, or credential. A strict mock rejects out-of-order lifecycle events.
- MSL and GLSL/SPIR-V paired closure sources compiled with Apple Metal 3.2 and glslang Vulkan 1.2. AIR SHA-256: `c24f3e384854f0cb302aaec76cda78b78a4e0d02b28c3ef7d6f3c5c953746116`; SPIR-V SHA-256: `be802e6b981d9f83de95ec8c0b2432f96c74c762b653e3112a00ad7b9abc4dc8`.

### Recorded conditions

- Donor revision: Godot `1160501c5f2aa02c0beb955487ae565e99c67bb5` plus the M1 change set that this journal accompanies.
- Host: Mac Studio `Mac16,9`, Apple M4 Max 40-core GPU, 128 GB unified memory.
- OS/runtime: macOS 27.0 build `26A5416b`, Metal framework `382.5.3`, MetalFX `40.8`.
- Toolchain: Xcode 27.0 build `27A5237l`, macOS SDK 27.0, Apple clang 21.0.0, SCons 4.10.1, glslang 16.5.0, SPIRV-Tools from Homebrew.
- Validation: Metal API validation and GPU shader validation enabled; 320x180 trace reconstructed to 640x360; one view in the actual editor gate and two independent views in the native backend/MetalFX test; 1 spp interactive, two bounces, deterministic capture/seed.

### Acceptance evidence

- 19 focused cases / 184 assertions passed, including packet/capture corruption, extraction, guides, remote preset/mock lifecycle, native stereo rendering, per-eye MetalFX contexts, static reuse, and dynamic refit.
- Live combined deformation: `combined_deformation_changed=true`, three vertices with eight influence slots.
- Actual editor scene: nonzero 640x360 reconstructed output (`1,843,200` bytes), changed animated frame, one requested dynamic BLAS refit, 1,121 finite mirror specular-hit-distance pixels in both captures, no material fallback, and temporal denoised MetalFX active. M2 later found that this test changed its random sample and read editor output asynchronously, so it did not independently prove refit correctness; see the M2 correction.
- The final regression rerun kept the M0 packet hash and images byte-identical. Standalone stage measurements for its tiny 192x128 corpus were BLAS build 0.163417 ms, TLAS build 0.168375 ms, initial trace 0.132375 ms, exact-repeat trace 0.055458 ms, BLAS refit 0.104125 ms, and deformed trace 0.056000 ms. These are correctness-prototype conditions, not production budgets.

### Unresolved risks and next executable step

- Vulkan execution, RTX/DLSS behavior, parity, XR, and 90 Hz remain untested and cannot be inferred.
- Godot's Metal timestamp query implementation intentionally returns zero. The M1 backend now rejects zero durations; add Metal counter sampling for production in-engine stage timings without replacing the valid standalone evidence.
- Schema 1 has no camera jitter field, textured closure representation, spot direction, or environment importance distribution. M1 therefore uses zero jitter, scalar materials, point/directional lights, and a constant-color environment. Evolve these contracts explicitly rather than overloading fields.
- The next executable milestone is M2: product-independent asset preparation, deformation/pose validation, visibility, gesture/tether foundations, and mocked companion/endpoint protocols. M3 remains the first Windows certification gate.

## 2026-08-20 — Runtime-boundary and primitive-mesh correction

- User testing confirmed that the M1 `Interactive` label was ambiguous. It is a low-sample editor reference preview, not the game renderer and not a WYSIWYG 3D viewport. The architecture now places a provisional Mac runtime/WYSIWYG hybrid milestone before Windows certification; the separate panel remains a reference and guide-inspection tool.
- `MeshInstance3D::bake_mesh_from_current_deformation()` requires an `ArrayMesh`. The scene compiler called it unconditionally, so valid `PlaneMesh`, `SphereMesh`, and other `PrimitiveMesh` resources failed before tracing. Static non-`ArrayMesh` instances now copy their generated triangle surfaces and active materials into the capture without entering the deformation path. Actual `ArrayMesh` deformation retains the existing previous/current bake and BLAS-refit behavior.
- Next executable step: integrate renderer-owned incremental geometry/material/light state with the Forward+ frame graph, then implement the same capability-gated Metal hybrid path for editor and game cameras. Do not represent the reference panel as that integration.
