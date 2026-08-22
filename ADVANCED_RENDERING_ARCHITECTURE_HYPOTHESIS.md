# Advanced Rendering and XR Architecture Hypothesis

**Status:** Working architecture hypothesis, not a specification

**Repository baseline:** Godot 4.8-dev fork

**Last revised:** 2026-08-21

**Primary development host:** macOS

**Primary development and reference-rendering host:** macOS on Apple silicon

**Deferred high-fidelity runtime:** Windows PC with NVIDIA RTX 5080

**Primary headset:** Apple Vision Pro through CloudXR/Foveated Streaming

## 1. Purpose and decision rule

This document proposes reusable Godot engine architecture for physically convincing high-speed VR applications. A superhero traversal simulator motivates the initial workload, but does not own the renderer, reconstruction, streaming, interaction, or asset-pipeline architecture. It is deliberately written as a set of hypotheses. An implementation agent should not treat a named API, donor branch, milestone, or performance target as sacred. It should preserve the product requirements, reproduce the evidence, benchmark alternatives, and replace a proposal when a demonstrably better solution exists.

The non-negotiable product requirement is that the rendering work is not Windows-only. Hardware availability should not unnecessarily serialize development, however. The Mac implementation is the executable reference backend and may advance through explicitly Apple-only and portable-foundation gates before a PC is available:

> The first usable **cross-platform-certified** path-tracing milestone must provide real, local path tracing inside the Godot editor on macOS and an equivalent RTX-capable implementation on Windows. Earlier Mac reference milestones are allowed, but must remain backend-qualified. MetalFX temporal denoised upscaling is part of the Mac reference path, not deferred polish.

“Equivalent” means the two backends implement the same scene, material, light, camera, dynamic-geometry, stereo, output-guide, and debugging contracts. It does not mean bit-identical pixels or the same vendor denoiser. A feature is not complete if it works only on Vulkan/RTX or only on Metal.

This document distinguishes **implementation progress** from **cross-backend certification**. Work may proceed on the Mac when it produces backend-neutral contracts, deterministic replay inputs, paired shader sources, CPU reference results, Metal evidence, or platform adapters that can later be exercised unchanged on Windows. Such work must remain labeled provisional wherever Vulkan/RTX evidence is missing. A missing PC blocks only Windows execution, comparative performance decisions, proprietary NVIDIA adapter validation, CloudXR host integration, and the final parity certificate; it does not block productive Mac implementation or unrelated product foundations.

The sequencing rule is:

1. Make every portable decision testable without a PC.
2. Use Metal as the first executable backend, not as a substitute for Vulkan evidence.
3. Prepare Windows source, build recipes, captured inputs, expected invariants, and machine-readable result formats before PC access.
4. Defer decisions that genuinely depend on comparative GPU evidence.
5. Run the prepared Windows certification suite as one bounded milestone when suitable hardware becomes available.

Each major implementation proposal should be managed with four fields:

1. **Hypothesis:** the proposed solution.
2. **Evidence:** code, documentation, or measurements supporting it.
3. **Falsification test:** the smallest experiment that could disprove it.
4. **Fallback:** the next-best architecture if it fails.

Architecture decisions should be recorded as short ADRs. An ADR must name the measured hardware, OS, SDK, driver, scene, resolution, sample count, and timings. Marketing names such as “path traced” or “AI upscaled” are not acceptance criteria.

## 2. Product vision

The long-term product is a high-presence Spider-Man simulation platform rather than a narrow web-swinging game. It should combine:

- physically rich lighting and materials;
- low-latency, hand-driven VR interaction;
- a highly articulated character with convincing pose-dependent suit deformation;
- a city capable of traversal at extreme speed and scale;
- practical development on macOS;
- high-end remote rendering from an RTX PC to Apple Vision Pro;
- later native execution on visionOS and iOS where hardware allows it; and
- extensible tracking inputs, including an iPhone full-body tracker.

The first playable experience remains intentionally small: a Spider-Man character, tracked hands and gestures, web attachment, web tension, release, and swinging through a representative city district. The engine work described here supplies rendering, XR, streaming, character deformation, deployment, and tracking infrastructure. Gameplay mechanics should consume those systems without becoming embedded inside the renderer or platform layers.

## 3. Goals by horizon

### 3.1 Immediate goals

- Establish a maintainable Godot fork and a reproducible macOS build, plus a pinned and documented Windows build recipe that remains unverified until a Windows worker is available.
- Build deterministic captured validation scenes and a native Metal reference implementation locally in the macOS editor.
- Use native Metal ray tracing and MetalFX temporal denoised upscaling on supported Apple silicon.
- Define the Vulkan ray-tracing and optional NVIDIA/DLSS adapters behind the same contracts, compile portable shader inputs where possible on macOS, and reserve runtime selection and tuning for Windows certification.
- Support bones, eight-weight skinning, blend shapes, and dynamic acceleration-structure updates on Metal; certify identical semantics on Vulkan before calling the feature cross-platform complete.
- Target sustained true-stereo 90 Hz on the RTX 5080 through measured full or hybrid rendering, without counting frame generation.
- Create an editor control that selects raster, interactive path tracing, or progressive reference rendering and exposes useful diagnostics.
- Prepare remote-export configuration validation and command generation without requiring a live endpoint; exercise transfer, launch, logging, and debugger attachment when the PC becomes available.
- Establish a sanitized character export and runtime-rig pipeline for the BND model.

### 3.2 Near-term goals

- True stereo/multiview path tracing with independent per-eye history and a shared scene acceleration structure.
- A native SwiftUI/RealityKit visionOS companion using Apple Foveated Streaming and NVIDIA CloudXR on the PC.
- Twenty-six-joint hand tracking from Vision Pro into Godot, with robust gesture inference and arm IK.
- Depth and alpha streaming where supported, plus resilient discovery, pairing, diagnostics, and reconnect behavior.
- A web-swinging vertical slice in a streamed city with believable character deformation and stable VR latency.
- Vision Pro microphone upstream to the Windows host.

### 3.3 Long-term goals

- iPhone-on-tripod full-body tracking, fused with Vision Pro head and hand tracking.
- A native visionOS build using the Mobile renderer and purpose-built reduced assets.
- Native ARKit hand tracking in Godot’s visionOS XR interface.
- A reusable native Metal ray-tracing path on iPhone/iPad hardware, constrained by thermals and memory rather than assumed to match desktop quality.
- Higher-order simulation: richer web physics, cloth accessories where appropriate, traffic/crowds, destructibility, weather, volumetrics, and scalable city streaming.

## 4. Explicit non-goals and separations

- The first renderer milestone is not required to run path tracing natively on Vision Pro.
- The CloudXR companion and the native Godot visionOS game are separate products with different renderers and asset budgets.
- Full real-time cloth is not the primary wrinkle solution for a skin-tight suit.
- MetalFX frame interpolation is not a first-line VR performance strategy; added latency and interpolation artifacts must be independently proven acceptable before use.
- Foveated video encoding must not be confused with foveated ray allocation. Apple does not expose the gaze focus region to the application.
- Direct `.blend` import of the full BND source scene is not a runtime asset pipeline.
- Proprietary SDK binaries should not be committed unless their licenses explicitly allow redistribution.
- A passing monoscopic demo is not evidence of a viable VR renderer.

## 5. Evidence from the current code and donor implementations

### 5.1 Current Godot fork

The repository is based on Godot 4.8-dev. It contains low-level ray-tracing concepts in `RenderingDevice`; the Vulkan driver implements the relevant ray-query/ray-pipeline facilities, while the Direct3D 12 and Metal driver methods are presently stubs. There is no high-level production path tracer in core.

The existing OpenXR module is a strong foundation. It already models hand tracking with the OpenXR twenty-six-joint layout and contains hand-interaction support. The visionOS XR module uses Metal and Compositor Services, supports layered stereo, head tracking, foveation, color/depth submission, and mixed/full immersive modes, but is currently a Mobile-renderer path and does not provide the native ARKit hand-tracking bridge required by the eventual standalone target.

Godot already has remote Windows deployment over SSH: export, SCP transfer, PowerShell launch/cleanup, remote-run UI, and debugger forwarding. This should be extended and productized instead of replaced.

### 5.2 NVIDIA Godot path-tracing branch

The `nvidia-pt-dlss-dev` branch is useful donor code and a behavioral reference, but must not define the cross-platform architecture. It contains substantial Forward+ integration: scene/material extraction, bindless resources, custom materials, skinned meshes, MultiMesh support, transparent raster overlays, fog, guide buffers, and DLSS Ray Reconstruction.

Its dynamic-mesh path is especially relevant. It treats blend-shaped meshes as instanced/deformed geometry; its compute deformation combines blend shapes and skeleton skinning, supports eight weights, writes an acceleration-structure-capable vertex buffer, builds an updateable BLAS, and updates that BLAS on later frames. This is evidence that the BND suit’s silhouette-changing cloth morphs can participate in ray-traced reflections, shadows, and GI. It must still be verified on the exact donor revision and reproduced in both target backends.

Its current stereo behavior is not sufficient: a single two-dimensional ray-tracing output is dispatched once and copied to each view. True XR requires per-eye camera state, output layers, motion, depth, disocclusion, jitter, denoiser history, and compositing. The scene TLAS/BLAS can generally be shared.

Its visibility masks also require extension. A first-person body should be hidden from primary eye rays while remaining available to reflection, shadow, and indirect rays. Until ray-category masks exist, use separately authored first- and third-person render variants.

### 5.3 Flow Engine as the MetalFX reference

The local Flow Engine repository at `/Users/ethan/Developer/Flow Engine` provides a concrete Apple-platform reference for MetalFX resource management and stereo integration. Relevant files include:

- `Sources/FlowMetalRenderer/Hybrid/FlowHybridRenderer+MetalFX.swift`
- `Sources/FlowMetalRenderer/Hybrid/FlowHybridRenderer+Rendering.swift`
- `Sources/FlowMetalRenderer/Hybrid/HybridRenderer.swift`
- `Sources/FlowMetalRenderer/Hybrid/FlowHybridRenderer+RayTracing.swift`
- `docs/SpatialStereoMetalFX.md`

Flow configures `MTLFXTemporalDenoisedScalerDescriptor` with color, depth, motion, normal, diffuse albedo, specular albedo, roughness, denoise-strength mask, reactive mask, and optional specular hit distance. It applies Halton jitter, motion-vector scale, history resets, exposure behavior, and encodes the scaler on the same command buffer. It also demonstrates the important stereo rule: MetalFX consumes 2D textures, so layered stereo inputs need per-slice 2D views and one scaler execution per eye, with each result copied to the matching compositor layer.

Flow’s path-tracing mode is explicitly unfinished and should not be used as proof of path-tracing correctness. Its Metal acceleration-structure, heap/scratch, layered-texture, and MetalFX patterns are valuable evidence, but the Swift implementation should not be transplanted wholesale into Godot’s C++ renderer.

### 5.4 M0 audit refinement (2026-08-19)

The initial audit reproduced the broad code findings above against Godot commit `872e2d04251efa33f8609c12cc06f3b233c898a7`. Vulkan implements capability detection, BLAS/TLAS build and update, ray queries, and ray-tracing pipelines. Metal and Direct3D 12 expose the same low-level interface but every ray-tracing entry point is an explicit unsupported stub. The pinned NVIDIA donor revision is `e00193295868ee85575ac17b7b53e2dd75acc7de`; it confirms the deformed-buffer/eight-weight/blend-shape/BLAS-update behavior and also confirms the stereo defect by copying one two-dimensional ray-tracing output into every view.

The selected Apple reference host is a Mac Studio `Mac16,9` with an Apple M4 Max 40-core GPU and 128 GB unified memory, running macOS 27.0 build `26A5416b`, Metal framework `382.5.3`, MetalFX `40.8`, Xcode 27.0 build `27A5237l`, macOS SDK 27.0, and Apple clang 21.0.0. For an integrated Apple GPU, the OS build and Metal framework versions are the applicable driver/runtime identifiers.

The SDK 27 `MTLFXTemporalDenoisedScaler` headers materially refine the proposed guide contract. Optional reactive-mask, denoise-strength-mask, specular-hit-distance, and transparency-overlay inputs have explicit enable flags. The scaler also consumes world-to-view and view-to-clip matrices and an explicit reversed-depth declaration. Its documented motion convention is current-to-previous in pixel coordinates after applying the configured motion-vector scale. A spike that only assigns textures or creates a scaler does not prove that the optional guides participate or that temporal reconstruction is correct. The Flow working copy inspected during M0 assigns several optional formats and textures but does not explicitly set all corresponding enable flags, so it remains a pattern source rather than acceptance evidence.

The reusable renderer M0 experiment map, outputs, and gates live in `misc/path_tracing/m0/README.md`; the concise renderer evidence journal lives beside it in `IMPLEMENTATION_JOURNAL.md`. Product-specific asset, gameplay, and streaming work must consume generic engine interfaces rather than appearing in engine code or subsystem names.

### 5.5 M0 outcome (2026-08-20)

The revised portable-contract and Apple-reference gate passed on the audited M4 Max host. The schema-1 packet is 1,024 bytes with two distinct cameras and payload FNV-1a `1136ee6ec9ba3139`; seven structural and semantic corruptions are rejected. CPU, MSL, and GLSL/std430 declarations agree on the frozen layout. The GLSL corpus compiles for Vulkan 1.2 with glslang 16.5.0 and validates with SPIRV-Tools; execution on Vulkan remains an M3 gate.

