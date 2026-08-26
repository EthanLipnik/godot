# Bistro convergence and scaling checklist

Last updated: 2026-08-25

This is the executable checklist for correcting Flux's raw-noise, temporal
stability, hybrid-lighting, reuse, and scaling failures exposed by the Danger
Room Bistro scene. It refines the M2.5 implementation journal; it does not
replace `ADVANCED_RENDERING_ARCHITECTURE_HYPOTHESIS.md`.

Check an item only when its named evidence exists. Code presence alone does not
complete a visual, correctness, performance, Metal/Vulkan parity, or stereo
gate.

## Outcome

Flux must provide three truthful modes:

1. **Raw interactive diagnostic**: current stochastic samples without MetalFX
   or hidden reconstruction, used to expose estimator defects.
2. **Interactive hybrid**: deterministic raster work plus bounded ray transport
   and explicit reconstruction, with stable energy and measured latency.
3. **Progressive reference**: unbiased stationary accumulation that visibly and
   measurably converges and resets on every invalidating change.

The production gate remains sustained true-stereo 90 Hz on an RTX 5080 without
frame generation. That is an 11.11 ms complete-frame p99 budget, not an
11.11 ms ray-effects-only budget. Metal must preserve the same named semantics
and validation scenes, but is not required to meet the RTX 5080 performance
number.

## Observed baseline

These numbers are diagnostic evidence, not accepted performance claims:

- Bistro has about 2,909 source instances, 4.2 million source triangles,
  1.18 million selected ray triangles, and 2,812 emissive triangles.
- A no-reuse 1280x720 run measured about 1,144 ms in `ray_effects`, 670,720 GI
  rays, 50,961 reflection rays, and more than one million alpha/mixed
  intersections.
- Later reuse/residency runs measured roughly 430-606 ms in `ray_effects` and
  still recorded about 293,000 alpha intersections.
- One measured reusable-path result reused about 1,899 of 179,646 queries;
  another recorded no selected reused candidate.
- ReSTIR DI off versus on was visually very close in one matched capture
  (luminance MAE 0.002006, correlation 0.999497), so reuse activity alone is not
  evidence of useful variance or ray reduction.
- With MetalFX disabled, Flux composes raw, frame-changing transport. Existing
  bounded history/replay is not unbiased progressive accumulation.

Source logs and harness:

- `../../../../danger-room/reports/flux_stress/`
- `../../../../danger-room/scripts/validation/flux_stress_suite.gd`
- `../../../../danger-room/tools/validation/FLUX_STRESS_SUITE.md`

## Completed correctness prerequisites

- [x] Use canonical 128x128x64 NVIDIA STBN data as Flux's stochastic source.
- [x] Remove output-tile-dependent STBN temporal phasing.
- [x] Preserve rectangular-light sample coordinates across reservoir reuse.
- [x] Correct rectangular-light area-measure proposal/target weighting.
- [x] Keep directional lights out of the unified finite-light reservoir until
  their angular payload and PDF contract are complete.
- [x] Reevaluate selected direct-light visibility at the current receiver.
- [x] Provide `--flux-fresh-ray-oracle` that disables reuse, history, MetalFX,
  and reconstruction while retaining fresh direct, GI, and reflection work.
- [x] Keep normal production ReGIR fail-closed while single-cell correlation and
  reuse quality remain unvalidated.
- [ ] Re-run the full Bistro matrix with a freshly installed editor containing
  all completed fixes above.
- [ ] Confirm that the original 128x128 tile flicker, lamp-position jumps,
  moving lamp shadows, and periodic area-light energy spikes are absent.

## P0 — Deterministic Bistro acceptance fixture

No estimator or optimization may advance without this fixture.

- [ ] Add a versioned Bistro validation profile with exact repository revision,
  OS, SDK, compiler, GPU, driver/runtime, resolution, camera transform, scene
  revision, sample settings, warmup, capture frames, and run count.
- [ ] Freeze camera transform, exposure, sky clock, sun, clouds, wind,
  animation, particles, streaming state, and resource residency for the static
  phase.
- [ ] Add separate moving-sky and moving-camera phases; never mix their history
  with the frozen phase.
- [ ] Record whether `AtmosphereSkyClock.paused` is true at runtime. Editor
  preview pause is not runtime pause.
- [ ] Capture raw RGB, diffuse/specular transport, depth, motion, normals,
  material identity, reservoir identity, reservoir weight/M, and per-lobe
  sample-count outputs.
- [ ] Capture fresh-ray oracle, DI-only reuse, reusable-path-only, combined reuse,
  and all-reuse-off variants from identical starting state.
