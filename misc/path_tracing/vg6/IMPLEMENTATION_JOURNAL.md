# VG6 implementation journal — native submission and Vulkan parity

## Status

The native Metal raster-submission slice is implemented and validated. VG6 as
a whole is not accepted: the repository still lacks a virtual-geometry Vulkan
raster/ray consumer, Windows validation, physical-XR stereo validation, and a
canonical representation for Danger Room's custom `ShaderMaterial` in the ray
path.

## 2026-08-24 native Metal raster evidence

- Pure-VG primary visibility was reaching Metal with valid identity, depth, and
  material pixels, but Flux's primary-surface shader always exported authored
  `EMISSION`. `StandardMaterial3D` in unshaded mode owns its final radiance in
  `ALBEDO`, so the later Metal composition replaced those valid surfaces with
  zero. `MODE_FLUX_PRIMARY_SURFACE` now exports albedo for `MODE_UNSHADED` and
  retains emission for shaded materials. The installed root and `SubViewport`
  fixture passes its exact-background gates (far coverage `0.042667`, near
  `0.325186`, five material regions, and deterministic far return). Danger
  Room's unchanged corridor now has nonzero foreground in both viewport modes,
  but its framing gate still fails: foreground coverage is about `0.0013` to
  `0.0015`, below the project's `0.10` threshold. Shaded-VG direct lighting,
  Danger Room framing, ray material parity, Vulkan, and physical XR remain
  separate unaccepted gates.

- Native VG indirect commands, page residency, and descriptor checks can all
  succeed while producing no color or depth if manually appended `InstanceData`
  retains its zero-initialized fade byte. Flux derives per-instance alpha from
  that byte; authored opaque-prepass materials then discard every alpha-zero
  fragment in the depth pass. VG now explicitly sets the normal opaque fade
  value (`255 << 24`) before indirect submission. The root and `SubViewport`
  fixture now gates pure-VG far/near/far captures against their exact clear
  sample for foreground coverage, bounding-box extent, local contrast, and at
  least three distinct material hue regions. The conventional reference stays
  separately framed and is not present during those gates.

- A pure virtual-geometry viewport previously crashed while appending Flux
  instance data. `_fill_instance_data()` maps the opaque UMA buffer only when
  the conventional opaque list contains elements; consequently the Danger Room
  candidate corridor could reach a valid indirect command and then write 15 VG
  records through a null `curr_gpu_ptr`. The VG append path now maps its newly
  grown buffer explicitly when the conventional base is zero, while retaining
  the normal refill path for mixed conventional/VG lists.
- The installed regression now passes both root swapchain and `SubViewport`
  (`UPDATE_ALWAYS`) phases with five freshly serialized/reloaded resources, 15
  instances, exactly 10 shared instances, no conventional mesh in the pure-VG
  phases, unload/reload, and far-near-far movement. The Danger Room root
  corridor also completed all three captures without SIGSEGV: far phases used
  9 VG instances/3 unique resources/3 page units, the near phase used 3/1/1,
  the arena remained at 96 of 65,536 records, command readback remained nonzero,
  and far-return image MAE was `0.0000149054`.
- The Danger Room validation still exits nonzero for a project-side assertion:
  its selected near cell contains only one unique resource while the script
  requires two. Cleanup also reports an invalid cast in the corridor streamer's
  `_exit_tree()`. These occur after successful far-near-far rendering and are
  not renderer crashes; the project was left unchanged.
- The VG1 `VirtualGeometry` resource now persists a versioned, canonical
  little-endian package blob containing the manifest, source identities,
  topology/compiler generations, hierarchy/material descriptors, bounds, and
  compressed page bytes. Decode is atomic and validates all ranges, page
  hashes, compression round trips, vector bounds, and boolean encodings before
  replacing the in-memory package; runtime residency is intentionally absent.
  Material bindings remain a separate storage property. The root cause of the
  reload probe's empty package was that only those bindings were exposed to
  ResourceSaver, so ResourceLoader reconstructed a resource with no source
  identities or pages. The fix is covered by a fresh ResourceSaver/Loader
  round-trip, corrupt-blob rejection, stable repeat-save blob/file checks, and
  an explicit legacy-empty invalid/no-crash case. Package revision is preserved
  and remains unchanged when material bindings are edited. The freshly
  installed editor also ran Danger Room's serialization probe from an isolated
  temporary project: source and fresh reload were valid, both source identities
  were preserved (`87123` and `44101`), and both runtime revisions were `3`.

- The dedicated `VirtualGeometry` resource/instance path now submits bounded
  fixed-slot indirect indexed draws through Flux. Descriptor snapshots publish
  only after completion, share a logical vertex-slot allocator, zero retired
  slots, and retain persistent coarse fallback while fine groups stream.
- Raster demand is the deterministic union of every supplied eye. Refine and
  coarsen thresholds have stateful hysteresis; page interest, retry, LRU
  eviction, and retirement are bounded by the storage heaps and completed RD
  serials.
- Semantic instance identity is an explicit nonzero signed 64-bit property,
  independent of Godot object/picking IDs, and survives resource revisions.
