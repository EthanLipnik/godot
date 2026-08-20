# Godot path-tracing M2 implementation journal

## 2026-08-20 — PC-independent foundation gate

### Decisions and evidence

- Kept proprietary character data external. A generic Blender process exports only the selected mesh/armature, removes authoring modifiers and materials, prunes and renormalizes to eight deform influences, retains all 35 correctives, disables embedded scripts, and writes hashes/settings/license notes.
- The source evidence resolves UDIM placeholders to all four physical tiles. This corrected an apparent set of 11 missing images; the files existed and Blender stored `<UDIM>` template paths. Pillow 12.3.0 produced and hashed 21 array-ready tiles, including four normal tiles with explicit green inversion from DirectX to the runtime OpenGL convention. Repeat manifest SHA-256 was `6bc697f5c4daa4dab8284c599201be16b1468877473fa87126f75016a6d08686`.
- The canonical retarget profile has 60 unique body/hand entries against 141 deform bones. Fingertips remain derived from distal joints because the source has no separate deform-tip bones.
- A smoothstep/exponential pose driver activated all 35 morphs with a maximum per-frame weight step of `0.0168908834`. The reference arm/finger solver remained finite through recorded tracking loss; maximum elbow step was `0.0176973790`, wrist target error `0.0000000596`, and finger-joint step `0.0078092273`. The debounced tether attached only after three confident frames and released on confidence loss.
- The 35-morph Metal validation exposed weak dynamic-geometry evidence. The existing native test changed its random sample while changing geometry, so output inequality did not isolate BLAS correctness. In-place Metal refit was replaced by refitting from the prior BLAS into a distinct destination BLAS with `AccelerationStructureRefitOptionVertexData`. The native test now holds its seed fixed and produces distinct raw output. The BLAS content hash covers current positions only; previous-position motion history no longer causes false refits.
- The visionOS shell was implemented directly against the installed XROS 27.0 `FoveatedStreaming.swiftinterface`. The primary interface provides `FoveatedStreamingSession`, `.systemDiscovered`, hand capability authorization, and `ImmersiveSpace(foveatedStreaming:)`; directly constructing `FoveatedStreamingSpaceContent` was invalid and was removed after the type-check falsified that assumption.

### Recorded conditions and results

- Godot base revision: `e0f1349bad` plus this M2 change set. Host/toolchain remain the M1 Mac Studio M4 Max, macOS 27.0 `26A5416b`, Xcode 27.0 `27A5237l`, SDK 27.0, Apple clang 21.0.0, Metal `382.5.3`, and MetalFX `40.8`.
- Blender 4.3.2 export: 56,640 vertices, 35 morph targets, 141 deform bones, eight maximum influences, 29,536,204-byte GLB, SHA-256 `15975d811ee4ec1af231bc8625541de9ab39783ea44f86c24b1a01a57b76711f`, byte-identical repeat, 70 source drivers, zero unresolved texture templates.
- Swift 6.2 package: three deterministic protocol/lifecycle/telemetry tests passed; the visionOS 27 app shell type-checked for `arm64-apple-xros27.0`.
- Pose/gesture Metal process: 35/35 morphs activated, combined morph/skinning bake changed geometry, arm/fingers remained within recorded bounds, and the gesture tether passed.
- Editor Metal process: nonzero temporal-denoised output, changed actual-scene current-position hash, one dynamic BLAS refit, finite secondary-hit-distance pixels in each capture, and no material fallback. Raw main-editor readback hashes remain diagnostic because native callback completion is not exposed as a reliable synchronous test boundary; fixed-seed raw-output correctness is covered by the local RenderingDevice test.

### Remaining claims and next step

- The companion has no entitlement, signed app, physical Vision Pro execution, Apple pairing exchange, or CloudXR connection evidence. Those remain M3 gates.
- The external asset and character IP redistribution rights remain unresolved; all generated geometry stays ignored.
- The reference pose solver validates numerical stability, contract shape, confidence loss, and deformation ordering. Product calibration and physical tracked-motion quality still require a headset and user proportions.
- M2 completes the productive PC-independent foundation. The next renderer milestone is M3, blocked on a configured Windows RTX host for Vulkan runtime parity and CloudXR. Mac-only work can still add integrated Metal counter sampling or broaden validation, but cannot satisfy or weaken M3.