- [ ] Record stage timings separately: deformation, BLAS build/refit, TLAS,
  light distribution, direct candidates, temporal reuse, spatial reuse,
  visibility, GI, reflection, alpha traversal, reconstruction, and composition.
- [ ] Record invalid-PDF, nonfinite, rejected-energy, stale-identity,
  visibility-rejection, disocclusion, and history-reset counters.
- [ ] Make captures deterministic for a fixed seed, frame index, and sample
  count.

Acceptance:

- [ ] Fixed-seed repeated captures are bit-identical where promised or have a
  documented nondeterminism boundary.
- [ ] Frozen-scene mean radiance has no periodic energy oscillation.
- [ ] No screen-tile, checkerboard, scanline, or unwritten-pixel structure is
  visible in raw sample error.
- [ ] Every active pixel is written exactly once and every inactive pixel is
  explicitly cleared.
- [ ] The fixture reports frame-matched timings and rejects stale asynchronous
  readbacks.

## P1 — True progressive accumulation

Progressive accumulation is a separate reference/photo mode. It must not be
confused with ReSTIR reservoirs, reusable paths, transport sample replay, or
MetalFX image history.

- [ ] Add per-view accumulated radiance and exact sample-count storage.
- [ ] Use a numerically stable unbiased running mean or sum/count contract.
- [ ] Accumulate direct, diffuse GI, and specular transport without changing
  their estimator means.
- [ ] Preserve independent accumulation, jitter, motion, and history for each
  stereo eye.
- [ ] Expose current sample count and accumulation validity in diagnostics.
- [ ] Add an explicit clear/freeze/restart control for editor and validation
  use.
- [ ] Reset on camera/projection/jitter-contract changes, resize, scene switch,
  geometry or material revision, light distribution revision, environment
  content revision, exposure/tonemap contract changes, and disocclusion.
- [ ] Do not reset for MetalFX enablement changes that cannot affect raw
  transport; keep image history and transport history separate.
- [ ] Prevent animated sky/cloud state from being treated as stationary unless
  the environment is explicitly frozen.
- [ ] Define bounded behavior for extremely long accumulation and sample-count
  overflow.

Acceptance:

- [ ] At 1, 2, 4, 8, 16, 32, and 64 samples, frozen-scene variance decreases
  monotonically and the mean stays within the fresh-ray reference confidence
  interval.
- [ ] A camera, geometry, material, light, or environment mutation invalidates
  affected history immediately without one stale frame.
- [ ] A fully unchanged frame does not reset accumulation.
- [ ] Mono and true-stereo tests show no cross-eye history reuse.

## P2 — Real adaptive sampling and work scheduling

The existing variance branch that may add one emissive sample is not sufficient.
Adaptive sampling must reduce dispatched work while preserving the reference
mean.

- [ ] Track per-pixel, per-lobe sample count, first moment, second moment,
  variance, and confidence.
- [ ] Separate direct, diffuse-GI, reflection, alpha/exact, and complex-path
  convergence decisions.
- [ ] Convert `adaptive_min_samples`, `adaptive_max_samples`, and variance
  reference settings into actual queue/sample allocation.
- [ ] Allocate additional samples to high-variance pixels rather than applying a
  single global count.
- [ ] Compact converged pixels out of direct, GI, and reflection queues.
- [ ] Preserve periodic refresh for dynamic lighting and glossy transport so a
  stale sample cannot remain indefinitely.
- [ ] Guarantee a minimum fresh-sample rate for every supported estimator.
- [ ] Reset only the affected lobe when its inputs change.
- [ ] Expose requested, dispatched, skipped, and completed samples per lobe.
- [ ] Add a fixed-ray-budget mode for fair estimator comparisons.

Acceptance:

- [ ] Adaptive off/on captures have statistically compatible means against the
  fresh-ray oracle.
- [ ] Adaptive mode reduces total traced rays and `ray_effects` time on Bistro.
- [ ] High-variance emissive, glossy, disoccluded, and alpha regions receive
  more work than converged diffuse regions.
- [ ] No stable pixel starves, freezes, or develops periodic refresh flicker.

## P3 — Explicit hybrid raster/ray ownership

Current Flux rasterizes the primary surface but ray tracing owns most lighting.
The intended hybrid split must be prototyped and measured before it replaces
the current estimator.

- [ ] Document a per-term composition equation for analytic direct diffuse,
  analytic direct specular, emissive direct, environment, solar, GI,
  reflections, transmission, AO/contact, and visibility.
