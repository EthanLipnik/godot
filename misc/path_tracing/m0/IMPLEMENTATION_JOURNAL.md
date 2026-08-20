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

## 2026-08-20 — M0 gate completion

### Decisions

- Freeze schema 1 at two distinct cameras and 1,024 bytes. The packet, expected invariants, and compiled SPIR-V are the inputs later replayed by M3; Metal-only success does not authorize layout changes that bypass that replay.
- Keep paired MSL/GLSL as the production baseline. Shader Slang 2026.14 is feasible for further evaluation but is not adopted without matched runtime and diagnostic evidence on Windows.
- Accept MetalFX as useful for the current display-space sequence while retaining the small-moving-HDR-highlight regression as a named M1 case. Aggregate quality improvement does not erase a local failure.
- Keep the representative source scene external and licensed. Reusable audit/export code accepts the scene and mesh name as arguments and exports only a deterministic geometry fixture.

### Evidence and measurements

- Full `run_m0_gate.sh` passed at checkpoint `d3e0585dd9b7df840660cd6a2388b96e8351746b` with a dirty pre-commit tree, as expected. Windows runtime certification and renderer parity are both false.
- Schema-1 packet: 1,024 bytes; payload FNV-1a `1136ee6ec9ba3139`; exact-repeat capture; seven corruptions rejected. MSL GPU sizes `[16, 64, 64, 288, 160, 80, 64, 80]` and key offsets `[272, 128, 16, 32]` matched the host ABI. GLSL compiled for Vulkan 1.2 with glslang 16.5.0 and validated with SPIRV-Tools; SPIR-V SHA-256 `893da411ff1726f051fb04d7f62fd91764d014968c68c48dea4dff18f09ad047`.
- Two-bounce Metal corpus at 192x128: initial capture `a477781cab9beb9d`, deformed capture `7de9af3f9d530e1b`, with 86 reflected dynamic pixels in each state and a changed silhouette. Latest full-gate GPU times were 0.134750 ms BLAS build, 0.164333 ms TLAS build, 0.128375 ms initial trace, 0.063125 ms exact-repeat trace, 0.092583 ms refit, and 0.055750 ms deformed trace. They are tiny-scene stage measurements, not renderer budgets.
- Animated MetalFX stereo suite: 96x64 internal to 192x128 output, 12 frames, independent per-eye scalers and histories. Display-space sequence RMSE improved from 0.22792652 to 0.10273325. Linear-HDR RMSE worsened from `[0.17370550, 0.17299154]` for noisy nearest reconstruction to `[0.19056773, 0.19484070]`; the failure category is `small_moving_hdr_highlight_energy`. Resetting only the left eye did not perturb the right-eye control. Total GPU time across the 12 tiny frames was 8.364 ms left, 8.238 ms right, and 8.254 ms control.
- Shader Slang 2026.14 was provisioned from the official macOS arm64 release with archive SHA-256 `b8e2e277abd5cbe4591168ab4350ee46bd5631f705410208cb505c795fbab4b9`. The bounded closure/layout corpus produced validated SPIR-V SHA-256 `582941d5c49438e0ce1088669b25b4df99d897cd60687fbbce8ba85debd6b986` and Xcode-compiled MSL SHA-256 `9ec9126223cee5e3eecd9d636c321ee6584c79e5179e2a7897d127f69abd935d`.
- Blender 4.3.2 build `32f5fdce0a0a` opened the external 4.2 scene with auto-execution disabled. The sanitized export was byte-identical across two runs: 12,282,496 bytes, SHA-256 `d4d919f20cabc94cfadd18cc78b35c297825a86d96ee4b8f8e6d521109f58a03`, one mesh primitive, one skin, eight runtime morph targets, and eight exported deform weights. Of 141 deform groups, 30,331 vertices required pruning; mean discarded normalized weight was 0.011531% and maximum was 3.30025%.
- The Windows certification bundle contains only non-proprietary frozen inputs and scripts. Its PowerShell entry points could not be syntax- or runtime-tested locally because `pwsh` and a Windows host are unavailable.

### Correctness findings

- The ignored SCons environment retained a shebang pointing to the directory's former product-specific name, causing the first aggregate gate to fail before compilation. `build_macos_editor.sh` now verifies the environment's SCons executable and recreates the venv in place when stale.
- Early asset export attempts duplicated hidden source state without its parent, counted controller groups as deform bones, and selected muted/self-relative authoring shape keys. Those caused an empty mesh, four effective exported deform weights, and zero morph targets respectively. The final exporter evaluates only armature deform groups, restores parent context, selects Basis-relative non-empty keys, and validates the GLB rather than trusting exporter success.
- Pipeline exit status had been masked by `tee` in three runners. They now capture output and propagate the executable's real status.

### Unresolved risks and blocker

- M0 supplies no Windows execution evidence. The smallest unblock is access to the target RTX 5080 PC with its exact Windows build, NVIDIA driver, Vulkan SDK, MSVC toolchain, and PowerShell; then run the prepared M3 environment probe and frozen replay bundle.
- Vulkan RT traversal, the query-versus-pipeline choice, DLSS, CloudXR, sustained 90 Hz stereo, and renderer parity remain unverified.
- MetalFX highlight-energy preservation and representative production-scale memory/frame budgets remain open M1/M4 work.

### Next executable step

Begin M1 with a backend-neutral scene-compiler boundary and deterministic capture/replay inside the Godot renderer, using schema 1 unchanged. Wire the native Metal reference behind that boundary before adding editor controls or broadening materials.
