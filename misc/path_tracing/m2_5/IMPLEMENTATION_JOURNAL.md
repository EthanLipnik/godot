# Flux Metal implementation journal

Last updated: 2026-08-25

## 2026-08-25 STBN and finite-light reservoir correctness patch

Removed the per-128x128 STBN Z-slice phase. It made each output tile advance
through a different temporal slice, so whole tiles changed together and looked
like flickering light. STBN remains the sole noise source with one canonical
frame/semantic/sample phase: `frame + semantic*13 + channel*29 +
sample_index*17 + sample_count*7 (mod 64)`. Virtual tiles now use the
frame-invariant toroidal translation `offset.x=(37*tile.x+73*tile.y) mod 128`,
`offset.y=(56*tile.x+29*tile.y) mod 128`, shared by CPU replay and Metal.

Focused sampling and Metal source tests pass (12 cases, 4227711 assertions).
The incremental macOS arm64 dev editor bundle build also passes. A bounded
real-Metal validation-project warmup with `--rendering-method flux`, canonical
STBN enabled, DI/reusable-path enabled, and ReGIR disabled loaded STBN v3
`128x128x64x4` (checksum `3995566716328164170`) and completed without shader
errors. It reported `cold_gi=25998`, `warm_gi=25712`, `temporal_di=208`,
`reused=408`, `queried=4712`, `regir_valid=0`, `regir_accepted=0`, and
`regir_selected=0`.

Unified finite-light reservoirs now carry the generated rectangular-light UV in
the persistent reservoir payload. Reused area sources evaluate that exact point
at the current receiver using an area-measure integrand with q=pL/A; the
ordinary analytic area-light path keeps its existing area compensation.
Directional lights are excluded from unified finite-light proposals and remain
in analytic direct lighting exactly once.

This file records the current implementation, verified boundaries, open failures,
and next work. Superseded chronological experiments and old performance numbers
were intentionally removed; they remain available in repository history.

## Current renderer boundary

Flux is a separate RenderingDevice renderer with its own raster buffers and a
native Metal ray scheduler. The implementation is currently Metal-only. Vulkan/
RTX parity and stereo performance are not complete and must not be inferred from
the macOS prototype.

Current primary implementation:

- `servers/rendering/renderer_rd/flux/render_flux.{h,cpp}`
- `servers/rendering/renderer_rd/flux/metal_flux_effect.{h,cpp}`
- `servers/rendering/path_tracing/`

The architecture hypothesis remains
`ADVANCED_RENDERING_ARCHITECTURE_HYPOTHESIS.md`. This journal describes observed
implementation state, not a second architecture specification.

The executable Bistro convergence, adaptive-sampling, hybrid-lighting, reuse,
and performance gates are tracked in
`misc/path_tracing/m2_5/BISTRO_CONVERGENCE_AND_SCALING_CHECKLIST.md`. That file is
the check-off tracker for this investigation; evidence and implementation
decisions still belong in this journal.

## Working transport

The Metal backend currently provides:

- Flux raster primary visibility and material guides.
- Native Metal BLAS/TLAS build, refit, reuse, and deferred resource retirement.
- Ray-owned analytic and emissive direct lighting with destination visibility.
- Environment importance sampling and procedural-sky solar lighting.
- One-bounce diffuse indirect lighting.
- GGX reflection rays with secondary material evaluation.
- Exact alpha intersection handling where required.
- Ordered direct, GI, reflection, exact-alpha, and complex trace queues with
  separate stage diagnostics.
- Material, geometry, residency, light, shadow, and environment-content revision
  tracking for transport invalidation.

MetalFX Temporal Denoised is the sole production image-space denoiser and
reconstructor. Flux does not run a second spatial or temporal color filter.
When MetalFX is disabled, Flux composes raw transport.

## Correctness cutover

The 2026-08-25 cutover fixed directionally incorrect lighting and shadows caused
by treating cached source-domain values as destination-domain answers:

- Cached direct visibility is no longer copied to a different receiver. Selected
  direct proposals trace current destination visibility.
- Reusable-path radiance cannot replace current GI without a complete endpoint,
  measure, PDF, material, revision, and reconnection contract.
- The camera-relative diffuse radiance cache is disabled because it did not carry
  sufficient world-cell and surface identity and was updated unsafely in the
  tracing dispatch.
- Environment radiance-content revision is distinct from importance-distribution
  revision.
- ReGIR and reusable-path CPU/Metal layouts have exact size and alignment guards.

The diagnostic mode `--flux-fresh-ray-oracle` disables reuse, Flux history,
MetalFX, and reconstruction while retaining fresh direct, GI, and reflection
transport. It is the correctness baseline, not the intended shipping mode.

