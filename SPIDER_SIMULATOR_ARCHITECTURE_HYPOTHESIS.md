# Spider Simulator Engine and Platform Architecture Hypothesis

**Status:** Working architecture hypothesis, not a specification

**Repository baseline:** Godot 4.8-dev fork

**Last revised:** 2026-08-20

**Primary development host:** macOS

**Primary development and reference-rendering host:** macOS on Apple silicon

**Deferred high-fidelity runtime:** Windows PC with NVIDIA RTX 5080

**Primary headset:** Apple Vision Pro through CloudXR/Foveated Streaming

## 1. Purpose and decision rule

This document proposes an end-to-end architecture for a physically convincing VR Spider-Man simulator. It is deliberately written as a set of hypotheses. An implementation agent should not treat a named API, donor branch, milestone, or performance target as sacred. It should preserve the product requirements, reproduce the evidence, benchmark alternatives, and replace a proposal when a demonstrably better solution exists.

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

The M3 Windows certification bundle now contains the frozen packet, SPIR-V, expected invariants, environment probe, and pinned build recipe. PowerShell execution, Vulkan traversal, RTX/DLSS behavior, CloudXR, 90 Hz performance, and Metal/Vulkan parity remain untested and must not be inferred from M0.

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

The renderer should expose four modes, all using the same scene representation:

- **Interactive:** one or a few samples per pixel, aggressive adaptive resolution, denoising, and upscaling.
- **Progressive reference:** accumulated samples with deterministic seeds, no temporal upscaler required, used for editor look development and correctness.
- **Hybrid interactive:** rasterized or visibility-buffer primary visibility plus selected ray-traced shadows, reflections, transmission, ambient occlusion, direct/indirect lighting, or limited secondary bounces. Effects are selected by measured cost and perceptual value, not by a fixed promise that every pixel follows a full path.
- **XR interactive:** per-eye interactive rendering with strict latency and history rules.

Raster Forward+/Mobile remains available as a fallback and for the future native visionOS build. Path tracing should be a rendering method or renderer feature with explicit capability checks, not gameplay-specific code.

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

Milestones M0 through M2 are intentionally executable without a Windows PC. They may establish a Mac reference implementation and portable product foundations, but they do not certify the renderer as cross-platform complete. M3 is the first PC-dependent milestone and the only place where Vulkan/RTX parity claims become eligible. Work after a failed gate may continue only when it is independent of the failed claim; dependent features remain provisional and must not be relabeled complete.

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

This is a concentrated optimization milestone. It begins only after M3 establishes correctness and cross-backend parity, so performance work cannot normalize a broken renderer.

Deliverables:

- freeze a production performance profile: per-eye submitted and internal resolution, field of view, sample/bounce limits, material/light scope, reconstruction mode, dynamic-resolution floor, scene/character budgets, and CloudXR stream settings;
- implement and expose full, hybrid, and progressive quality modes with truthful diagnostics;
- implement the shared hybrid path on Metal and Vulkan, using raster/visibility primary hits and measured ray-traced secondary effects where that is the winning quality/performance allocation;
- benchmark ray query versus RT pipeline, wavefront compaction, ray/material sorting, pipeline specialization, and capability-gated NVIDIA scheduling features;
- evaluate adaptive sampling, light importance sampling, reservoir-based candidates, bounded bounces, and Russian roulette against the progressive reference;
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

Deliverables may include additional closures, transmission, decals, particles, fog/volumes, city LOD/streaming, lighting reservoirs, adaptive sampling, shader compilation/cache improvements, robust device loss, crash reporting, and asset validation.

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
13. When the PC is available, run the Windows build, Vulkan dynamic-geometry replay, ray-query-versus-pipeline benchmark, guide/denoiser validation, and full Metal/Vulkan parity suite before choosing or optimizing the Windows path.
14. Run Apple’s reference StreamingSession/CloudXR path unchanged, then integrate the supervised endpoint, remote workflow, and physical stereo headset tests.
15. Execute M4 as a concentrated performance program: freeze the production profile, implement and compare full/hybrid paths and the optimization families in section 7.2, and meet sustained true-stereo 90 Hz on the RTX 5080.
16. Only after M4 passes, scale the streamed city and web-swinging vertical slice.

This order maximizes useful Mac work while preserving an honest falsification point. The Windows phase is deferred, not removed: M0–M2 manufacture stable inputs and expected results so PC access is spent executing and diagnosing a bounded certification suite rather than discovering basic contracts interactively.

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

Documentation and local code should be rechecked at implementation time. In particular, platform availability, SDK licensing, extension behavior, and feature tables are moving inputs, not permanent truths.
