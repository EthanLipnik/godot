# Godot path-tracing implementation journal

## 2026-08-19 — M0 audit and baseline

### Decisions

- Keep all early executables under `misc/path_tracing/m0` and label them as spikes. This original decision blocked high-level work on the dual-backend gate; it was superseded by the 2026-08-20 PC-deferred milestone decision below.
- Keep engine paths, symbols, resources, settings, diagnostics, and validation assets product-neutral. Game-specific projects may consume these facilities but must not define their architecture.
- Use the existing cross-platform `RenderingDevice` acceleration-structure vocabulary as evidence for the Vulkan side, but do not force Metal into Godot's Vulkan RT-pipeline abstraction. The first Metal spike uses native Metal compute intersection.
- Treat Flow as a resource-management reference only. Its working tree is dirty, its documented path-tracing mode is unfinished, and its current shader reads eye zero inside the inspected ray-tracing kernel.
- Pin the NVIDIA donor to `e00193295868ee85575ac17b7b53e2dd75acc7de` before adapting any behavior.

### Evidence

- Godot baseline: `872e2d04251efa33f8609c12cc06f3b233c898a7`.
- Vulkan implements capability detection, BLAS/TLAS creation, build/update, ray queries, and RT pipelines. Metal and D3D12 contain explicit unsupported stubs for every RT entry point.
- The NVIDIA donor creates AS-capable buffers for deformed mesh instances, recognizes eight-weight surfaces, evaluates blend shapes and skeleton deformation, preserves previous positions, and queues BLAS update after data changes. Its single 2D output is copied to every render-buffer view, confirming that it is not true stereo.
- Godot OpenXR uses `XR_HAND_JOINT_COUNT_EXT`; visionOS returns two views, consumes compositor color/depth, supports a foveated render region, and documents Metal + Mobile as the only current path.
- Windows remote run already exports, creates a remote temporary directory, uploads via SCP, runs PowerShell scripts through SSH, and forwards normal debug flags.
- Apple feature tables dated 2026-02-05 list MetalFX denoised upscaling at Apple family 9 and compute ray tracing at Apple family 6. The installed SDK 27 headers expose `supportsDevice:` and explicit guide enable flags.
- Flow reference baseline: `6ee5ba9af20718ea315bd2af28789d2ad7748895`. Dirty reference files at audit time: `FlowHybridRenderer+MetalFX.swift`, `FlowHybridRenderer+Rendering.swift`, `HybridRenderer.swift`, and `docs/SpatialStereoMetalFX.md`.

### Environment

- Mac Studio `Mac16,9`; Apple M4 Max, 40-core GPU; 128 GB unified memory.
- macOS 27.0 build `26A5416b`; Metal 4; system Metal framework `382.5.3`; MetalFX framework `40.8`.
- Xcode 27.0 build `27A5237l`; macOS SDK 27.0; Apple clang 21.0.0; Python 3.14.6.
- Display during command-line spikes: remote 2752x2064 display at 120 Hz. M0-A and M0-B are headless and do not use display resolution.
- No discrete driver version exists for the integrated Apple GPU; OS build and Metal framework versions are the driver/runtime identifiers.

### Material architecture refinement

SDK 27 adds requirements omitted by the initial MetalFX contract: explicit enable flags for reactive mask, denoise-strength mask, specular hit distance, and transparency overlay; world-to-view and view-to-clip matrices; reversed-depth declaration; and current-to-previous motion-vector direction in pixel units after scale. The production guide ABI and validator must carry these semantics. Flow's current local descriptor assigns several formats/textures but does not explicitly enable the corresponding optional inputs, so that code is not accepted as proof that those guides participate.

### Measurements