- [ ] Prototype raster-owned analytic direct diffuse/specular lighting using the
  same light records, energy, color, attenuation, masks, and material BRDF as
  the ray path.
- [ ] Let ray-traced visibility replace or modulate the matching raster shadow
  term exactly once.
- [ ] Keep emissive-triangle, environment, indirect, and unsupported-material
  terms ray-owned unless a separately validated raster approximation exists.
- [ ] Define behavior for area, omni, spot, directional, negative, baked, and
  shadow-disabled lights.
- [ ] Define the fallback when raster primary material data is incomplete.
- [ ] Remove stale comments or diagnostics that disagree with actual term
  ownership.
- [ ] Expose the active owner of each lighting term in diagnostics.
- [ ] Add a debug decomposition view that sums to final HDR color.

Acceptance:

- [ ] A colored analytic-light fixture matches the all-ray oracle within the
  documented BRDF and visibility tolerance.
- [ ] Turning a term's owner from ray to raster does not change mean energy or
  count it twice.
- [ ] The Bistro prototype reduces variance and ray work before adoption.
- [ ] Raster, Metal Flux, and future Vulkan/RTX implementations share semantic
  light/material contracts rather than vendor-specific behavior.

## P4 — ReSTIR DI correctness and effectiveness

Reservoir activity is not success. ReSTIR DI must reduce error or ray work at a
fixed cost without biasing energy or producing temporal instability.

- [ ] Preserve complete selected-sample payload: stable source identity, source
  type, sampled position/direction or parametric coordinates, proposal domain,
  source proposal PDF, target value, weight sum, represented M, and revisions.
- [ ] Recompute destination target and proposal PDFs at the current receiver.
- [ ] Trace current destination visibility for the selected sample.
- [ ] Validate temporal reuse using current/previous surface identity, depth,
  normal, material, motion, disocclusion, light revision, and environment
  revision.
- [ ] Validate spatial reuse with receiver compatibility and deterministic
  neighbor selection.
- [ ] Keep reservoir M and weights bounded without hiding fireflies through an
  undocumented energy clamp.
- [ ] Give emissive triangles, analytic finite lights, solar, and environment
  proposals explicit mixture PDFs and full support.
- [ ] Keep directional lights on the exact analytic path until their solid-angle
  proposal is complete.
- [ ] Add thousands-of-lights, colored-area-light, moving-light,
  appearing/disappearing-light, occluder-motion, and disocclusion fixtures.
- [ ] Report accepted, selected, rejected, visibility-tested, fresh-fallback,
  invalid-PDF, zero-target, stale-identity, and effective-ray-reduction counts.

Acceptance:

- [ ] DI on/off means agree with the fresh-ray oracle within statistical
  confidence and all invalid/nonfinite counters remain zero.
- [ ] At fixed rays and time, DI reduces variance or error on Bistro and the
  thousands-of-lights fixture.
- [ ] Temporal sequences have no lamp power spikes, position jumps, moving
  shadows, tile energy changes, or persistent stale samples.
- [ ] Reuse is enabled in normal rendering only after these gates pass.

## P5 — Indirect reuse: ReSTIR GI, reusable paths, and caches

Evaluate these as separate hypotheses. Do not label reusable-path cache activity
as complete ReSTIR GI.

- [ ] Give ReSTIR GI an independent surface-sample reservoir contract or remove
  the ReSTIR GI completion label from prototype reusable-path diagnostics.
- [ ] Store complete secondary endpoint geometry, material, throughput, PDF,
  represented M, stable identity, and scene/light/environment revisions.
- [ ] Reevaluate current endpoint lighting and reconnection visibility.
- [ ] Validate Jacobians/measure conversion and source-to-destination weighting.
- [ ] Keep at least one supported fresh proposal wherever reuse lacks full
  support.
- [ ] Measure correlation, bias, disocclusion, memory, and GPU time separately
  for ReSTIR GI, generalized path reuse, and a world-space radiance cache.
- [ ] Replace the ineffective current reusable-path query pattern or fail it
  closed when selected reuse remains negligible.
- [ ] Add dynamic geometry, moving emissive, glossy indirect, thin occluder,
  indoor/outdoor transition, and stereo-occlusion fixtures.
- [ ] Preserve independent per-eye screen-space reservoirs and temporal state.
- [ ] Permit shared world-space scene/cache data only when the sample contract is
  view-independent.

Acceptance:

- [ ] Each technique independently beats fresh transport at equal error, rays,
  or time on at least one target scene without regressing the others.
- [ ] Reuse counters correspond to actual fresh-ray replacement.
- [ ] No technique ships merely because it has nonzero query/accept counters.
- [ ] Any biased mode is explicitly named, bounded, and compared with an
  unbiased progressive reference.