Native Metal rendered the deterministic 192x128 two-bounce corpus with emissive geometry, environment lighting, a mirror reflection, and a dynamically refitted BLAS. Repeated captures were byte-identical, and the reflected dynamic silhouette changed after deformation. These sub-millisecond micro-scene timings are correctness evidence, not production budgets.

The animated 12-frame, two-eye MetalFX suite used distinct cameras, matrices, jitter, guides, scalers, and histories. Resetting only the left history left the right eye byte-identical to an independent right-eye control. MetalFX improved the combined sequence's display-space RMSE from 0.22792652 to 0.10273325, but worsened linear-HDR RMSE around a small moving highlight. This falsifies any blanket claim that the scaler improves every signal metric; the issue remains an explicit M1 reconstruction case rather than being hidden by the aggregate result.

Shader Slang 2026.14 compiled the bounded closure/layout experiment to both validated SPIR-V and MSL. That establishes feasibility for further evaluation, not adoption: paired MSL and GLSL remain the baseline until M3 can compare runtime semantics, diagnostics, and performance on both GPUs. The source asset audit and deterministic sanitized GLB export also passed with embedded scripts disabled, deform influences pruned to eight, eight non-empty morph targets, and no material or texture payload in the geometry fixture.

At the M0 boundary, the Windows certification bundle contained the frozen packet, SPIR-V, expected invariants, environment probe, and pinned build recipe. Section 5.8 records its later replacement with the self-contained schema-2 certification bundle. PowerShell execution, Vulkan traversal, RTX/DLSS behavior, CloudXR, 90 Hz performance, and Metal/Vulkan parity remain untested and must not be inferred from M0.

### 5.6 M1 outcome (2026-08-20)

The usable Mac reference gate passed on the same M4 Max host. The actual Godot editor now extracts a deterministic live scene, bakes combined blend-shape and eight-weight skeleton deformation, rebuilds or refits native Metal acceleration structures, traces the edited camera into Godot textures, emits the complete guide set, and reconstructs the result through a native temporal-denoised MetalFX adapter. The animated silhouette/mirror automation produced nonzero distinct frames, one dynamic BLAS refit, finite secondary-hit-distance pixels, and no hidden material fallback under Metal API and GPU shader validation.

Implementation evidence materially refined several assumptions. Writable `R32Float` reversed depth was accepted directly by the SDK 27 denoised scaler, so the proposed depth-conversion pass is unnecessary on the reference configuration. The initial Godot native callback can execute before the Metal command buffer has otherwise been materialized, so the driver adapter must lazily create it. Native callback outputs must also resolve RenderingDevice pending clears before encoding, or a later lazy clear can overwrite correct native output. These are reusable callback/adapter constraints, not path-tracer-specific policy.

The accepted M1 material subset is intentionally narrower than the eventual StandardMaterial3D goal: scalar base color, a bounded approximate diffuse/specular metallic-roughness response, and scalar emission. Textures, alpha, normal mapping, transmission, arbitrary shaders, spot lights, and non-color environments remain explicitly diagnosed. Spot lights cannot be represented faithfully because schema 1 lacks their direction; treating them as point lights was falsified and rejected. Schema 1 also lacks jitter, so M1 editor reconstruction uses zero jitter while preserving the adapter field; adding jitter requires an explicit compatible capture revision.

Godot's current Metal RenderingDevice timestamp-query implementation returns zero rather than hardware GPU timestamps. The M1 adapter therefore rejects zero-duration results instead of presenting false measurements. Individual BLAS, TLAS, trace, and refit evidence remains measured by the standalone M0 Metal harness; adding Metal counter sampling to the integrated renderer is retained as productive follow-up work.

### 5.7 M2 outcome (2026-08-20)

The PC-independent foundation gate passed without committing proprietary asset data. A repeat Blender export preserved eight weights, all 35 correctives, and 141 deform bones in a byte-identical 29,536,204-byte GLB. Its source manifest records 70 driver definitions, a 60-entry body/hand retarget map, four-tile UDIM hashes, DirectX-to-OpenGL normal conversion, view-variant policy, and license status. Apparent missing suit textures were falsified: Blender stored `<UDIM>` template paths, while all expected physical tiles were present.

The deterministic pose/interaction scene activated all 35 smooth clamped morph drivers, kept filtered two-bone arm and 26-joint finger motion bounded through tracking loss, and validated a debounced tether gesture. The final combined morph/eight-weight deformation then passed through the actual Metal scene compiler, BLAS path, mirror secondary effect, guides, and MetalFX.

That scene exposed a flaw in the original integrated dynamic-BLAS evidence. The native unit changed its random sample at the same time as geometry, so output inequality did not isolate acceleration-structure correctness. In-place Metal refit was replaced by a distinct destination BLAS with explicit vertex-data refit options. The corrected native test holds its seed fixed and produces distinct raw traced radiance; the actual editor scene separately proves changed current-position capture data and a refit request. The cache now hashes current positions only, so previous-position motion history no longer causes false refits. M1's dynamic-refit claim should be read with this M2 correction.

The installed XROS 27.0 primary Swift interface also refined the companion shell. `FoveatedStreamingSession` supplies system discovery, capability authorization, connect/disconnect, and a framework-provided `ImmersiveSpace(foveatedStreaming:)` initializer. `FoveatedStreamingSpaceContent` is not directly constructible. The shell type-checks for visionOS 27 and its platform-neutral protocol/lifecycle tests pass, but entitlement, pairing, CloudXR, and device behavior remain M3 claims.

### 5.8 M3 preparation refinement (2026-08-20)

The current Godot Vulkan driver already contains capability-gated RenderingDevice primitives for BLAS/TLAS creation and build, ray-query feature reporting, RT-pipeline creation, shader-binding tables, ray dispatch, barriers, and timestamps. This falsifies the need for a separate NVIDIA-specific renderer foundation: the initial Vulkan backend should use Godot's existing abstractions, with proprietary NVIDIA functionality confined to optional adapters or measured scheduling paths. Local code also deliberately disables Vulkan ray tracing on macOS/iOS because the selected MoltenVK route cannot exercise it, so a Mac compile alone cannot certify the backend.

The M0 transfer bundle was stale after M1 established that a schema-1 scene packet omits mesh streams. Bundle schema 2 now contains a deterministic 1,404-byte self-contained capture with the unchanged two-view packet, one dynamic opaque triangle, three vertices, and three indices. It also contains paired layout/closure SPIR-V, protocol schemas, exact file hashes, fail-closed Windows build/test orchestration, and a renderer-report validator that requires both ray-query and RT-pipeline executions with separate GPU-stage timings. This completes productive Mac-side certification preparation only. The Vulkan backend/replay executable, Windows execution, parity, OpenXR, CloudXR, and physical Vision Pro evidence remain missing, so M3 has not passed and M4 remains gated.

## 6. Proposed system architecture

```text
Godot scene/resources/gameplay
          |
          v
Cross-platform PT scene compiler
  - instances, geometry, materials, lights, cameras
  - canonical GPU ABI and material closure IR
  - dynamic deformation schedule and dirty tracking
          |
          +------------------------------+
          |                              |
          v                              v
Vulkan/RTX backend                  Native Metal backend
ray query or RT pipeline            compute intersector/query
BLAS/TLAS build/refit                MTL acceleration structures
          |                              |
          +---------------+--------------+
                          v
Common path-tracing output contract
radiance, depth, motion, normals, albedos,
roughness, masks, hit distance, exposure
          |
          +------------------------------+
          |                              |
          v                              v
DLSS RR/SR adapter                 MetalFX denoised scaler
          |                              |
          +---------------+--------------+
                          v
Editor viewport / desktop / XR stereo layers
                          |
                          v
Windows OpenXR -> CloudXR -> visionOS Foveated Streaming
```

Other subsystems consume or feed this spine:

- the character-preparation pipeline produces clean meshes, a canonical skeleton, morph targets, material textures, and LODs;
- OpenXR/Vision Pro supplies head, wrist, and finger poses;
- iPhone body tracking later supplies timestamped torso and leg observations;
- the pose solver produces final bones and morph weights before GPU deformation and BLAS update;
- remote deployment builds and launches the Windows endpoint;
- the companion app manages discovery, authentication, streaming, UI, and diagnostics.

## 7. Renderer architecture hypothesis

### 7.1 Shared semantic core, native execution backends

**Hypothesis:** a portability-first wavefront/compute path tracer is the best initial common basis. Shared C++ code compiles Godot scene state into a canonical GPU ABI and a limited material-closure representation. Vulkan uses `rayQueryEXT` or a Vulkan RT-pipeline fast path; Metal uses native Metal Shading Language compute kernels and intersection queries. Both emit the same guide buffers.

This avoids forcing Metal into a Vulkan-style raygen/miss/hit abstraction that it does not naturally share. It also minimizes the semantic drift that would arise from two independent renderers.

The shader-sharing strategy is intentionally undecided. A Slang spike may be worthwhile, but a Metal target must be treated as experimental until it compiles the required closures, bindless patterns, ray queries, debugging information, and performance-critical code on the selected Apple SDK. The safe baseline is a shared data/algorithm specification with paired GLSL/SPIR-V and MSL implementations plus golden-image tests.

**Falsification test:** first implement the Cornell-box and material-grid kernels on Metal with two diffuse/specular bounces, emissive geometry, environment sampling, motion vectors, and one dynamic BLAS. Freeze the canonical packets, seeds, invariants, captures, and timing schema. Prepare the paired Vulkan implementation without choosing between ray query and RT pipeline. When the RTX 5080 is available, replay the frozen corpus and compare authoring cost, GPU time, shader iteration, and output divergence. The architecture remains provisional until that comparison runs.

**Fallback:** retain the shared scene compiler and output contract but use an NVIDIA-optimized Vulkan ray pipeline/SER integrator and a separate MSL compute integrator. Parity remains enforced at the feature and validation levels.

### 7.2 Rendering modes

The renderer should expose four modes, all using the same renderer-owned scene representation. The M1 editor panel is only a reference harness: its `Interactive` label means low-sample preview, not an in-game rendering method, and must not be presented as runtime or WYSIWYG support.

- **Interactive:** one or a few samples per pixel, aggressive adaptive resolution, denoising, and upscaling.
- **Progressive reference:** accumulated samples with deterministic seeds, no temporal upscaler required, used for editor look development and correctness.
- **Hybrid interactive:** rasterized or visibility-buffer primary visibility plus selected ray-traced shadows, reflections, transmission, ambient occlusion, direct/indirect lighting, or limited secondary bounces. Effects are selected by measured cost and perceptual value, not by a fixed promise that every pixel follows a full path.
- **XR interactive:** per-eye interactive rendering with strict latency and history rules.

Raster Forward+/Mobile remains available as a fallback and for the future native visionOS build. Path tracing should be a rendering method or renderer feature with explicit capability checks, not gameplay-specific code.

The first production-facing implementation is **Hybrid interactive**. It must execute inside the normal viewport/game frame graph, reuse Forward+ depth, motion, material, and primary-visibility work, and add capability-selected ray-traced effects before temporal reconstruction and composition. The editor camera and a running game camera must invoke the same renderer path and settings; a separate panel rendering a separately extracted scene is useful for reference comparison only. Scene data must be renderer-owned and updated through dirty propagation rather than traversing and serializing the entire scene tree every frame.

The production allocation is now explicit: Forward+ owns primary visibility and the direct-light BRDF, while ray tracing owns visibility for shadows, diffuse and glossy indirect transport, emissive/environment transport, reflection/refraction visibility, and the occlusion data applied only to indirect terms. A ray result must enter the lighting equation at the term it represents. In particular, shadow visibility replaces a light's raster shadow factor; ambient occlusion modulates ambient/indirect energy; and reflection/GI radiance is composed through material-aware weights. A single post-process multiplier over final raster color is rejected because it double-shadows direct light and incorrectly darkens emission and unrelated indirect energy.

The provisional Full Hybrid Metal path refines that boundary with a bounded world-space primary-diffuse contact-visibility estimator. Two to four stable low-discrepancy cosine-hemisphere rays test a configurable authored horizon (`4` rays and `1.2 m` by default). Hits retain full occlusion response through the near 75% of that horizon and fade smoothly to neutral over the final 25%; unoccluded surfaces evaluate to visibility `1`. This visibility may modulate only the primary surface's diffuse environment and diffuse-indirect terms. It must never modify sampled HDR direction, PDF, or exact ray visibility, nor multiply punctual direct light, emission, glossy/reflection transport, visible Sky, raster base color, guides, or final composition. It is a short-range grounding estimator, not another GI bounce, full path tracing, a substitute for finite-source shadows, or evidence that indirect transport is complete. It is fused at the existing internal ray-effects resolution and reconstructed by the existing per-view temporal path; no final-space scalar AO or arbitrary visual blur is allowed. The earlier Forward+ SSAO seam was removed after its nearly flat final visibility could not meet the fixed contact gate, avoiding double application. Current quality/cost evidence is provisional Metal only; corresponding Vulkan semantics, independent stereo execution, production-scene cost, and the RTX 5080 true-stereo gate remain open.

