# M3 preparation journal

## 2026-08-20 — Mac-side certification preparation

Decision: M3 remains the first Windows- and physical-device-dependent milestone. This work prepares its deterministic boundary and is not an M3 implementation or pass.

Evidence:

- Local Godot Vulkan code already provides optional capability queries plus native BLAS, TLAS, ray-query, RT-pipeline, shader-binding-table, ray dispatch, barrier, and timestamp primitives. The planned Vulkan backend should use those Godot abstractions instead of adding an NVIDIA-specific renderer API above the optional reconstruction adapter.
- Godot deliberately disables Vulkan ray tracing on macOS/iOS because the selected MoltenVK path cannot exercise the required implementation. Compiling an unavailable branch locally would not provide runtime correctness evidence.
- The earlier M0 transfer bundle contained only the schema-1 scene packet. M1 later established that this packet lacks mesh streams and therefore is not an arbitrary self-contained replay input.

Implemented preparation:

- The portable M0 generator now also emits the deterministic schema-1 self-contained capture: 1,404 bytes, one dynamic opaque triangle, three vertices, three indices, and the unchanged two-view packet.
- Bundle schema 2 includes and hashes every file: the packet, complete capture, layout SPIR-V, paired closure SPIR-V, expected invariants, companion protocol schemas, build/probe/orchestration scripts, validators, and this workflow description.
- The Windows orchestrator requires a clean checkout at the exact bundled revision, records the machine/toolchain, builds tests, runs focused tests, and stops explicitly if the Vulkan replay runner is absent.
- The renderer-report validator requires both ray-query and RT-pipeline executions and separate finite timings for every required GPU stage before accepting a Windows renderer component result.

Unresolved risks:

- No Vulkan path-tracing backend or replay runner has executed on Windows.
- No query-versus-pipeline measurement, Metal/Vulkan parity result, DLSS/fallback result, OpenXR result, CloudXR result, or physical Vision Pro result exists.
- The portable report schema may need additive diagnostic fields after the first Windows implementation, but required semantics and the frozen capture must not be weakened.

Next executable step: implement the Vulkan backend and replay runner against Godot's existing RenderingDevice ray-tracing APIs on the recorded Windows RTX 5080 host, then run this harness before choosing the execution path.