## P6 — Bistro cost reduction

Optimize only after the corresponding correctness gate passes.

### Alpha traversal and materials

- [ ] Break out primary, visibility, GI, reflection, and reconnection alpha
  candidate/intersection costs.
- [ ] Validate opaque/empty/mixed occupancy classification against exact alpha.
- [ ] Reduce exact-alpha work using safe material/geometry classification.
- [ ] Measure vegetation and thin-geometry subsets independently.

### Geometry and acceleration structures

- [ ] Resolve ray-proxy fail-open cases, especially invalid containment.
- [ ] Record admitted, culled, proxied, alpha-retained, and dynamic triangles.
- [ ] Measure deformation, BLAS build, BLAS refit, TLAS update, and traversal
  separately.
- [ ] Validate that LOD/proxy changes do not cause stale geometry, shadow leaks,
  or missing reflection/GI occluders.

### Dispatch and shading

- [ ] Reduce the direct-only population through validated raster ownership,
  reservoir reuse, or convergence compaction.
- [ ] Dispatch GI/reflection only for pixels that need those lobes.
- [ ] Measure internal resolution independently from target resolution and
  reconstruction.
- [ ] Improve emissive proposal quality for Bistro's 2,812 emissive triangles.
- [ ] Measure environment, solar, analytic, and emissive candidate costs
  independently.
- [ ] Remove CPU/GPU synchronization, allocation, or readback from steady-state
  rendering where diagnostics do not explicitly request it.

Acceptance:

- [ ] Every accepted optimization includes before/after stage timings, ray and
  intersection counts, error versus progressive reference, memory, and exact
  benchmark conditions.
- [ ] No optimization passes by lowering undocumented resolution, material,
  light, alpha, bounce, or stereo quality.
- [ ] The frozen RTX 5080 production profile eventually reaches complete-frame
  p99 <= 11.11 ms for two distinct stereo views without frame generation.

## P7 — Reconstruction, parity, and shipping gates

- [ ] Raw diagnostic mode remains available and never silently enables MetalFX.
- [ ] Interactive reconstructed mode identifies MetalFX or another active
  reconstructor truthfully in diagnostics.
- [ ] Progressive mode does not run a second temporal filter over its accumulated
  reference result.
- [ ] Add matched Vulkan/RTX implementation and replay for every accepted Metal
  feature.
- [ ] Validate stereo camera state, jitter, motion, disocclusion, reservoir,
  reconstruction, and temporal history independently per eye.
- [ ] Run mono, stereo-static, stereo-occlusion, camera-motion, dynamic-geometry,
  and moving-light sequences on both backends.
- [ ] Record separate memory for scene data, distributions, ReGIR/cache data,
  per-eye reservoirs, accumulation, guides, and reconstruction.
- [ ] Update the M2.5 implementation journal after every completed gate with the
  exact evidence and next executable item.

## Required result table for every experiment

| Field | Required value |
|---|---|
| Source state | Godot and Danger Room revisions plus dirty-tree disclosure |
| Platform | OS, SDK, compiler, GPU, driver/runtime |
| Scene | Exact scene/resource revision and frozen/moving phase |
| View | Resolution, internal resolution, mono/stereo, camera transform |
| Sampling | Sequence, seed, samples per lobe, bounces, reuse flags |
| History | Accumulation age, validity, reset reason, per-eye owner |
| Work | Rays and intersections per lobe/stage |
| Reuse | Queries, valid, accepted, selected, fresh work replaced |
| Correctness | Mean/error versus oracle, variance, invalid/nonfinite counters |
| Performance | CPU submit and every GPU stage; median and p99 |
| Memory | AS, scene records, reservoirs/caches, accumulation, guides |
| Artifact | Logs, raw captures, guides, diagnostic snapshot |

## Definition of done

This checklist is complete only when:

- [ ] Frozen raw Bistro noise is fine-grained and statistically correct.
- [ ] Progressive Bistro converges monotonically without stale history.
- [ ] Adaptive sampling reduces real work at equal reference error.
- [ ] Hybrid composition has explicit single ownership for every lighting term.
- [ ] ReSTIR DI passes correctness and produces measured benefit.
- [ ] Every enabled indirect-reuse technique produces measured benefit and has a
  truthful name and contract.
- [ ] Metal and Vulkan/RTX pass matched correctness and stereo fixtures.
- [ ] The RTX 5080 true-stereo production profile sustains 90 Hz without frame
  generation and without hidden quality reductions.