Flow's thin-G-buffer structure is the starting comparison, not the quality ceiling. Godot's target goes further by requiring all supported directional, omni, spot, and area-light shadow families; explicit secondary-hit geometry/material evaluation; emissive and environment sampling; separate diffuse/specular radiance and hit-distance guides; motion/disocclusion-valid per-view histories; adaptive sample allocation; and truthful per-effect fallback. Screen-projected raster radiance is permitted only as a labeled provisional visible-hit approximation. It cannot satisfy material parity or the M2.5 gate.

The RTX 5080 streamed target is sustained true-stereo 90 Hz. Full path tracing is the quality reference, not the only permitted real-time execution strategy. Hybrid interactive rendering is a first-class architecture because a stable 90 Hz result with correct motion, stereo, materials, and secondary effects is more valuable than an intermittently full-path-traced result. The UI and diagnostics must report which effects are rasterized, ray traced, reconstructed, or omitted; “Path Traced” must never silently select a hybrid path.

Performance work should evaluate the following capability-gated families in one dedicated milestone:

- raster or visibility-buffer primary hits with ray-traced secondary effects;
- dynamic selection of full, hybrid, and progressive modes by quality preset;
- adaptive sampling, bounded path depth, Russian roulette, light importance sampling, and candidate reservoir-based direct/indirect lighting;
- ray-query versus RT-pipeline execution, wavefront compaction, material/ray sorting, specialization, and vendor scheduling features only when exposed and measured;
- shared view-independent stereo work while preserving distinct per-eye cameras, visibility, guides, jitter, and temporal histories;
- static BLAS compaction, update-versus-rebuild policy, geometry/instance LOD, culling, batching, and capability-gated opacity/displacement acceleration;
- bindless resource access, descriptor reuse, pipeline/shader caches, compact records, texture residency/compression, transient-resource aliasing, and bounded memory growth;
- incremental scene extraction and dirty propagation, parallel CPU jobs, command recording/submission cost, GPU-driven work generation, shader warmup, and hitch-free cache population;
- measured reduced-precision storage/arithmetic where it preserves the reference tolerances;
- async compute and safe overlap among deformation, AS work, tracing, reconstruction, composition, and encode preparation;
- dynamic internal resolution and reconstruction, including DLSS RR/SR when available, without frame generation in the initial VR path;
- stable guide generation, selective native-resolution shading for adversarial detail, and explicit history resets; and
- content budgets for animated geometry, lights, transparent layers, material complexity, and city residency.

Every optimization needs an isolated A/B capture against the same packet, seed, pose, camera path, resolution, and quality metric. Optimizations that change semantics must be exposed as a named quality-mode choice. An optimization is rejected if it meets timing by causing stereo mismatch, stale geometry, temporal instability, material loss, or an undocumented resolution/quality reduction.

### 7.3 Scene extraction and canonical GPU ABI

The cross-platform scene compiler should own:

- stable instance and material IDs;
- mesh streams and index formats;
- previous/current transforms and deformed positions;
- light records and sampling distributions;
- environment importance sampling;
- material parameters and texture handles;
- camera/view records, including per-eye previous/current matrices;
- acceleration-structure dirty flags;
- ray visibility categories;
- guide-buffer definitions and units; and
- deterministic debug capture.

The ABI must specify alignment, handedness, matrix convention, UV convention, normal-map convention, color space, motion-vector direction and units, depth convention, roughness convention, and invalid-pixel sentinels. Both backends should deserialize the same captured scene packet in an offline validation harness.

The M0 candidate ABI adopts 16-byte record alignment, little-endian packet storage, right-handed coordinates with +Y up and -Z camera forward, column-major matrices, top-left UV origin, reversed `[0, 1]` device depth, perceptual roughness in `[0, 1]`, and quiet NaN for invalid floating-point guide pixels. Canonical motion is previous UV minus current UV in normalized input-texture units. This matches Godot's existing Forward+ motion-vector shader and converts to MetalFX by multiplying by input width and height. The candidate is versioned independently and validated as a deterministic captured packet under `misc/path_tracing/m0`. MSL and GLSL/SPIR-V layout reflection, CPU decoding, corruption tests, and golden-packet checks can complete on macOS; Vulkan GPU replay remains the later certification gate.

M1 implementation evidence refined the meaning of “packet.” Schema 1 is a deterministic per-frame scene packet: it carries cameras, instances, material parameters, lights, guide semantics, and stable geometry references, but not mesh streams. Therefore it is not a self-contained arbitrary-scene replay artifact. A separately versioned capture container wraps the unchanged schema-1 packet with canonical geometry records, current and previous deformed vertices, tangents/UVs, 32-bit triangle indices, bounds, flags, and its own payload hash. Both backends must consume that same complete capture during validation. Procedural M0 replay remains valid, but a schema-1 packet alone must no longer be described as a full Godot scene capture. Within schema 1, `InstanceRecord.material_id` is a one-based material-table index and zero means no material; this resolves the original ID ambiguity without changing the frozen wire layout.

### 7.4 Material scope

Start with a deliberately bounded closure set:

- diffuse and metallic/roughness microfacet reflection;
- normal and height-derived detail;
- emissive surfaces;
- alpha cutout and controlled alpha blending;
- dielectric transmission with a clear nested-medium limitation;
- clear coat and anisotropy only after the base contract is stable; and
- texture arrays/atlases suitable for the BND suit and city materials.

Godot visual shaders and arbitrary shader code cannot automatically become physically valid ray-hit shaders. The first implementation should translate supported `StandardMaterial3D` features and a documented custom material closure API. Unsupported raster materials must show an editor diagnostic and use a visible fallback, never silently render incorrectly.

For M1, “initial subset” means the smaller scalar diffuse/specular/emissive closure recorded above. It is not yet the full microfacet, textured, normal-mapped, alpha, or transmission set described as the direction for later milestones. Its matching MSL and Vulkan GLSL sources compile in the deterministic M1 gate, but only the Metal implementation has runtime evidence.

### 7.5 Dynamic geometry and acceleration structures

The mandatory frame order is:

1. Resolve tracking and animation.
2. Solve IK and twist distribution.
3. Evaluate pose-space wrinkle weights.
4. Run eight-weight skinning and blend-shape deformation.
5. Publish current and previous deformed vertex positions.
6. Refit/update eligible BLAS objects; rebuild when topology or update constraints require it.
7. Update TLAS instances.
8. Trace rays and generate guides.
9. Denoise/upscale and composite overlays.

The implementation needs separate metrics for deformation, BLAS refit, BLAS rebuild, TLAS update, tracing, denoising, and composition. A renderer that deforms the raster mesh but traces against an old BLAS is incorrect even if the error is subtle.

Static city geometry should use compact, non-updateable BLAS objects and batching chosen through measurement. Character and other deforming geometry should use updateable BLAS objects. Streaming must use deferred destruction and GPU fences so unloaded geometry cannot be referenced by in-flight frames.

### 7.6 Stereo and multiview

Stereo is a renderer-wide data model, not a final copy operation. Each eye needs:

- current and previous projection/view transforms;
- camera-relative origin where needed for city-scale precision;
- output radiance and guide layers;
- motion and disocclusion history;
- independent jitter sequence/state;
- independent denoiser/upscaler instance or history;
- correct compositor slice and depth submission; and
- a coordinated history reset on tracking discontinuity, resolution change, recenter, teleport, or scene cut.

The default architecture shares geometry, materials, BLAS, TLAS, and light distributions between eyes. It dispatches the view dimension either as texture-array layers or as two explicit launches. The MetalFX adapter should follow Flow’s proven per-slice 2D-view technique until Apple exposes an equally suitable layered interface.

### 7.7 First-person ray visibility

Define at least these masks:

- primary/eye visibility;
- shadow visibility;
- reflection/refraction visibility;
- diffuse-indirect visibility; and
- optional editor/debug visibility.

The first-person head and interior faces can then be excluded from eye rays while remaining visible where physically useful. Author separate first- and third-person assets initially because mask semantics, reflection composition, and near-plane behavior need deliberate testing.

### 7.8 City-scale light sampling and indirect transport

