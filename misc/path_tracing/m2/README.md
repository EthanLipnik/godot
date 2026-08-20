# Godot path-tracing M2 — PC-independent foundations

M2 prepares deterministic character, pose, interaction, companion, and deployment inputs without claiming Windows, CloudXR, physical-headset, or dual-renderer success.

## Character preparation

`run_character_foundation_gate.sh` accepts an external Blender file, exports the configured runtime mesh twice with embedded scripts disabled, preserves eight weights and all 35 morphs, and validates byte identity. `inspect_character_source.py` records the complete deform skeleton, explicit canonical retarget map, 70 Blender driver definitions, UDIM tile hashes, color spaces, missing paths, DirectX-to-OpenGL normal conversion, first-/third-person visibility policy, and license notes. The Pillow-based conversion step emits an array-ready tile set, copies color/data textures byte-for-byte, inverts green for normal tiles, and hashes every source/output. Set `M2_PYTHON` to a Python with Pillow installed. Generated GLBs, converted textures, and reports remain ignored and no restricted asset is committed.

The checked-in profile is test configuration, not a distributable character. The exported GLB deliberately omits materials; the source evidence is the deterministic conversion manifest consumed by later texture bake/renderer work.

## Pose, interaction, and Metal validation

The validation project contains reusable reference implementations for:

- smooth clamped pose-feature-to-morph evaluation across 35 simultaneous correctives;
- confidence-aware filtered two-bone arm solving and 26-joint finger filtering;
- a debounced tracked pinch/tether state machine; and
- combined eight-weight skeleton plus morph deformation.

Its Metal editor gate renders the final deformation through the production scene compiler, dynamic BLAS refit, mirror secondary path, full guides, and MetalFX. A large silhouette corrective is reserved for renderer correctness; the other morph deltas exercise millimeter-scale wrinkle behavior.

## Companion and endpoint contracts

`companion` is a Swift package containing independently versioned endpoint messages, telemetry, and a strict discovery/pairing/connect/reconnect state machine. Tests run on macOS. A minimal visionOS 27 shell is type-checked against the installed primary `FoveatedStreaming.framework` interface and uses system discovery, hand authorization, connect, disconnect, and the framework-provided immersive space. It is a compile-validated shell, not pairing, CloudXR, entitlement, or device evidence.

The credential-free Windows manifest/argv generator and mocked endpoint ordering delivered in M1 remain the deployment foundation for M2.

## Gates

```sh
misc/path_tracing/m2/run_character_foundation_gate.sh /path/to/source.blend
misc/path_tracing/m2/companion/run_companion_gate.sh
bin/godot.macos.editor.dev.arm64 --display-driver macos --rendering-driver metal --audio-driver Dummy --path misc/path_tracing/m2/validation_project -- --validate-m2-pose-gesture
bin/godot.macos.editor.dev.arm64 --display-driver macos --rendering-driver metal --audio-driver Dummy --editor --path misc/path_tracing/m2/validation_project res://pose_gesture_validation.tscn -- --validate-path-tracing-editor
```

Run Metal gates with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` for the recorded conditions.
