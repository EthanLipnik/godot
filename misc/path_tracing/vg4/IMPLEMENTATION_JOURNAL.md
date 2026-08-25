# VG4 implementation journal — virtual ray structures

## Status

VG4 correctness implementation is complete for the product-neutral contract and native Metal primitive-AS path. It is not a Vulkan/RTX parity claim or a 90 Hz performance claim.

## Strategy choice

Metal uses immutable primitive acceleration structures per stable virtual ray group and one mask-separated TLAS family. A frame submits the persistent far group before middle/near work, then substitutes the highest group whose complete BLAS set is available. This is narrower and measurable on current Metal than adding a second acceleration-structure owner or encoding a vendor structure into the portable package. Fixed-topology refit remains limited to surfaces explicitly declared dynamic; immutable virtual groups rebuild on topology revision.

Stable group identity is derived from source primitive identity, the independent ray-hint schema generation, transport-region/root identity, tier, and sorted cluster identities. It does not include physical page packing, heap offsets, raster traversal order, or the selected raster cut.

## Implemented evidence

- Compiler emits deterministic near/middle/far ray hints before physical page packing. Every occupied transport region has one persistent coarse group.
- Flux gathers virtual transport from renderer virtual-geometry instances and storage ray hints, independently of raster selection. It requests persistent coarse pages first and withholds detail until coarse residency is complete.
- Distance, footprint, roughness, path depth, expected contribution, off-screen influence, and transport role select a desired tier in the product-neutral contract.
- The Metal owner budgets immutable BLAS builds, writes compacted sizes to shared readback buffers, issues compact copies on a later submission, and completion-gates compacted-resource swaps.
- TLAS instance construction atomically chooses one highest complete group per transport region. Failed, deferred, or late detail therefore leaves the conservative coarse structure selected.
- Command-owned resources use the existing `retain_resource` path. Superseded acceleration structures retire only after the completion token covering their last use.
- Diagnostics expose the actual path, selected tier counts, pending/fallback/off-screen counts, compact query/copy/swap/retirement counts, TLAS rebuild/refit/reuse, and compacted/uncompacted bytes.
- CPU tests cover role selection, coarse fallback, off-screen retention, completion/compaction atomicity, failure preservation, and last-use retirement. Native source tests cover the compiler-independent transport feed and Metal completion/retention API path.

## Validation environment

- Hardware: Apple M4 Max, 40 GPU cores, Metal 4
- OS: macOS 27.0 (26A5416b)
- Toolchain: Xcode 27.0 (27A5252f), macOS SDK 27.0
- Build: `/tmp/godot-vg-scons/bin/scons platform=macos target=editor dev_build=yes tests=yes extra_suffix=vg4 vulkan=no angle=no accesskit=no -j8`
- Binary: `bin/godot.macos.editor.dev.arm64.vg4`
- Focused CPU tests: `[Rendering][VirtualGeometry]` and `[Flux][Metal][VirtualGeometry]`
- Unattended rendered fixture: `validation_project`, real Metal driver, `--no-window-focus --audio-driver Dummy`, bounded by `--quit-after 120`

Validation result on 2026-08-24: the unique-suffix build completed; 20 matching VirtualGeometry cases passed with 747 assertions, and the two native VG4 source cases passed with 25 assertions. The unattended Metal fixture exited successfully with a mean RGB difference of `0.02880658301311` after removing the off-screen contributors. The captured one-shot diagnostic reported `path=metal_primitive_as_masked_tlas`, TLAS reuse, and separate GPU stages (`ray_effects=64.523 ms`, `spatial_reconstruction=9.312 ms`, `composition=0.741 ms`). These are correctness-fixture timings at 640×360, not performance results.

## Fixture contract

The product-neutral scene contains an off-camera emissive surface and light, an off-camera blocker, a sharp mirror, and an off-camera reflected contributor. Removing those contributors after convergence must measurably change the rendered result. Tier substitution itself is deterministic and covered by the CPU lifecycle test because compiled virtual packages are intentionally not constructible through GDScript.

## Remaining gates

- Measure separate build, compact, TLAS, and trace stage timings on production-scale content.
- Implement and validate the same semantic contract on Vulkan/RTX before renderer parity can be claimed.
- Run the documented true-stereo production performance gate on the target RTX 5080; VG4 makes no 90 Hz claim.