**Primary hypothesis.** ReSTIR DI, initialized from a camera-centered ReGIR structure and an environment importance distribution, is the first many-light direct-illumination candidate for the city profile. This is a hypothesis, not an SDK commitment. The [ReSTIR DI paper](https://cs.dartmouth.edu/~wjarosz/publications/bitterli20spatiotemporal.html) establishes reservoir-based temporal/spatial reuse for many-light direct illumination; the [ReGIR chapter](https://research.nvidia.com/labs/rtr/publication/boksansky2021rendering/) provides a world-space grid-based proposal. RTXDI 3.0's [integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md) independently documents distinct local and environment presampling distributions and current/previous light-index mappings. Camera-centering ReGIR for this renderer is our inference from those ingredients and must be measured against fixed and other world-space placements.

ReSTIR GI, ReSTIR PT/GRIS, and a shared world-space radiance cache are competing or complementary indirect-transport hypotheses, not promised additions. The [ReSTIR GI publication](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing) and [RTXDI GI guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md) support evaluating surface-sample reuse; [GRIS/ReSTIR PT](https://research.nvidia.com/labs/rtr/publication/lin2022generalized/) and the [RTXDI PT guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirPT.md) motivate path-space reuse but also require explicit MIS, correlation, disocclusion, and bias evaluation. RTXDI's [v3.0.0 release](https://github.com/NVIDIA-RTX/RTXDI/releases/tag/v3.0.0) is evidence for an optional adapter investigation only. Its contexts, bridge functions, and types must never become the Godot renderer ABI or a required dependency.

The backend-neutral boundary is a versioned unified-light record and distribution contract: analytic lights, emissive-instance/triangle samples, and environment samples carry stable 64-bit source/sample identities; compact current and previous indices are derived mappings, never identities. A source ID may intentionally cover multiple emissive triangles, while every selectable sample ID is non-zero and unique. Inputs with zero or duplicate sample identities fail visibly. Non-finite or negative sampling weights are retained as diagnosed zero-weight records, rather than being allowed to poison a distribution. Local/emissive and environment distributions remain separate, have deterministic ordering/ties, explicit all-zero behavior, replay checksums, and CPU validation before a backend uploads them. A vendor adapter may translate this semantic input to its own light buffer, PDF texture, or ReGIR resources, but must return equivalent diagnostics and retain no vendor type above the adapter boundary.

Scene AS, unified light records/distributions, camera-centered ReGIR data when it exists, environment preprocessing, and a world-space radiance cache may be shared only when their camera/visibility assumptions permit it. Every eye owns its screen-space DI/GI/PT reservoir arrays, selected visibility, motion/disocclusion validation, jitter, reconstruction inputs, and temporal histories. Cross-eye reservoir/sample reuse is experimental and prohibited by default until a stereo-occlusion fixture demonstrates correct left/right parallax and independent invalidation.

The staged order is deliberately small: (1) deterministic CPU records, previous/current identity mapping, distributions, diagnostics, and a portable reservoir ABI; (2) a deterministic thousands-of-lights fixture plus per-view reservoir ownership tests; (3) paired Metal/Vulkan upload and replay validation; (4) direct-only ReSTIR DI initial/temporal/spatial prototypes; (5) measured camera-centered ReGIR A/B; then (6) separate ReSTIR GI, ReSTIR PT, and radiance-cache experiments. No stage may silently replace the current estimator merely because the data prerequisite exists.

Each candidate must pass correctness (finite PDFs/weights, identity continuity, all-zero behavior, temporal/disocclusion and stereo-occlusion fixtures), quality (fixed-seed progressive-reference error plus temporal stability), memory (separate scene/distribution/ReGIR/cache/per-eye reservoir accounting), and GPU-stage timing (light extraction/distribution build, presampling, ReGIR update, initial candidates, temporal reuse, spatial reuse, visibility, indirect transport, reconstruction). The Mac implementation is provisional until it has a corresponding Vulkan/RTX replay. Missing Windows RTX hardware blocks backend parity/certification and the 90 Hz decision, not these deterministic prerequisites; it does not authorize lowering the true-stereo RTX 5080 90 Hz target.

The first environment adapter is a separate M0 prerequisite, not proof that the generic environment domain above is a directional texel CDF or a ReSTIR/ReGIR implementation. The provisional Metal implementation is enabled only by `rendering/hybrid_renderer/environment_lighting/enabled` (default `false`, restart required) in Full Hybrid mode. It consumes Godot's active renderer-owned sharp octahedral Sky radiance, preserving border/layout and the radiance generation's existing linear energy/exposure boundary. Explicit environment transport requires a finite RGBA32F sharp resource: when the setting is enabled at startup, SkyRD renders a separate single-layer, single-mip RGBA32F sharp octmap while leaving the filtered raster reflection array in its existing format. The adapter fails closed rather than silently consuming RGBA16F. A monotonic content generation advances only when both sharp mip-0 representations are rerendered. The cache identity combines the Sky source, actual full-float sharp resource, content generation, dimensions, border, and layout; pure orientation changes retain the radiance-space distribution but update the world/radiance transform and invalidate each render buffer's temporal history.

The Metal adapter builds an R32-float GPU importance pyramid without a CPU distribution rebuild. It weights finite nonnegative luminance by the octahedral texel solid angle/Jacobian, excludes border padding, pads non-power-of-two source extents to a power-of-two reduction domain with zero-weight texels, and descends that hierarchy for a solid-angle PDF. Selection, PDF evaluation, direct samples, and misses all use the same RGBA32F sharp radiance. Primary diffuse environment NEE has a visibility ray and uses the balance weight only while the paired GI cosine/BSDF proposal is active; the BSDF environment miss uses the complementary weight. With GI disabled, primary NEE retains full weight. Secondary diffuse and reflection hits receive full-weight environment NEE because this provisional path has no paired continuation estimator at those hits, while diffuse and glossy misses read the same sharp radiance/orientation. It synthesizes no directional light. Hybrid environment transport fails closed unless raster ambient and reflected-light sources are disabled, preventing silent double count while allowing the Sky background to remain visible. View-independent radiance-space weights are cached once; view parameters, visibility samples, jitter, histories, and reconstruction remain independent.

Diagnostics expose `active`/`fallback`/`unsupported`, `rebuilt`/`reused`, source ID, generation, deterministic metadata checksum, and the reason. A small shared Metal diagnostic buffer is written only when the distribution rebuilds and read once after command completion; static reuse performs no source readback. It reports post-Sky non-finite texel count, finite peak RGB/luminance, total and maximum texel importance weight, and top probability. Non-finite source values still receive zero weight. Capture labels are `environment_importance_build`, `environment_importance_diagnostic`, and `environment_sampling`, but build/diagnostic and sampling still lack isolated counter timings; sampling remains in combined `ray_effects` rather than receiving a fabricated number. At 2560 square, the extra RGBA32F sharp octmap is 104,857,600 bytes per allocated Sky, excluding driver overhead, while the existing raster/filter storage is unchanged. This correctness-first cost and the extra sharp render pass must be measured before production adoption.

The precision policy was forced by a concrete falsification. The finite source asset with SHA-256 `62cdbdcc2c427a68feb08bce1ca572759d8847305bacab3a2112bb88b914f3a5` contains a compact approximately `0.47°` sun and finite red values near `127,975`. Godot import preserved RGB32F exactly, but the former RGBA16F Sky octmap converted red values above `65,504` to `+Inf`; the importance builder correctly rejected those non-finite texels and thereby removed the compact sun core from the distribution. A post-correction M4 Max/Metal 4 rebuild reported zero non-finite texels, finite peak RGB `(126263.01, 44989.97, 3995.00)`, peak luminance `59308.78`, total importance `14.49495`, maximum texel weight `0.190485`, and top probability `0.0131415`. This proves post-Sky finite preservation and selectable compact-source probability on this Metal configuration; it does not yet prove the final shadow-width, energy, or performance gates. The next M0 comparison must measure this source against constant, broad, and compact controls, verify rotation/no-double-count behavior, and record rebuild plus combined ray timing without inventing an isolated stage result. Vulkan/RTX must implement and validate the same finite-radiance semantic contract before parity can be claimed.

### 7.9 Dynamic-atmosphere dominant-radiance-lobe contract (2026-08-22)

**Hypothesis.** A procedural Sky may author a deterministic visible solar/lunar radiance field without creating a scene `DirectionalLight3D`. The renderer-facing handoff is a product-neutral, renderer-owned *dominant radiance lobe proposal* plus an optional world-space cloud-transmittance-field reference. The visible Sky remains the sole radiance authority for backgrounds, misses, reflections, diffuse environment transport, and environment importance. The proposal records a stable identity and content generation, direction, finite angular support, integrated radiance/irradiance relationship, and optional cloud field; it does not add a second solar term.

The selected first executable contract is an explicit radiance partition, not an added light: `L_full(omega) = L_residual(omega) + sum(L_lobe(omega))`. Visible background, primary Sky misses, and full-delta reflection paths consume `L_full`. Diffuse and rough-glossy paths that also evaluate the explicit finite lobe consume `L_residual`; paths that do not evaluate that lobe continue to consume `L_full`. The lobe evaluator owns its direct contribution, geometry visibility, and cloud transmittance exactly once. Sampling the unchanged Sky and the explicit lobe remains only a fallback hypothesis and would require evaluated mixture PDFs/MIS against the same radiance plus an equal-power validation before adoption.

The state contract separates content evolution from temporal discontinuity. `state_generation`, `radiance_generation`, `partition_generation`, and `cloud_content_generation` identify their respective current/previous content. Continuous clock and cloud motion advance the applicable generations without inherently advancing `history_epoch`; scrub, seed/weather replacement, layer topology change, or lobe-profile replacement advance the epoch. Every eye retains independent histories and compares the same discontinuity epoch. The current provisional Forward+ environment key still includes Sky radiance generation, so it would reset reconstruction on every realtime radiance update; renderer wiring must replace that coupling before dynamic atmosphere can be called history-correct.

**Evidence.** Current Sky materials feed renderer-owned Sky radiance and mark it dirty through `material_set_param`, but SkyRD and Forward+ direct/shadow paths consume scene directional lights; no existing Sky-owned direct-light seam exists. A material-only sun can therefore render coherently into radiance/environment transport but cannot yet cast raster or world shadows.

**Falsification and fallback.** A direct-light implementation must prove, at deterministic clear/partial/overcast states, background/reflection/direct-shadow alignment, finite-source penumbra, cloud attenuation, and no-double-count energy against an equal-power residual/mixture reference. Residual construction and profile matching fail closed on non-finite input or oversubtraction; no direct lobe is exposed until the renderer can generate and route the matching residual. If no bounded residual or mixture-PDF contract can pass that test, retain procedural atmosphere as environment-only and require explicit owner approval for a renderer-light ABI. The first atmosphere slice is intentionally non-volumetric and leaves direct-light/cloud-shadow wiring as the next gate. Its direction-space shader clouds are visible-radiance prototypes only: they are not a world-space cloud-shadow field and cannot substantiate world-shadow or solar-blocking claims.

The first CPU-only semantic seam is `servers/rendering/sky_lighting.{h,cpp}`. It fixes the receiver-to-source direction, angular-radius, perpendicular-irradiance, uniform-disk normalization, stable-identity, generation/epoch, bounded planar optical-depth, and fail-closed partition rules without exposing a scene light, renderer-server ABI, GPU layout, or backend type. This is contract and deterministic-test evidence only; it changes no rendered behavior.

**Metal implementation update (2026-08-22).** Full Hybrid now implements the first bounded renderer-owned path for opt-in Sky shaders. `AT_HYBRID_RESIDUAL_PASS` renders a second RGBA32F sharp octmap with only the exact uniform solar disk removed; the ordinary sharp octmap remains `L_full` for raster background and true-delta reflection misses. The transport/importance texture is `L_residual` only when the generic named material contract validates; otherwise it remains the unchanged full sharp texture and the explicit lobe is zero. The state passed through Forward+ and Metal carries the actual Sky-RID-derived source/sample lineage, current and previous world directions, radius, pre-attenuated perpendicular irradiance, cloud scalar, profile/partition/state/history generations, and separate full/residual resource identities.

The atmosphere’s `sun_disk_energy` now means integrated perpendicular irradiance multiplier rather than disk radiance scale. CPU and visible Sky use the same `E_perp = sun_color * sun_disk_energy * sun_visibility * source_cloud_transmittance * exposure`; the disk has hard support and radiance `E_perp / (PI * sin(radius)^2)`. Metal takes one uniform-solid-angle lobe sample per primary opaque surface (Hammersley dimensions 17 and 18), traces its TLAS visibility, and adds the Lambert direct term outside the bounded contact-visibility multiplier. Clouds are therefore applied exactly once. No `Light3D`, directional shadow-only path, lunar direct term, fabricated projected shadow, or Vulkan implementation is involved.

At the demo's 640x640 sharp-octmap allocation, each RGBA32F resource is 6,553,600 bytes; the residual is the additional allocation. The installed arm64 Metal editor ran matched 112-frame captures on Apple M4 Max / Metal 4 / Forward+ Full Hybrid at 1152x648 under `/tmp/dynamic_sky_lobe_final`, with zero `Light3D` nodes. A row-adaptive receiver-floor mask compares each condition to its `energy=0` pair, uses the far-side unoccluded receiver `p90` as direct reference, selects responses below 45%, and weights the remaining receiver deficit. Clear 17.35 enabled versus energy-zero measured direct-luma `p90=0.15020`, selected-shadow response median `0.01006`, and mean receiver deficit `0.13324`. The weighted receiver-shadow centroid moved from `(531.20, 582.88)` at 16.00 to `(632.76, 591.82)` at 17.35, a `101.96 px` displacement. Clear/0.25/0.65 cloud direct `p90` was `0.15020/0.11666/0.06578` (ratios `0.777/0.438`). Equal-energy 4° to 8° disk diameter increased normalized transition-band pixels `22,876` to `30,116` (`+31.6%`) at equal `p90=0.15020`; visible-disk centroid was `(497.11, 90.44)` and the respective disk boxes were `45x48` and `93x99` pixels. These are display-space RGBA8, one-view acceptance measurements, not isolated GPU-stage, performance, backend, or stereo certification. The existing per-render-buffer history key still includes radiance generation, which is correct but may reset too often during continuous clock updates. Vulkan/RTX parity, stereo validation, isolated stage timing, and the 90 Hz target remain open.

## 8. Denoising and upscaling

### 8.1 Common guide contract

Both vendor adapters should consume semantically equivalent inputs:

- noisy linear-HDR radiance/color;
- view-space or device depth with documented conversion;
- motion vectors with documented direction and scale;
- shading/geometric normal as required;
- diffuse albedo;
- specular albedo/F0;
- roughness;
- denoise-strength mask;
- reactive mask;
- optional specular hit distance;
- jitter offset;
- exposure or exposure mode; and
- explicit history-reset flag.

The canonical contract must additionally carry current and previous view/projection transforms, the depth direction, and adapter-enable intent for every optional guide. The MetalFX adapter converts those transforms to world-to-view and view-to-clip matrices, declares reversed depth explicitly, and converts canonical motion into MetalFX's documented current-to-previous pixel-space convention. On SDKs that expose optional-guide enable flags, setting a format or binding a texture without enabling the guide is invalid adapter behavior. Transparent overlays are represented separately from the reactive mask because MetalFX assigns them different semantics.

The guide validator should visualize each buffer, detect NaN/Inf, report out-of-range values, and validate texture device, dimensions, array layer, usage flags, and lifetime.

### 8.2 macOS: MetalFX temporal denoised upscaling

**Required hypothesis:** on Apple-family-9-or-newer hardware, `MTLFXTemporalDenoisedScaler` can turn a low-sample path-traced input into a stable, useful editor image at an interactive rate.

Implement it directly in the Metal rendering path, borrowing Flow’s resource and synchronization patterns:

- create descriptors from actual input/output formats and dimensions;
- provide depth, motion, normal, diffuse/specular albedo, roughness, denoise and reactive masks, and specular hit distance when useful;
- keep input/output resources on the same `MTLDevice`;
- set motion-vector scale and jitter consistently with the renderer ABI;
- encode on the path-tracing command-buffer timeline;
- cache scaler/resources by input size, output size, format, view count, and device;
- create per-eye 2D views over layered XR textures and execute once per eye;
- reset history on any semantic discontinuity; and
- fall back cleanly if the device lacks denoised-upscaler support.

MetalFX temporal denoised upscaling requires Apple family 9 according to Apple’s current feature tables, making M3/A17 Pro-class hardware the practical minimum for this exact mode. Older ray-tracing-capable Macs may run progressive rendering or a common fallback denoiser, but must not be described as providing the same MetalFX feature.

On the audited M4 Max and SDK 27 combination, the descriptor accepted the canonical writable `R32Float` reversed-depth guide directly. The adapter should keep capability/format validation, but should not pay for a conversion pass unless another supported device or SDK falsifies direct use.

The first spike must test disocclusion, thin web lines, glossy lens reflections, fast hand motion, swinging camera rotation, subpixel raised webbing, and blend-shape motion. A clean static Cornell box is insufficient.

### 8.3 Windows: DLSS and portable fallback

Use a replaceable adapter for DLSS Ray Reconstruction and/or Super Resolution, conditioned on SDK availability and license. The renderer must not expose NVIDIA SDK types above the adapter boundary. An SVGF/à-trous-style fallback is desirable for debugging, unsupported hardware, and cross-backend diagnosis; it need not match vendor quality.

Frame generation is excluded from the initial VR path. It may be investigated separately only with end-to-end motion-to-photon measurement and artifact tests.

## 9. CloudXR and the visionOS companion

### 9.1 Product split

The recommended streaming client is a small native SwiftUI/RealityKit visionOS application. It is not a Godot export. Its responsibilities are:

- discover or accept a Windows endpoint;
- perform Apple’s pairing/authentication flow;
- own the `FoveatedStreamingSession` lifecycle;
- present streamed immersive content;
- show connection, latency, bitrate, packet-loss, audio, and tracking diagnostics;
- manage reconnect and user-friendly error recovery; and
- provide minimal native overlays/settings without obscuring the stream.

The future standalone visionOS simulator is a separate Godot Mobile-renderer target with separate assets and capability constraints.

### 9.2 Native Apple integration

On visionOS 26.4 and later, Apple’s Foveated Streaming framework is built into the OS and integrates with CloudXR. The client uses the Foveated Streaming entitlement and immersive-space API; a physical Apple Vision Pro is required for meaningful testing. System discovery uses Apple’s endpoint/session protocol rather than merely connecting a generic CloudXR client socket.

The Windows side should derive from Apple’s `StreamingSession` reference architecture:

- advertise `_apple-foveated-streaming._tcp` with Bonjour/mDNS;
- exchange the prescribed TCP JSON messages;
- present QR/certificate pairing when requested;
- start and supervise the CloudXR service/runtime;
- tell the client when media is ready; and
- launch or attach to the Godot OpenXR application at the correct lifecycle point.

This launcher/service is important because an OpenXR app can ask for `xrGetSystem` before a remote client exists. CloudXR may report `XR_ERROR_FORM_FACTOR_UNAVAILABLE`; Godot’s normal startup path treats that as fatal and destroys the instance. The endpoint manager should establish the streaming session first and launch Godot only when the runtime can provide a system. A fixed development profile may be supported, but should not hide production lifecycle failures.

### 9.3 Tracking and input

CloudXR forwards the standard twenty-six-joint `XR_EXT_hand_tracking` representation, which aligns with Godot’s existing hand tracker. That is the initial hand-input contract. Gesture recognition should be derived from joint positions, orientations, velocities, and confidence, with debouncing and state machines for web-shoot/release/grab.

Server-derived `XR_EXT_hand_interaction` should be optional because Apple’s Foveated Streaming availability begins later than the base 26.4 streaming capability. It can improve interaction on visionOS 27+, but cannot be the only gesture path.

Godot does not currently expose CloudXR’s opaque data channel through `XR_NV_opaque_data_channel`. Add it only when a concrete gameplay/platform payload requires it, such as companion diagnostics or later auxiliary tracking. Version the payload protocol independently of product versions.

### 9.4 Foveation semantics

Apple’s system uses approximate gaze information to allocate stream quality, but the application does not receive the focus region. Therefore:

- rely on it for encoder bandwidth efficiency;
- do not plan eye-directed sample counts in the path tracer from this API;
- use dynamic internal resolution, denoising, and upscaling for render cost; and
- keep gaze-dependent gameplay out of scope unless a different permitted API supplies it.

### 9.5 Depth, alpha, audio, and security

Depth and alpha should be enabled and tested early because they influence composition and comfort, but exact capabilities must be negotiated at runtime. The companion must degrade explicitly when they are unavailable.

Upstream microphone audio is available in recent CloudXR Runtime releases on Windows and requires NVIDIA’s virtual audio driver; Apple’s Foveated Streaming support begins on visionOS 27. Treat microphone passthrough as a later milestone with permission UX, device selection, mute state, echo behavior, and privacy indicators.

CloudXR security properties must be verified against the chosen release. Do not assume media/input encryption from secure signaling. Use a trusted LAN during development and a VPN or otherwise documented trusted network for remote use. Never expose the endpoint broadly without threat modeling, authenticated pairing, rate limits, and update procedures.

## 10. Character, IK, and suit-deformation pipeline

### 10.1 Source assessment

The BND model at `/Users/ethan/Downloads/BND MODEL/` is an excellent cinematic source but not a runtime asset. The inspected Blender 4.2 scene contains approximately:

- 258 objects, including 252 meshes and one armature;
- 774 bones, of which 141 are deform bones;
- 730 pose constraints, 23 materials, 27 images, and 11 actions;
- a 56,640-vertex/113,152-triangle main body;
- Basis plus 35 pose-driven cloth/wrinkle shape keys;
- about 364,000 hero triangles without the web shooter and 521,000 with it; and
- roughly 1.55 million source vertices when hidden corrective sources, widgets, and helpers are counted.

The Blender scene uses IK/FK controls, corrective/tweak bones, finger controls, lens expressions, raised webbing, and web-shooter controls. Direct import would pull in authoring-only content and lose important Blender driver behavior.

### 10.2 Dedicated preparation/export pipeline

Create a deterministic Blender-side export process that produces a sanitized runtime collection:

- only approved render meshes and LODs;
- one standardized runtime skeleton;
- baked skin weights with eight influences where supported;
- named shape keys and a separate driver-definition data file;
- baked raised webbing/logo/sole geometry where silhouette matters;
- normal/height detail where geometry does not justify its cost;
- converted material textures and explicit color-space metadata;
- first-person and third-person variants;
- collision/physics proxies; and
- a manifest containing source revision, export settings, units, axes, hashes, and license notes.

Do not execute untrusted embedded Blender scripts during automated import. Keep the original `.blend` files outside normal Godot import scanning if possible.

### 10.3 Skeleton and IK

The source names such as `DEF_FINGER_A_01.L` do not match OpenXR joint conventions. Retarget through an explicit map into a clean canonical runtime skeleton. The first VR solver should use:

- XR camera pose for head/neck intent;
- tracked wrist and finger joints for hands;
- `XRHandModifier3D` or equivalent joint application;
- two-bone IK for forearms/upper arms;
- `BoneTwistDisperser` for forearm and upper-arm roll;
- clavicle/shoulder reach and limits;
- inferred chest, spine, pelvis, and legs driven by head/hands and locomotion; and
- later `XRBodyModifier3D` or a custom fused body tracker when iPhone data exists.

The solver should separate observed pose, filtered pose, anatomical constraints, and rendered pose. Tracking confidence must affect blending; a lost hand should transition to a predicted/rest pose without a single-frame snap.

### 10.4 Wrinkles

The 35 cloth morphs encode useful pose-space deformation. They affect thousands of vertices, mostly at millimeter scale, with some deviations near 4.5 cm. Blender drivers will not survive a normal glTF export, so Godot must reimplement their relationship to joint rotations.

For each corrective:

1. Record the source driver variables, coordinate spaces, ranges, and combination rule.
2. Convert them to canonical-skeleton joint angles or pose features.
3. Use smooth, clamped response curves with defined overlap behavior.
4. Evaluate weights after IK but before skin/morph GPU deformation.
5. Test a dense pose sweep for popping, double correction, self-intersection, and asymmetric errors.

Combine these silhouette/large-fold morphs with dynamic wrinkle normal maps and stable fabric microstructure. Full live cloth is reserved for loose components or later research. The ray tracer must consume the final deformed positions and update the BLAS so the wrinkles affect all ray types.

### 10.5 Skinning influence evidence

Preserve eight influences. Measured pruning of the main asset to four influences loses about 1.63% weight on average and more than 5% on 6,211 vertices. Eight-weight pruning loses roughly 0.0115% on average, with no vertex over 5%. Godot’s glTF importer already supports eight weights, and both new deformation kernels must do so as well.

### 10.6 Materials and geometry conversion

The asset’s Blender materials and four-tile UDIM sets will not map automatically to a standard Godot material. The source includes 4K base/roughness/metal/normal sets, 8K brick maps, and 4K displacement. Some dirt, grunge, scratch, and lens texture paths are broken. Source normal maps use the DirectX convention and require green-channel inversion for the chosen Godot/renderer convention.

Select among a packed atlas, texture arrays, virtual texturing, or a custom material only after measuring quality, residency, filtering across tile boundaries, and ray-hit lookup cost. For the first runtime asset, an offline bake to a controlled atlas/array is the lowest-risk hypothesis. Raised webbing, logo, and soles that depend on Surface Deform/Geometry Nodes must be baked to skinned meshes or selectively represented as height/normal detail.

### 10.7 LOD and view variants

Produce at least:

- **hero close-up:** preserves the important silhouette geometry and wrinkle morphs;
- **streamed VR gameplay:** retains hands, lenses, torso, and near-body quality with measured simplification; and
- **native visionOS:** much lighter geometry, textures, morph set, shaders, and draw-call count.

The first-person variant should remove or hide the head and interior surfaces that can clip the eyes. Hands and arms may need separate near-field topology and texture density. The third-person/reflection variant preserves the full character.

### 10.8 Rights gate

The guide asks for credit but does not state an explicit commercial license. Do not infer commercial redistribution rights. Obtain a written asset license before distributing builds. Separately, Spider-Man names, designs, logos, and likenesses are Marvel/Sony intellectual property; public or commercial release requires an IP strategy independent of the model license. Until resolved, keep the project private or use an original substitute character for distributable tests.

## 11. iPhone full-body tracking hypothesis

Start with Apple’s native body-tracking facilities, not a custom model. An iPhone on a tripod can run ARKit body tracking where available or Vision 3D body-pose estimation, then send timestamped joint transforms, confidence, camera calibration, and session metadata directly to the Windows host.

The PC fuses:

- Vision Pro head pose;
- high-confidence Vision Pro hand/wrist joints;
- iPhone torso, pelvis, and leg observations;
- character proportions and joint limits; and
- locomotion/physics state.

The transport should be low-latency, ordered where necessary, tolerant of packet loss, clock-synchronized, and versioned. Send skeleton data, not full camera frames, by default. Provide a calibration pose that estimates floor, camera-to-play-space transform, body proportions, and latency offset.

A single camera will have occlusion, facing-direction, crouch, fast-leg, and floor-contact failures. The milestone succeeds only if confidence-aware fusion fails gracefully. Train or integrate a specialized model only after the native baseline produces an error dataset proving where it is inadequate.

## 12. Native visionOS and iOS paths

### 12.1 Native visionOS

The standalone target should use Godot’s Mobile renderer, the visionOS compositor path, and aggressively reduced city/character assets. Add an ARKit `HandTrackingProvider` bridge that populates Godot `XRHandTracker` state; do not depend on CloudXR for a native build.

Its product promise is portability and local interaction, not parity with the RTX visual target. Shared gameplay, tracking abstractions, asset manifests, and material intent should be reused; render budgets and implementations should differ deliberately.

### 12.2 iOS ray/path tracing

The native Metal backend should be structured so it can later compile on Apple-family-9-or-newer iPhone/iPad GPUs. This is an architectural affordance, not an early performance promise. A mobile experiment must measure thermal throttling, memory, battery, sustained frame time, and MetalFX behavior. Likely modes are lower resolution, fewer bounces, restricted materials/lights, progressive photo mode, or hybrid rendering rather than desktop-equivalent VR path tracing.

## 13. Editor and remote-deployment experience

### 13.1 macOS editor controls

Add an editor viewport control with:

- Raster / Path Traced Interactive / Path Traced Progressive;
- internal resolution or quality preset;
- sample and bounce limits;
- MetalFX enable/quality and fallback status;
- freeze accumulation;
- reset history;
- guide-buffer/debug view;
- acceleration-structure and GPU timing overlay;
- backend/capability report; and
- a prominent warning for unsupported materials or stale dynamic geometry.

Editor correctness is required before XR streaming. The viewport should render the actual scene and animation through the same scene compiler as runtime, not an offline proxy renderer.

### 13.2 Send to PC

Extend Godot’s existing remote Windows run feature into a preset-driven **Build & Run on VR PC** action:

1. Validate the selected export template, SDK/dependency availability, and endpoint reachability.
2. Increment only the appropriate build artifact identifier if the project’s policy requires it; do not couple unrelated version contracts.
3. Export a Windows build locally or invoke a documented remote build.
4. Transfer changed artifacts with SCP/rsync-like delta behavior where practical.
5. Start the Apple/CloudXR endpoint manager.
6. Pair or wait for Vision Pro.
7. Launch Godot after CloudXR reports an OpenXR system is available.
8. Forward debugger/log/metrics channels.
9. Stop the previous instance safely and preserve crash logs.

Credentials belong in Keychain/SSH configuration, not project files. The UI should show which commit, export preset, asset manifest, and renderer backend are running remotely.

## 14. Milestones and acceptance gates

Dates should be estimated only after M0 measurements. Every milestone ends with an evidence report and a go/change/stop decision.

Milestones M0 through M2.5 are intentionally executable without a Windows PC. They may establish a Mac reference implementation, portable product foundations, and a provisional Metal runtime/hybrid path, but they do not certify the renderer as cross-platform complete. M3 is the first PC-dependent milestone and the only place where Vulkan/RTX parity claims become eligible. Work after a failed gate may continue only when it is independent of the failed claim; dependent features remain provisional and must not be relabeled complete.

### Current implementation and verification status (2026-08-20)

“Implemented” means source and deterministic fixtures exist. “Verified” means the applicable acceptance gate ran on the recorded target. An implemented item awaiting another platform or physical device remains provisional; it is not counted as renderer parity or milestone completion beyond its explicitly stated scope.

| Scope | Implementation status | Verified evidence | Verification still required |
| --- | --- | --- | --- |
| M0 portable contracts and Metal risk reduction | Complete | Frozen packet/capture contracts, paired shader compilation, native Metal traversal/refit, two-bounce corpus, stereo MetalFX fixture, and sanitized export passed on the recorded M4 Max | Vulkan execution, RTX behavior, and cross-backend parity are intentionally deferred to M3 |
| M1 Mac reference renderer | Complete as a Mac-only reference implementation | Live Godot scene extraction, combined morph/eight-weight deformation, Metal BLAS build/refit, primary and secondary rays, guides, interactive/progressive modes, and MetalFX ran in the editor validation scene | The shared ABI, closure semantics, guide contract, dynamic geometry, stereo behavior, and diagnostics still require matched Vulkan replay before they are called dual-backend features |
| M1 Windows remote workflow | Implemented as manifests, command generation, and mocked endpoint tests | Deterministic local tests passed | A real Windows deployment, launch, debugger, log, and failure-recovery session is required |
| M2 character preparation and interaction foundation | Complete for the inspected source asset and deterministic fixtures | Repeat export, texture conversion, retarget data, 35 morph drivers, eight-weight deformation, arm/finger filtering, gesture state machine, and Metal secondary-effect visibility passed locally | Production motion captures, broader assets, content rights, and Windows runtime behavior require later verification |
| M2 visionOS companion core | Implemented as a type-checked shell, versioned schemas, and mocked lifecycle services | Platform-neutral lifecycle tests and visionOS SDK type checking passed | Entitlement, system discovery, QR/certificate pairing, reconnect, physical-device presentation, tracking, and CloudXR interoperability require a physical Vision Pro and Windows endpoint |
| M2.5 Mac runtime hybrid renderer | In progress; executable provisional implementation | Forward+ now schedules native Metal BLAS build/in-place refit/TLAS reuse; ray-traced first-directional-light soft visibility inside direct-light evaluation; emissive-triangle direct lighting; and bounded one-bounce diffuse transport with secondary positive-energy Omni direct lighting. The 16-entry Omni packet preserves Forward+ linear color, energy, indirect-energy multiplier, physical-unit conversion, camera distance fade, range/attenuation, cull-mask eligibility, and a finite visibility ray. Primary Forward+ direct lighting remains its owner. Negative Omni, spot, and area secondary lights are explicitly unsupported/diagnosed rather than substituted or silently clamped. The existing Mac-only material/guide/MetalFX work remains provisional. | The emitter-free 640×360 Omni Cornell A/B proves the new packet independently of emissive triangles; red/green neutral-floor transfer and GI-on/off deltas are recorded in the M2.5 journal. This is Mac-only one-bounce correctness evidence—not full analytic-light parity, a performance acceptance, Windows parity, DLSS, true stereo, or the RTX 5080 90 Hz target. The texture slice remains limited to its documented 16-slot opaque UV0 base-color contract. Negative Omni, spot, area, and secondary directional light contracts, full material closures, and Vulkan/RTX replay remain open. M2.5 has not passed. |
| M3 certification preparation | Complete | Schema-2 self-contained replay bundle, exact hashes, environment/build orchestration, and fail-closed report validators run on macOS | PowerShell execution on the recorded Windows host is still required |
| M3 Vulkan renderer and streamed XR | Not implemented or certified | Godot's existing Vulkan ray-tracing primitives and the required adapter boundary were audited | Vulkan backend/replay runner, ray-query-versus-pipeline measurements, reconstruction, parity, Windows OpenXR, CloudXR, physical stereo, tracking, reconnect, telemetry, and remote launch are all still required |
| M4 90 Hz performance program | Not started; gated by M3 | Optimization families and the 11.11 ms p99 acceptance contract are documented | Implementation and measurement must wait for correct Metal/Vulkan parity and then run on the RTX 5080 production profile |

Accordingly, the offline native Metal editor reference path tracer is verified within its Mac-only reference scope. The production runtime hybrid renderer remains provisional and incomplete. Portable/Vulkan-facing contracts and companion integration are partly or fully implemented but remain explicitly provisional until their named Windows, RTX, CloudXR, and physical Vision Pro checks run.

### M0 — Portable contracts and Apple risk-reduction

Deliverables:

- reproducible macOS engine build and a pinned, documented, not-yet-certified Windows build recipe;
- native Metal triangle, BLAS/TLAS, traversal, and dynamic-refit proof;
- dynamic eight-weight skinning plus blend-shape deformation followed by Metal BLAS update;
- MetalFX temporal denoised-upscaling spike using the full proposed guide contract;
- deterministic canonical scene packets, CPU validators, corruption tests, stable seeds, and machine-readable result schemas;
- paired MSL and GLSL/SPIR-V declarations with compile/reflection checks runnable on macOS;
- shader-sharing/Slang feasibility result for compilation, layout, diagnostics, and maintainability, explicitly excluding cross-GPU performance conclusions;
- a two-bounce Metal Cornell/material renderer with emissive and environment lighting, guides, motion, and deterministic captures;
- an adversarial animated MetalFX quality suite with disocclusion, thin curves, glossy response, articulated motion, camera rotation, subpixel relief, and morph motion;
- a local two-view harness with distinct cameras, layers, jitter, guides, histories, and MetalFX executions;
- sanitized export of one body LOD with eight weights and several morphs; and
- selected reference Mac hardware and measured baseline.

Gate: the Metal reference path must render the deterministic two-bounce corpus, update a dynamically deformed BLAS without stale geometry, emit valid guides, and demonstrate useful temporal MetalFX behavior rather than descriptor construction alone. Packet and shader layouts must be reproducible and the stereo harness must prove independent views and histories. Passing M0 authorizes a provisional shared contract and Mac implementation work; it does not certify Vulkan, RTX performance, DLSS, CloudXR, or renderer parity.

### M1 — Usable Mac reference path tracer

Deliverables:

- shared scene compiler and GPU ABI;
- macOS editor native Metal path tracing;
- initial StandardMaterial3D closure subset, emissives, analytic lights, and environment;
- Metal static and dynamic AS management;
- bones, eight weights, blend shapes, motion, and BLAS updates;
- interactive and progressive modes;
- common guide-buffer inspector;
- MetalFX temporal denoised upscaling on macOS;
- deterministic validation scenes, CPU invariants, golden captures, and capability diagnostics;
- backend-neutral interfaces for the future Vulkan implementation and denoiser adapters;
- paired shader source or generated-shader evidence for every accepted closure; and
- remote Windows preset validation, manifest generation, command preview, and mocked endpoint tests without claiming a live deployment.

Gate: the Mac editor must render and diagnose the actual animated Godot validation scenes through the shared scene compiler, including silhouette morphs reflected in a mirror, with no stale geometry or hidden material fallback. This may be called a usable **Mac reference path tracer**, never a completed dual-backend renderer. Any interface whose semantics cannot yet be exercised on Vulkan remains experimental.

### M2 — PC-independent product and integration foundation

Deliverables:

- deterministic BND preparation/export tool;
- canonical runtime skeleton and retarget map;
- first-/third-person variants and initial LODs;
- material conversion, repaired texture manifest, and correct normal convention;
- head/hand-driven arm and finger IK;
- 35 pose-space wrinkle-driver implementation and pose-sweep tests;
- ray-visibility masks or documented asset-variant fallback;
- simple web gesture state machines and a test tether;
- local stereo camera/guide/history validation using simulated views;
- versioned companion/endpoint protocol schemas and state-machine tests;
- a native visionOS companion shell with mocked discovery, pairing, session, telemetry, and reconnect services where SDK and device access permit; and
- generated Windows deployment manifests and endpoint commands validated without network side effects.

Gate: hands/arms remain stable across tracked or recorded motion, wrinkles do not visibly pop, and final morph/skinning deformation is visible in Metal path-traced primary/secondary effects. Protocol and deployment tests must be deterministic, but mocked success is not evidence of CloudXR interoperability or a working remote launch.

### M2.5 — Mac runtime and WYSIWYG hybrid foundation

This milestone corrects the boundary exposed by the M1 reference panel. It remains Mac-only and provisional, but it moves productive runtime integration ahead of PC access.

The 2026-08-20 frame-graph audit materially refined the reconstruction boundary. The installed macOS 27 SDK's `MTLFXTemporalDenoisedScalerDescriptor` requires an explicitly formatted full guide set and `supportsDevice:` capability detection. NVIDIA's current primary DLSS Ray Reconstruction guidance separately requires linear depth, accurate ordinary and specular motion, camera constants/jitter/reset state, and an optional host runtime. Therefore “MetalFX/DLSS” is not one interchangeable upscaler switch: both implement a shared semantic adapter, while each adapter validates its stricter native inputs and owns independent per-view history. Ordinary MetalFX temporal scaling is not evidence that MetalFX temporal denoised reconstruction is valid, and the absent licensed DLSS runtime must fail closed.

The audit also falsified the idea that the M1 capture backend should be expanded into the runtime renderer. Forward+ already owns the relevant camera histories and raster buffers, while `RenderGeometryInstanceBase` and `MeshStorage` own scene instances and current/previous GPU deformation buffers. M2.5 therefore consumes renderer-side state directly. Capture packets remain deterministic replay/reference artifacts, not the live frame transport.

The 2026-08-20 Flow audit used read-only revision `6ee5ba9af20718ea315bd2af28789d2ad7748895` with local modifications present in its renderer and MetalFX files; it is evidence, not a clean donor snapshot. Flow demonstrates a useful thin-G-buffer allocation: Forward-style direct lighting remains rasterized while ray queries produce directional visibility, diffuse transport, glossy transport, and reconstruction guides. Its shader evaluates secondary geometry/material data instead of treating projected screen color as complete hit shading. Those semantic boundaries are accepted. Its fixed pixel-stride block splatting, fixed small sample counts, Apple-26.1-only policy, and single combined lighting texture are not accepted as final Godot contracts: they complicate disocclusion, separate-effect measurement, older deployment targets, and future Vulkan parity. Godot now uses the verified complete-guide and per-view MetalFX pattern for its macOS runtime adapter, but does not copy Flow source. The implementation remains a Metal-only reference path; it cannot satisfy the separate-signal quality gate or any Windows parity claim until equivalent Vulkan/RTX guides, reconstruction, and validation exist.

The first runtime slice further falsified post-compositing shadow multiplication. Multiplying ray visibility over the already-lit Forward+ color darkens ambient and indirect energy and double-applies raster shadowing. Ray-traced directional visibility must therefore run after the depth/normal prepass and be sampled inside Forward+'s direct-light evaluation, replacing that light's raster shadow factor. Diffuse/specular transport runs after opaque shading and is reconstructed separately. This split is now the required architecture for all backends.

Deliverables:

- integrate a capability-gated hybrid renderer into the normal Forward+ viewport/game frame graph rather than a separate editor preview;
- make editor cameras and running-game cameras use the same renderer path, quality settings, scene state, materials, lights, guides, and history rules;
- replace per-frame scene-tree traversal/full capture serialization with renderer-owned resources, stable IDs, incremental dirty propagation, and bounded deferred destruction;
- reuse raster or visibility-buffer primary visibility, depth, motion, and material classification, then add measured Metal ray-traced shadows, reflections, ambient/indirect lighting, and limited secondary bounces;
- run MetalFX reconstruction and final composition on the runtime command-buffer timeline;
- produce a thin material-guide pass and explicit geometry/material tables so secondary hits evaluate supported Godot closures without relying on screen projection;
- implement ray visibility for every supported light family, with per-light diagnostics and raster fallback rather than a first-light-only claim;
- keep diffuse, specular, shadow, AO, hit-distance, reactive, motion, and disocclusion signals separable through reconstruction and measurement;
- expose truthful `Raster`, `Hybrid`, and `Progressive Reference` modes, with diagnostics listing which effects are rasterized, traced, reconstructed, disabled, or unsupported;
- support ordinary static Godot mesh resources, including `PrimitiveMesh`, without entering the deformation-bake path; reserve deformation baking/refit for actual blend-shape or skinned geometry;
- add deterministic editor-versus-game camera captures, animated geometry, disocclusion, glossy motion, thin detail, and material fallback tests; and
- measure CPU extraction/dirty propagation plus GPU deformation, BLAS build/refit, TLAS update, raster primary, ray effects, reconstruction, and composition separately.

Gate: on the recorded Mac, the same saved validation scene and camera must produce equivalent named hybrid effects in the editor viewport and a running game, update motion/deformation without stale acceleration structures, evaluate supported secondary-hit materials, cover the documented light families, reject history at motion/disocclusion, and sustain an explicitly recorded interactive test profile without full scene extraction every frame. This authorizes the label **provisional Mac runtime hybrid renderer** only. It does not authorize Vulkan parity, Windows performance, streamed XR, or the RTX 5080 90 Hz claim.

### M3 — Windows backend certification and true stereo CloudXR

Deliverables:

- reproducible Windows engine build on the recorded OS, compiler, Vulkan SDK, NVIDIA driver, and RTX GPU;
- Vulkan triangle/AS/dynamic-refit replay from the frozen M0 corpus;
- Vulkan ray-query versus RT-pipeline measurement before choosing the initial execution path;
- Vulkan implementation of the accepted scene, material, light, dynamic-geometry, camera, stereo, guide, diagnostic, and capture contracts;
- replay of identical canonical packets, seeds, animations, and expected invariants on Metal and Vulkan;
- DLSS adapter validation when the optional licensed SDK is available, otherwise a documented portable fallback without weakening the adapter contract;
- true per-eye tracing and histories on both backends;
- per-eye MetalFX validation on macOS and the corresponding Windows reconstruction validation;
- Windows OpenXR path-traced runtime;
- native visionOS Foveated Streaming companion;
- Apple `StreamingSession` + CloudXR reference path connected to physical Vision Pro before custom integration is accepted;
- discovery, QR/certificate pairing, reconnect, and endpoint supervision;
- delayed OpenXR-launch lifecycle proof;
- twenty-six-joint hand tracking into Godot;
- depth/alpha negotiation and diagnostics;
- runtime latency, frame-time, encode, network, and decode telemetry; and
- one-button remote deploy/launch workflow.

Gate: the frozen renderer suite must satisfy the parity definition on Metal and Vulkan before the renderer is called dual-backend or complete. Physical Vision Pro must receive distinct correct stereo views; hand poses must be time-aligned; reconnect must not require manually restarting the whole stack; and no monoscopic-copy path may be active. A Windows failure blocks M3 and all streamed-VR claims, but does not retroactively invalidate the separately labeled Mac reference renderer.

### M4 — Path-tracing performance and 90 Hz stereo

This is the concentrated cross-backend optimization and certification milestone. Correctness-preserving Mac profiling, AS reuse, guide packing, reconstruction work, shader specialization, cache preparation, and isolated A/B prototypes may proceed earlier in M2.5. Backend-selection and RTX-specific conclusions begin only after M3 establishes correctness and cross-backend parity, so performance work cannot normalize a broken renderer.

Deliverables:

- freeze a production performance profile: per-eye submitted and internal resolution, field of view, sample/bounce limits, material/light scope, reconstruction mode, dynamic-resolution floor, scene/character budgets, and CloudXR stream settings;
- implement and expose full, hybrid, and progressive quality modes with truthful diagnostics;
- implement the shared hybrid path on Metal and Vulkan, using raster/visibility primary hits and measured ray-traced secondary effects where that is the winning quality/performance allocation;
- benchmark ray query versus RT pipeline, wavefront compaction, ray/material sorting, pipeline specialization, and capability-gated NVIDIA scheduling features;
- evaluate the already-defined unified light distributions and DI reservoir ABI through measured ReSTIR DI and camera-centered ReGIR prototypes; evaluate adaptive sampling, bounded bounces, and Russian roulette against the progressive reference;
- optimize stereo dispatch and share only view-independent work;
- tune BLAS compaction/refit/rebuild, TLAS updates, LOD/culling/batching, texture residency, descriptor/bindless access, transient memory, and pipeline caches;
- measure safe async overlap among deformation, AS work, tracing, reconstruction, composition, and encode preparation;
- tune DLSS RR/SR or the portable reconstruction path, guide quality, history behavior, and dynamic resolution without frame generation;
- create automated A/B performance and image-quality reports for every accepted optimization;
- add in-engine GPU stage timelines, worst-frame capture triggers, memory/residency telemetry, and mode/effect diagnostics; and
- build and run a representative stereo character/city performance fixture for at least ten continuous minutes, without waiting for the M5 gameplay slice.

Gate: on the recorded RTX 5080 host, the frozen production profile must submit two distinct correct views at a sustained 90 Hz without frame generation. Both the complete engine GPU path from deformation through final composition and the CPU frame-to-submit path must have p99 frame time at or below 11.11 ms over the ten-minute route, with no sustained thermal, memory, shader-compilation, or streaming hitch. The profile must meet its fixed resolution floor and documented perceptual thresholds against the progressive reference; lowering quality below that profile does not pass. Metal must render the same named hybrid features and validation scenes correctly, but is not required to meet the RTX 5080 performance number.

### M5 — Web-swinging city slice

Deliverables:

- representative city district and streaming strategy;
- web attach/release, tension, locomotion, collision, comfort modes, and recovery;
- near-field character quality and fast-motion MetalFX/DLSS tuning;
- initial audio and haptics;
- repeatable traversal benchmark route; and
- user comfort and latency study.

Gate: a complete five-to-ten-minute session can be launched from macOS, streamed from the RTX PC, played with hand gestures, and reproduced with captured performance/quality data while retaining the frozen M4 90 Hz production profile on the traversal route.

### M6 — Fidelity and production scalability

Deliverables may include additional closures, transmission, decals, particles, fog/volumes, city LOD/streaming, adaptive sampling, shader compilation/cache improvements, robust device loss, crash reporting, and asset validation. Lighting reservoirs are not deferred here: their semantic ABI and direct-light prototypes are staged before M4 under section 7.8; only a measurement-justified expansion belongs in M6.

Gate: changes are driven by captured vertical-slice bottlenecks, not feature-list ambition. Any feature accepted into the production profile must preserve the M4 90 Hz gate; higher-cost features must live in a separately named non-VR or reference profile.

### M7 — Body and microphone

Deliverables:

- iPhone body-tracking companion and calibration;
- timestamped skeleton transport and clock synchronization;
- confidence-aware PC fusion with AVP head/hands;
- upstream microphone through supported CloudXR/visionOS versions;
- permission, mute, device, privacy, and diagnostics UI; and
- recorded occlusion/failure dataset.

Gate: fusion improves measured lower-body/pelvis fidelity over inference alone and degrades gracefully when the phone loses the body.

### M8 — Native Apple-device execution

Deliverables:

- native visionOS Mobile-renderer build with ARKit hand tracking;
- reduced character/city asset profile;
- shared gameplay compatibility report;
- iOS/iPadOS Metal RT experiment; and
- sustained thermal/performance measurements.

Gate: define separate native product quality targets from evidence. Do not imply RTX parity.

## 15. Provisional quality and performance budgets

These are experiment targets, not promises:

| Target | Initial objective | Required measurement |
|---|---:|---|
| RTX 5080 streamed VR | Sustained true-stereo 90 Hz; no frame generation | Frozen production profile, p99 engine CPU and GPU frame times at or below 11.11 ms over ten minutes, full stage breakdown, frame-pacing distribution, and motion-to-photon latency on the M5 route |
| macOS editor interactive PT | Responsive camera/material iteration at adaptive internal resolution | Selected M3-or-newer Mac, 1 spp, MetalFX, representative character/city scene |
| macOS progressive PT | Correct converged reference without temporal dependency | Fixed seed and sample count across validation scenes |
| Stereo correctness | Independent views and histories | Parallax, motion, disocclusion, compositor layer, and capture tests |
| Dynamic character | No stale BLAS and bounded update cost | Skin + 35 morph sweep, mirror/shadow scene, deformation/refit timings |
| Streaming | Comfort-compatible stable session | Render, encode, network, decode, display, and tracking-age telemetry |

The selected development host is the audited M4 Max Mac Studio with 128 GB unified memory. MetalFX denoised upscaling’s Apple-family-9 requirement makes M3 or later the general minimum for this exact mode. A Mac that can render a correct but slow progressive image does not satisfy the interactive MetalFX goal.

## 16. Validation strategy

Maintain small, deterministic scenes:

- Cornell box for energy, emissive sampling, and convergence;
- material sphere grid for roughness/metal/transmission parity;
- normal/UDIM/texture-array scene for tangent and filtering errors;
- glass, alpha cutout, and transparent-overlay scene;
- skinned eight-weight character with several active morphs and a mirror;
- full 35-morph pose sweep;
- stereo parallax/disocclusion and rapid head-turn scene;
- thin web-line fast-motion scene;
- CloudXR depth/alpha composition scene; and
- a fixed stereo character/city performance fixture independent of gameplay; and
- city traversal stress route.

For renderer parity, capture the same canonical scene packet, fixed random seeds, camera, exposure, and high sample count on Vulkan and Metal. Compare numeric error where meaningful, perceptual difference, material invariants, and obvious structural failures. Denoiser output should be evaluated temporally with motion sequences, not only screenshots.

Automated checks should include ABI layout, shader compilation, unsupported-material reporting, resource lifetime, AS update ordering, NaN detection, history reset, left/right layer identity, and headless scene-export validation. Physical headset tests remain mandatory for comfort and system integration.

## 17. Hypothesis register

| ID | Hypothesis | Falsification experiment | Fallback |
|---|---|---|---|
| H1 | A compute/wavefront design is a viable semantic base for Vulkan and Metal. | M0 proves the Metal/reference semantics; M3 replays the matched two-bounce scene and performs the cross-GPU profile comparison. | Separate optimized integrators behind shared ABI. |
| H2 | Native Metal is required; Vulkan-through-MoltenVK cannot meet the feature/control goal. | Recheck current MoltenVK RT support and prototype if status changes. | Continue native Metal; MoltenVK is never the only plan. |
| H3 | MetalFX denoised upscaling provides useful 1–few-spp editor output. | Fast camera/character/web/material temporal suite on reference Mac. | Progressive mode plus portable denoiser; reassess hardware/algorithm. |
| H4 | One scene AS can serve both eyes. | Stereo correctness and profiling with shared TLAS. | Per-view instance data/TLAS only where visibility demands it. |
| H5 | Eight-weight skinning + 35 morphs + BLAS refit fit the character budget. | M0/M2 measure the Metal pose sweep; M3 repeats it at Windows VR resolution before establishing the shared budget. | Selective morph LOD, split BLAS, async scheduling, or reduced runtime mesh. |
| H6 | NVIDIA’s branch can donate isolated systems without owning architecture. | Port one dynamic-mesh/material slice and audit coupling/upstream drift. | Reimplement from documented behavior and tests. |
| H7 | A common guide contract can feed both MetalFX and DLSS well. | M0 validates MetalFX semantics; M3 replays matched motion sequences through the Windows adapter before certifying the common contract. | Superset contract with explicit adapter conversions/extra guides. |
| H8 | CloudXR Runtime can host Godot reliably after lifecycle supervision. | Repeated cold-connect, reconnect, crash, and delayed-client tests. | Patch Godot OpenXR retry behavior or maintain a dedicated bootstrap runtime layer. |
| H9 | Apple’s native companion can stay thin. | Implement discovery/pairing/session/diagnostics prototype. | Add a versioned companion service layer without moving gameplay into it. |
| H10 | Native Apple body tracking is a sufficient first full-body baseline. | Calibrated motion set with occlusion and ground-truth comparison. | Specialized model or additional sensors after failure data exists. |
| H11 | A measured full or hybrid renderer can sustain true-stereo 90 Hz on the RTX 5080 at the frozen production profile. | M4 ten-minute fixture with p99 CPU/GPU frame times, fixed quality floors, temporal captures, and no frame generation. | Reduce effect allocation through an explicitly named hybrid preset, improve reconstruction, or revise the content profile; never silently lower the frozen gate. |
| H12 | ReSTIR DI with camera-centered ReGIR and environment importance sampling is the best first city-scale many-light candidate. | Replay the deterministic many-light/stereo-disocclusion suite, then compare DI-only, DI+ReGIR placements, and baseline importance sampling for correctness, temporal quality, memory, and separated GPU stages on Metal and Vulkan/RTX. | Keep the unified distributions; choose conventional importance sampling, another ReGIR placement, or a different measured direct-light method. |
| H13 | ReSTIR GI, ReSTIR PT/GRIS, and a shared world-space radiance cache are worthwhile indirect-transport complements. | Run separate fixed-cost, progressive-reference, correlation/disocclusion, memory, and stage-timing prototypes after DI is correct. | Retain the lower-complexity DI/hybrid path; do not couple indirect methods to the shared ABI prematurely. |

## 18. Repository and dependency strategy

- Keep upstream Godot merges regular and isolate renderer work in reviewable topic commits.
- Record the exact NVIDIA donor commit for every adapted section and preserve its license notices.
- Introduce engine interfaces before proprietary integrations: `PathTracingBackend`, `DenoiserUpscaler`, scene compiler, guide contract, XR endpoint manager.
- Compile CloudXR and DLSS adapters conditionally and provide actionable missing-SDK messages.
- Keep Apple companion code in a clearly separate Xcode project/repository boundary if that improves signing and release management.
- Run CI on a real or hosted Apple-silicon Mac and a Windows NVIDIA worker; software compilation alone cannot validate RT/MetalFX/DLSS.
- Cache shaders/pipelines by backend, device, SDK, and source hash. Never share incompatible caches.
- Version asset manifests, tracking packets, opaque-channel messages, build numbers, marketing versions, and SDK compatibility independently.
- Preserve user project compatibility where feasible and clearly mark experimental APIs.

## 19. Principal risks

### Renderer scope

A production path tracer is a multi-year engine program if allowed to absorb every material, effect, and editor feature. Control it with the bounded closure set, validation scenes, milestone gates, and a raster fallback.

### Deferred Windows evidence becoming forgotten

Deferring the PC creates a risk that Metal-only assumptions harden into supposedly portable architecture. Prevent this with frozen canonical packets, paired shader declarations, backend-neutral interfaces, explicit provisional labels, and a visible M3 certification backlog. Do not optimize or expand a contract when the missing Vulkan experiment could still falsify it. Once PC access exists, prevent the inverse problem—an NVIDIA path that outruns Metal semantics—with the same M3 parity gate and matched captures.

### Temporal reconstruction in violent motion

Swinging, thin webs, fast hands, glossy lenses, and animated wrinkle microgeometry are adversarial for temporal denoisers. Build those tests before the city and preserve a higher-cost/native-resolution mode for diagnosis.

### Performance work hiding quality loss

Hybrid rendering, reconstruction, adaptive resolution, LOD, and temporal reuse can meet a timing graph while visibly breaking motion or materials. Freeze the M4 profile before optimization, compare every accepted change against progressive references and temporal sequences, and expose the active technique in diagnostics. A hidden quality reduction is a correctness failure, not an optimization.

### XR latency and sickness

The target is sustained 90 Hz stereo on the RTX 5080, not a beautiful low-frame-rate demo. Measure the complete pipeline and provide truthful full/hybrid quality modes, stable pacing, prediction, comfort locomotion, and rapid fallback. Do not use averages alone; track p99/worst-frame behavior and latency distributions. Frame generation does not satisfy the initial target.

### Asset and IP rights

Both the BND asset license and Spider-Man IP are unresolved distribution blockers. Technical investment should remain reusable with an original character and generic web-swinging mechanics.

### SDK/platform churn

CloudXR, visionOS Foveated Streaming, MetalFX, OpenXR extensions, and donor branches will evolve independently. Pin verified combinations, isolate adapters, and re-run capability probes on upgrades instead of coupling product versions.

## 20. Decisions still requiring evidence or owner input

- Windows certification host availability, exact GPU/driver/OS/toolchain, and whether RTX 5080 remains the required reference target.
- Whether visionOS 26.4 compatibility is required or visionOS 27 can be the companion baseline.
- Minimum fixed per-eye submitted resolution, internal-resolution floor, and perceptual thresholds for the 90 Hz RTX 5080 production profile.
- The first supported material/lighting feature boundary.
- Whether renderer work is intended for upstream Godot or a long-lived private fork.
- Licensing/redistribution terms for BND, CloudXR, DLSS/Streamline, and any donor code.
- Whether the product remains private research, becomes an original-character project, or pursues Marvel authorization.
- City source/data and its license/streaming requirements.
- Whether the first iPhone body tracker targets only LiDAR/modern Pro devices or a broader camera-only set.

## 21. Recommended first implementation sequence

1. Freeze a reproducible baseline and capture exact target hardware/software.
2. Write the canonical GPU ABI and tiny captured-scene format before importing a large donor renderer.
3. Add paired MSL/GLSL declarations, host-side layout reflection, corruption tests, golden packets, fixed seeds, CPU invariants, and result schemas.
4. Complete Metal triangle/AS/dynamic-refit and the synthetic eight-weight-plus-morph proof.
5. Implement the deterministic two-bounce Metal validation renderer and freeze its canonical packet corpus.
6. Turn the MetalFX descriptor probe into the animated temporal-quality suite, including explicit guide validation and history-reset tests.
7. Build true two-view simulation on macOS with distinct cameras, layers, jitter, guides, histories, and MetalFX executions.
8. Evaluate Slang and compile the paired Vulkan shader corpus to SPIR-V on macOS, but defer execution-path and performance decisions.
9. Build the shared scene compiler, Mac editor controls, guide inspector, capability diagnostics, and deterministic capture/replay.
10. Establish sanitized character export, pose-space drivers, recorded tracking inputs, and product-neutral interaction tests.
11. Define and test companion/endpoint protocols, lifecycle state machines, deployment manifests, and remote-command generation against local mocks.
12. Prepare a single Windows certification bundle containing source revisions, dependency probes, build scripts, scene packets, expected invariants, automated captures, and benchmark commands.
13. Integrate the provisional Metal hybrid path into the actual Forward+ editor/game viewport frame graph, replace full scene-tree capture with renderer-owned incremental state, and pass the M2.5 WYSIWYG/runtime gate.
14. When the PC is available, run the Windows build, Vulkan dynamic-geometry replay, ray-query-versus-pipeline benchmark, guide/denoiser validation, and full Metal/Vulkan parity suite before choosing or optimizing the Windows path.
15. Run Apple’s reference StreamingSession/CloudXR path unchanged, then integrate the supervised endpoint, remote workflow, and physical stereo headset tests.
16. Execute M4 as a concentrated cross-backend performance program: freeze the production profile, optimize the already-correct hybrid path and the remaining families in section 7.2, and meet sustained true-stereo 90 Hz on the RTX 5080.
17. Only after M4 passes, scale the streamed city and web-swinging vertical slice.

This order maximizes useful Mac work while preserving an honest falsification point. The Windows phase is deferred, not removed: M0–M2.5 manufacture stable inputs, a real Metal runtime integration, and expected results so PC access is spent executing and diagnosing a bounded certification suite rather than discovering basic contracts or viewport integration interactively.

## 22. References and evidence entry points

### Local code

- Godot rendering device and drivers: `servers/rendering/rendering_device*`, `drivers/vulkan`, `drivers/d3d12`, and `drivers/metal`.
- Godot OpenXR and visionOS modules: `modules/openxr` and `modules/visionos_xr`.
- Godot remote deployment/export platform code: `platform/windows` and editor export/run integration.
- Flow Engine MetalFX and stereo files listed in section 5.3.
- BND source asset: `/Users/ethan/Downloads/BND MODEL/`.
- NVIDIA donor branch: [`nvidia-pt-dlss-dev`](https://github.com/NVIDIA-RTX/Godot/tree/nvidia-pt-dlss-dev).

### Primary external documentation

- [Godot RenderingDevice](https://docs.godotengine.org/en/latest/classes/class_renderingdevice.html)
- [Apple Metal feature set tables](https://developer.apple.com/metal/capabilities/)
- [Apple Metal ray tracing](https://developer.apple.com/documentation/metal/metal_sample_code_library/accelerating_ray_tracing_using_metal)
- [Apple StreamingSession reference implementation](https://github.com/apple/StreamingSession)
- [NVIDIA: CloudXR client for visionOS using Foveated Streaming](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/foveated_streaming/getting_started.html)
- [NVIDIA: streaming endpoint setup for Foveated Streaming](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/foveated_streaming/server_setup.html)
- [NVIDIA CloudXR release notes](https://docs.nvidia.com/cloudxr-sdk/latest/release_notes/release_notes.html)
- [NVIDIA CloudXR system requirements](https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/system_requirements.html)
- [Apple ARKit body tracking](https://developer.apple.com/documentation/arkit/arbodytrackingconfiguration)
- [Apple Vision body-pose detection](https://developer.apple.com/documentation/vision/detecting_human_body_poses_in_3d_with_vision)
- [Bitterli et al.: ReSTIR DI](https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/)
- [Boksansky et al.: ReGIR](https://research.nvidia.com/labs/rtr/publication/boksansky2021rendering/)
- [Ouyang et al.: ReSTIR GI](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing)
- [Lin et al.: GRIS / ReSTIR PT](https://research.nvidia.com/labs/rtr/publication/lin2022generalized/)
- [RTXDI repository and v3.0.0 release](https://github.com/NVIDIA-RTX/RTXDI/releases/tag/v3.0.0)
- [RTXDI integration](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md), [ReSTIR GI](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md), and [ReSTIR PT](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirPT.md) documentation

### 2026-08-21 implementation refinement

The installed Xcode 27.0/macOS 27 `MTLFXTemporalDenoisedScaler` header falsified the temporary policy of setting denoise-strength and reactive masks for every valid reflection. A denoise-strength value of `1` excludes the pixel from denoising; a reactive value of `1` ignores temporal history. Stable opaque reflection pixels must normally use `0` for both, with depth/motion/history invalidation governing true discontinuities. The Metal implementation now reuses an already traced reflection hit to provide primary-surface-replacement normal, diffuse albedo, Fresnel specular albedo, and roughness guides, while retaining primary depth/motion. This is a Mac-only refinement with no additional ray query, not backend parity or a performance acceptance result.

The reported razor-straight editor line was separately falsified as renderer output: it is the editor origin/grid gizmo layer, which is deliberately in the editor camera's cull mask and continues outside scene content. A clean game capture does not contain it. Do not use global reactive/denoise masking to accommodate editor UI. Game transparency remains fail-closed until the renderer can supply MetalFX's exact linear RGBA transparency overlay; that overlay is composited/upscaled by MetalFX and is not denoised scene radiance.

### 2026-08-21 editor-overlay reconstruction boundary

The initial classification did **not** authorize hiding the editor origin/grid as the fix. The editor creates origin, grid, and transform-gizmo primitives as 3D scenario instances, so they previously entered the same low-resolution scene color that MetalFX temporal reconstruction receives while having no corresponding ray-material guides. The editor now renders the product-neutral editor overlay layers through a separate transparent, native-resolution `SubViewport` with a synchronized camera, then composites that texture in editor UI above the reconstructed scene viewport. This keeps grid/gizmos visible while excluding them from both denoised and ordinary temporal MetalFX input. The normal scene camera carries only content layers; the overlay camera carries editor gizmo, grid, and tool layers. Camera-preview transform/projection are synchronized explicitly.

This is deliberately an editor-owned boundary, not a vendor/layer exception in the runtime renderer and not a substitute for a real game transparency overlay. The current automated source contract verifies the separated cull masks, transparent full-resolution overlay, post-scene `TextureRect` composition, and preview-camera synchronization. A rebuilt Apple M4 Max editor launched the deterministic editor capture with MetalFX temporal denoised active and no lifecycle errors (`/tmp/hybrid-editor-overlay-final.lbfSXY/home/Library/Application Support/Godot/app_userdata/Hybrid Runtime Validation/hybrid_runtime_validation_editor.png`). The new composite-window harness captures native/bilinear, ordinary MetalFX temporal, and denoised MetalFX at 0.67 scale, then makes a 0.035-radian camera orbit and compares the first moved frame to frame 16. On the recorded host, each mode's saturated origin-line span was unchanged after settling (0-pixel delta); static span was 2 pixels in each mode. The metric is a thin-line regression signal, not a substitute for broader perceived-quality evaluation.

### 2026-08-21 editor preview-light ownership

The editor Preview Sun is a real shadowed `DirectionalLight3D`, so leaving it active while Full Hybrid explicitly owns a scene's `WorldEnvironment` double-counts direct lighting and makes the editor viewport disagree with sky-only runtime output. The 3D editor now suppresses that internal light only when all three ownership conditions hold: a `WorldEnvironment` exists, Full Hybrid mode `2` is selected, and hybrid environment lighting is enabled. The Preview Sun button is disabled and its state text identifies Full Hybrid environment ownership. Outside this predicate, authored directional-light suppression and the ordinary manual Preview Sun toggle retain their existing behavior. This is editor-only ownership hygiene; it does not synthesize a light or alter environment transport, energy, exposure, game runtime, or backend parity.

Documentation and local code should be rechecked at implementation time. In particular, platform availability, SDK licensing, extension behavior, and feature tables are moving inputs, not permanent truths.

### 2026-08-22 procedural atmosphere radiance evidence

The following is baseline evidence from the former visible-Sky-only procedural atmosphere, superseded for Metal Full Hybrid by the residual/lobe experiment recorded in Section 7.9. Its finite 3.2-degree authored solar disk was separate from the diffuse dome and attenuated by the same bounded cloud field. A Mac-only fixed-capture run found finite, selectable RGBA32Float Sky importance data at twilight and night, but did not establish a measurable moving hard-shadow centroid from that full-Sky disk. This baseline falsifies neither the implemented residual-lobe path nor the unchanged-Sky MIS hypothesis; it retains the no-double-count comparison as a required follow-up. Do not infer a direct-light, cloud-shadow, backend-parity, stereo, or performance result from these baseline captures.

The final 112-frame-settled clear daylight baseline A/B produced no spatially separable hard receiver-shadow deficit when clock time changed. Its shadow-deficit centroid movement is therefore undefined, not zero: the unchanged full-Sky procedural lobe did not meet the hard-directional-shadow requirement. This is evidence that motivated the implemented renderer-owned residual-lobe path and remains a no-double-count comparison requirement, not authority to add an analytic or supplementary light.

A bounded energy sweep strengthened that conclusion: with all non-disk radiance fixed, disk energy `0/48/96/192/256` scaled selectable Sky peak luminance from `2.8598` through `152.4333`, but receiver-only floor luma differences remained below `1/255` at the 95th percentile even at `256`. The permissive residual response stayed centered on visible witnesses rather than an occluded receiver. This falsifies insufficient authored disk energy as the explanation; the shadow-deficit centroid/motion is undefined at every bounded value.

### 2026-08-22 transport-conservative hybrid scene selection hypothesis

The renderer may use camera/baked-occluder/HZ visibility to decide the raster-primary set, but that set alone is not a valid ray-transport scene: an off-camera object can alter a visible receiver through reflection, GI, emissive sampling, direct-light visibility, or a finite positional-light path. The current hypothesis is therefore a **runtime conservative transport superset** layered on top of existing baked-occluder/HZ raster-primary behavior, rather than a replacement for it.

For the currently implemented model, a visible receiver has at most one secondary reflection or diffuse segment of length `D`, followed by at most one direct/emissive/environment/shadow visibility segment of length `D`. Any geometry that can participate in that bounded chain lies in the Minkowski-expanded primary-receiver region of `2D`; a further 10% expansion covers finite bounds and numeric/bucket discretization. Positional-light influence is selected from the corresponding `D + 10%` receiver expansion; directional lights are explicit, always-retained candidates because their influence is unbounded. Geometry is grouped by deterministic `D` buckets and stable IDs. Transport light selection remains separate from transport geometry selection, and the current secondary punctual packet retains the existing 8-bit ray layer mask and deterministically score-sorts at most 16 eligible Omni lights.

The culler must fail open to every eligible geometry and light when it is disabled, `D` is non-finite or non-positive, no primary geometry exists, or any participating geometry/light influence bound is invalid or non-finite. This is a correctness rule, not a quality/performance fallback. The bounded proof does not apply to a future multibounce integrator without a new derivation and fixture.

This work is not a `.occ` format extension, baked PVS system, or a claim of multibounce correctness, stereo certification, Vulkan parity, or performance. It preserves primary raster behavior while making the hybrid ray scene conservative for the present one-secondary-segment transport model. Useful primary-visibility context is [Teller & Séquin's PVS work](https://people.csail.mit.edu/teller/pubs/siggraph91.pdf) and [GPU Gems 2's visibility management for per-pixel lighting](https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-15-managing-visibility-pixel-lighting); the transport distinction follows the path-space framing in [Veach's thesis](https://graphics.stanford.edu/papers/veach_thesis/).