## Reuse features currently fail-closed

These paths remain implemented or scaffolded but are deliberately disabled in
the Metal production path:

- Temporal/spatial ReSTIR DI reuse.
- ReGIR direct-light reuse.
- Reusable world-path samples.
- GPU diffuse radiance-cache consumption and update.

They were disabled because their old records lacked one or more of: exact source
and destination proposal PDFs, receiver compatibility, current endpoint lighting,
current reconnection visibility, stable world/surface identity, or synchronized
cache updates. Fresh current-frame direct candidates remain active.

Re-enable each feature only after an isolated fixture proves unbiased current-
receiver evaluation, immediate invalidation, deterministic behavior, and a
measured reduction in fresh ray work.

## Sampling

Flux has two sampling modes:

1. `0` — progressive Owen-scrambled low-discrepancy sampling.
2. `1` — scalar spatiotemporal blue noise (STBN).

The project setting is `rendering/flux/ray_tracing/sampling_sequence`. The engine
default is currently `0`, so STBN is available but is **not** the default.
`--flux-stbn-enabled` and `--flux-stbn-disabled` provide deterministic A/B
overrides.

STBN uses a generated 32x32x64, four-channel R16Uint rank volume. It is intended
for low-sample real-time transport feeding MetalFX. Owen-scrambled sampling is
retained for progressive/reference work.

## Active visual failure

Raw Flux transport now has correct light and shadow direction, but Cornell and
dynamic-sky editor views still show a frame-changing checkerboard/horizontal-band
pattern. This is not acceptable Monte Carlo noise and must not be hidden by
MetalFX.

The cause is not yet proven. Investigation must isolate:

- STBN/Owen pixel and dimension indexing.
- Compact trace-queue pixel coverage.
- Unwritten or partially written transport pixels.
- Raw output resolution and composition mapping.
- MetalFX input coverage and guide alignment.

Acceptance requires fine-grained unbiased raw noise without coherent missing,
replicated, or scanline-shaped regions. MetalFX should then denoise that corrected
signal.

## Performance status

There is no current city-scene performance claim after the correctness cutover.
Earlier Bistro measurements used reuse paths that are now disabled or were later
shown to be invalid. They are historical debugging evidence, not the current
performance baseline.

New Bistro, Zero-Day, and Emerald measurements are meaningful only after the
structured raw-noise failure is fixed and each reuse feature passes correctness
gates.

## Latest verified build

The fresh macOS editor was built with:

```text
scons platform=macos target=editor arch=arm64 dev_build=yes tests=yes \
  metal=yes vulkan=no module_meshoptimizer_enabled=yes generate_bundle=yes -j4
```

Installed executable:

```text
/Applications/Godot.app/Contents/MacOS/Godot
SHA-256 c767c9d417dab98cd07bbbc28aaf7191e767f59a93446c375c43f8387e461252
```

The focused Flux Metal source contracts passed 22 cases and 398 assertions. A
bounded real-display Metal fresh-ray-oracle run exited normally. This verifies
the installed correctness baseline; it is not a visual-quality or performance
gate for the structured-noise issue above.

## Next executable work

Use `BISTRO_CONVERGENCE_AND_SCALING_CHECKLIST.md` as the ordered executable work
list. Start at P0: establish the deterministic frozen Bistro fixture and rerun
the full raw/oracle/reuse matrix with the current STBN and finite-light fixes.
Do not advance an estimator or optimization past its named acceptance gate.

## Reuse contract implementation (2026-08-25)

The Flux reuse gates are now enabled behind the existing request flags. Direct
reservoir reuse carries source proposal/target/weight state and applies the
source-to-destination PDF correction before current selected visibility. Spatial
neighbors are selected from canonical STBN. ReGIR reduction is weighted and
serial per cell, with a 224-byte cell carrying revisions and receiver snapshot
storage. Reusable paths use four bounded STBN-selected staging slots per cell,
immutable previous storage, current endpoint reevaluation, and reconnection
visibility; cached radiance is not consumed. Fresh DI/GI candidates remain in
every reservoir and invalid proposals reject independently.

The macOS editor build with Vulkan disabled completed before the final CPU
selection-only cleanup. Runtime Metal shader compilation and isolated ray-count
comparisons remain the next validation gate; no installed editor was changed.

## Runtime audit correction (2026-08-25)

The combined warmup audit showed that the prior enabled prototype did not
replace fresh GI work (`cold_gi=25998`, `warm_gi=25712`, `temporal_di=0`). All
three support constants are therefore fail-closed again. The correction removes
the dead zero-target path query, keeps one fresh GI anchor while allowing
configured additional samples to be represented by explicit `represented_m`,
uses weighted STBN K=4 staging reduction, reevaluates emissive/punctual/solar/
environment endpoint transport, and recomputes current direct-light proposal
PDFs. Shared ReGIR and reusable-path CPU contracts now validate weighted merge
fields and explicit represented M. Re-enable remains gated on isolated real-
Metal acceptance and measured ray replacement.