- M0-A and synthetic M0-Fa passed on the Apple M4 Max. The GPU kernel evaluated eight influences per vertex plus one blend shape, wrote the BLAS input, and preserved previous positions before the second deformation. One-shot GPU times: initial deformation 0.007500 ms; BLAS build 0.088625 ms; TLAS build 0.163667 ms; initial one-ray trace 0.127750 ms; previous-position copy 0.003750 ms; second deformation 0.006708 ms; in-place BLAS refit 0.151250 ms; post-refit one-ray trace 0.105542 ms. The center ray hit before deformation and missed after the combined skin/morph change and refit. These tiny workloads are correctness evidence, not renderer budgets.
- M0-B passed as an API/resource viability probe on the Apple M4 Max at 64x64 input and 128x128 output. MetalFX accepted and encoded depth, motion, normal, diffuse/specular albedo, roughness, denoise-strength mask, reactive mask, specular hit distance, and transparency overlay with their explicit enable flags. One-shot GPU time was 1.110417 ms. Inputs were synthetic/uninitialized, so this is not image-quality or temporal-stability evidence.
- M0-J candidate scene-packet validation passed. The fixture is 736 bytes, every record begins at a 16-byte boundary, repeat captures are byte-identical, and the payload FNV-1a hash is `e9078257e76c1508`. The packet fixes Godot's existing previous-UV-minus-current-UV motion convention, reversed `[0, 1]` depth, top-left UVs, column-major right-handed transforms, perceptual roughness, and quiet-NaN invalid pixels. This does not pass the ABI gate until both shader layouts are reflected and both backends replay the same packet.

### Unresolved risks and blockers

- No reachable Windows RTX 5080 environment or its OS, NVIDIA driver, Vulkan SDK, compiler, and GPU capture tools have been identified, so M3-C, M3-D, and M3-Fb cannot yet run.
- Vulkan tools and `slangc` are not installed on this host. SCons 4.10.1 is pinned inside the ignored M0 build environment, not installed globally.
- DLSS is absent and remains an optional adapter. No restricted SDK content will be added to the repository.

### Build evidence

- The default macOS editor build failed before compilation because no MoltenVK SDK path was available. It also reported missing optional AccessKit and ANGLE dependencies.
- A reproducible Metal-only development editor build passed with SCons 4.10.1 and `platform=macos arch=arm64 target=editor dev_build=yes vulkan=no angle=no accesskit=no -j12`. Clean build elapsed time reported by SCons: 2 minutes 37.99 seconds. Output: `bin/godot.macos.editor.dev.arm64`.
- This build proves the current fork compiles on the selected Mac with the native Metal driver. It does not satisfy the full macOS build deliverable because the Vulkan/MoltenVK configuration is intentionally absent, and it provides no Windows evidence.

### Next executable step

Add paired MSL/GLSL layout declarations and reflection checks for the candidate records, then use the packet as the shared input for the two-bounce Metal/Vulkan experiment. Vulkan/RTX execution remains blocked on a reachable configured Windows host.

## 2026-08-20 — PC-deferred milestone refinement

### Decision and evidence

- The original M0 mixed portable contract work, Apple execution, Windows execution, proprietary streaming, and physical-headset integration into one gate. With no reachable PC, that structure blocked unrelated work even though the canonical packet, Metal AS/deformation spike, MetalFX API probe, shader compilation, temporal fixtures, stereo simulation, asset preparation, and protocol mocks are independently testable on macOS.
- M0 now accepts only portable foundations and the Apple reference implementation. M1 makes that reference usable in the Mac editor, and M2 advances PC-independent product foundations. Windows/Vulkan parity, comparative backend decisions, DLSS, CloudXR hosting, remote execution, and physical streamed stereo are concentrated in M3.
- This changes sequencing, not requirements. Metal-only results remain backend-qualified; no renderer feature is cross-platform complete until the frozen suite passes on Vulkan/RTX.
- The Windows performance requirement is now sustained true-stereo 90 Hz on the RTX 5080 without frame generation. M4 is a dedicated post-correctness optimization milestone covering full and hybrid rendering, sampling/reuse, traversal and scheduling, stereo sharing, AS/LOD/residency work, async overlap, reconstruction, diagnostics, and content budgets. Its production profile must freeze resolution and quality floors before timing so dynamic scaling cannot disguise a failed gate.

### Next executable step

Add paired MSL/GLSL layout declarations and reflection checks, then build the deterministic two-bounce Metal corpus. In parallel with later Mac work, keep the Windows certification bundle source-complete and ready to execute without redesign when a PC becomes available.