- On an Apple M4 Max using native Metal, the compiled
  `VirtualGeometryInstance3D` fixture transitioned from 3 resident pages and 4
  selected commands to 32 pages and 18 commands near the camera, then to 15
  pages and 4 commands after coarsening. The frame arena peaked at 184 of
  65,536 records. Indirect-command readback reported 4 nonzero commands and 0
  rejected commands. The mirrored conventional `ArrayMesh` comparison passed
  (`parity_difference=0.004123`, `motion_parity=0.004237`).
- Focused source tests passed: 35 cases, 10,525 assertions, including the
  32-assertion fresh resource serialization case. They cover fixed
  descriptor slots and generations, allocator reuse/alignment, stale zeroing,
  demand/fallback/hysteresis/eviction, semantic identity revisions, bounded
  commands, and a rotated two-frustum union.
- The freshly built editor was installed at `/Applications/Godot.app`, ad-hoc
  signed, and verified as `4.8.dev.custom_build.d368d00bf` with executable
  SHA-256 `ad38fcca9d301931c5809f5cf6205140f281e9967574e35091a7b4acf309eb6e`.
  The installed executable repeated the native fixture result and opened the
  Danger Room project under Flux with no reported engine or script errors.

The rendered fixture exercises geometry, UV-capable material inputs, instance
motion, and previous-stream wiring. It does not establish exact per-pixel
normal/UV/depth equivalence. Rotated stereo union is currently a deterministic
CPU contract test, not a physical XR display result.

The Metal ray-group path remains independently selected and rejects visibility
masks that cannot be represented without truncation. It admits only materials
whose canonical albedo/metallic/roughness parameters are available. Danger
Room's custom `ShaderMaterial` is therefore explicitly unsupported rather than
being rendered with invented fallback semantics.

## Implemented prerequisites

- VG1 provides deterministic ordinary triangle clusters, deep hierarchy
  metadata, material/opacity/sidedness semantics, stable identities, checked
  page payloads, and persistent coarse roots in
  `servers/rendering/virtual_geometry/virtual_geometry_format.h` and the
  meshoptimizer compiler.
- VG2 provides renderer-owned residency, decoded position/index/attribute
  heaps, descriptor publication, atomic group activation, GPU completion
  serials, and deferred retirement. The scene boundary remains one
  `VirtualGeometryInstance3D`; pages are not scene nodes.
- VG3 provides the product-neutral projected-error/stereo selection contract
  and a bounded compute/indirect indexed native Flux path on Metal. The path
  uses explicit supported-material admission rather than conventional mesh
  compatibility or hidden material fallback.
- VG4 provides transport-role selection, stable near/middle/far ray-group
  hints, coarse ray fallback, and a native Metal primitive-AS lifecycle. Its
  journal records the local Metal build and focused tests; it makes no Vulkan
  parity claim.
- The Vulkan driver already exposes ordinary indirect indexed draws and
  `VK_KHR_acceleration_structure`-backed BLAS/TLAS primitives through
  `drivers/vulkan/rendering_device_driver_vulkan.{h,cpp}`. Those are generic
  device capabilities, not a virtual-geometry implementation: the current
  interface has no virtual page/group scheduler, compacted static BLAS
  lifecycle, complete-group swap contract, or virtual-geometry diagnostics.

## Evidence and missing work

The architecture requires the Vulkan adapter to consume the same logical
clusters and selection rules as Metal, with compute/indirect raster as the
portable fallback and ordinary KHR BLAS/TLAS as the cross-vendor ray
baseline. The following have not been implemented or validated here:

- Vulkan shader/pipeline and frame integration for virtual raster selection,
  descriptor-driven heap fetch, material batching, stereo masks, and the
  persistent coarse raster fallback.
- A Vulkan ray-world scheduler and ordinary KHR BLAS/TLAS owner with explicit
  scratch budgeting, asynchronous completion, compaction, complete group
  substitution, TLAS updates, deferred destruction, and last-trace-use
  retirement.
- Shared Vulkan diagnostics and capture/replay records for selected raster
  cuts, transport tiers, page misses, BLAS/TLAS state, fallback reasons, and
  per-stage timings.
- Windows execution on the target Vulkan/RTX configuration, including the
  representative street/roof/canopy/night/traversal and true-stereo gates.

No Vulkan performance, visual-parity, or 90 Hz result is claimed. The local
Mac Metal tests cannot establish this milestone.

## Why blocked

The architecture intentionally leaves TLAS role layout, Vulkan mesh versus
compute/indirect raster choice, and ordinary BLAS grouping as measured backend
decisions. The current environment has no Windows Vulkan/RTX runtime or
RTX 5080 test target, so those choices cannot be certified against the
required stage timings and semantic tolerances. This is an external
validation blocker, not permission to silently select a Metal-specific or
unmeasured Vulkan design.

## Next executable steps

1. Implement the Vulkan virtual-raster consumer using the existing VG2 heaps
   and VG3 selection contract; retain compute/indirect as the declared
   capability fallback.
2. Add the ordinary KHR ray-group owner and lifecycle, then exercise atomic
   coarse/detail swaps and deferred retirement in a deterministic source
   fixture.
3. Run identical Metal/Vulkan fixtures on Windows, record separate raster,
   BLAS, TLAS, trace, reconstruction, and composition timings, and compare
   semantic/visual tolerances before declaring VG6 accepted.