## Isolated real-Metal validation (2026-08-25)

Built with `scons platform=macos target=editor accesskit=no angle=no vulkan=no metal=yes tests=yes generate_bundle=yes -j4`; focused path/reuse/source tests passed (40 cases, 1082 assertions). The 640x360 fixture used two GI samples so reusable paths could retain one fresh anchor. Fresh oracle reported 51424 warm GI rays. Reusable-path-only reported 25712 warm GI rays, 247 accepted and 155 selected cached candidates, 155 reconnection visibility rays, and mean absolute RGB difference 0.0032938680853 versus the matched oracle; no Metal shader errors occurred. Combined reported 25712 warm GI rays, 375 accepted and 237 selected cached candidates, and 237 reconnection visibility rays.

Temporal/spatial DI remained `temporal_reuse_count=0`, `spatial_reuse_count=0`; ReGIR had no observable weighted reuse replacement counters, so both remain fail-closed with their warnings. Final support gates are `DI=false`, `ReGIR=false`, `reusable_path=true`.

## DI history/PDF and ReGIR diagnostics correction (2026-08-25)

Split direct-reservoir history validity from the MetalFX image-history bit and
persist the exact source proposal PDF across reuse generations; current DI
proposal PDFs are recomputed per receiver and selected visibility remains
current-frame only. Added exact parameter ABI offsets and ReGIR classify,
reduce, query, and weighted-merge counters/status readback. The macOS Metal
fixture then showed DI temporal/spatial reuse `208/219`, current visibility
tests `24630`, and zero invalid-PDF/nonfinite/rejected-energy samples. ReGIR
classification/reduction was active (`24551` classified, `8` reduced cells),
but query-valid and weighted merge counters stayed zero, so ReGIR remains
fail-closed. Final gates are `DI=true`, `ReGIR=false`, `reusable_path=true`.

## ReGIR completion-gated grid correction (2026-08-25)

ReGIR previously queried the same pending grid that scroll/classify/reduce
updated, while CPU setup flipped its current index before GPU completion. The
fix keeps the last completed grid immutable for trace queries, writes a
separate pending grid, skips overlapping updates with an atomic in-flight
owner, and promotes the pending index only from the Metal command-buffer
completion handler. The completed-frame fixture then observed `21354` valid
queries, `20285` accepted merges, and `19904` selected merges with zero PDF
rejects. It also observed `1069` current-receiver zero-target rejects, so the
ReGIR support gate remains false. DI and reusable-path combined warmup still
passes: `cold_gi=25998`, `warm_gi=25712`, `staged=29`, `queried=4468`,
`reused=613`, `temporal_di=208`, and `reconnection_visibility=480`.

## ReGIR destination-zero acceptance (2026-08-25)

The destination-zero audit confirmed that the `1069` zero-target outcomes occur
after finite committed-cell, source-PDF, and current destination-PDF checks;
they are current-receiver `h_d == 0`, not missing cell payloads. The merge status
leaves the fresh direct anchor unchanged and records explicit fresh fallback
count (`75` in the validation frame). Key, revision, and payload rejection
diagnostics are now separate from destination-zero, so stale or malformed cells
cannot be conflated with a valid zero contribution.

The completed-frame Metal capture reported weighted ReGIR classification/reduce
and query/merge activity, with zero PDF/nonfinite/rejected-energy samples. A
matched two-sample oracle comparison measured mean absolute RGB difference
`0.00405897113606`; the fixture produced no comparison failure.
The stable combined warmup remained valid: `cold_gi=25998`, `warm_gi=25712`,
`queried=4788`, `reused=246`, `temporal_di=213`, and
`reconnection_visibility=151`. ReGIR is enabled; it replaces proposal work and
does not claim a selected-visibility-ray reduction on this one-light fixture.

## Reusable-path normalization fail-close (2026-08-25)

The reusable-path source reservoir still lacks a validated normalization
contract. Its established Metal capability gate is now false, and the scheduler
reports `invalid_source_reservoir_normalization` once when the project setting or
validation override requests the feature. The request is cleared before shader
feature admission, so bit 4 is not submitted and the reusable-path staging,
query, accepted, selected, and reconnection counters remain zero. GI dispatch
uses the configured sample count and normalization unconditionally; STBN,
ReSTIR DI, ReGIR, emissive NEE, and MetalFX are unchanged.
