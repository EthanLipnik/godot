# Windows path-tracing certification harness

This directory prepares the bounded M3 Windows/Vulkan certification pass. It is source-complete only for bundle integrity, environment capture, the baseline Godot editor build, focused portable tests, canonical self-contained capture/layout/closure replay inputs, and fail-closed result provenance. Vulkan ray-tracing execution, query-versus-pipeline comparison, DLSS, CloudXR, and parity remain unimplemented or unverified until run on the target PC.

The harness deliberately contains no proprietary SDK, binary, credential, endpoint, or machine-specific path. Run it from a clean checkout at the revision being certified.

## Prepared workflow

1. On macOS, run `../m0/prepare_windows_certification_bundle.sh` from a clean revision and transfer the resulting tarball without modifying it. The script refuses dirty worktrees by default; `PATH_TRACING_ALLOW_DIRTY_BUNDLE=1` exists only for local harness development and produces an ineligible manifest that Windows rejects.
2. On Windows, use a Visual Studio developer PowerShell and run `run_renderer_certification.ps1` with the checkout, extracted bundle, and output paths.
3. The harness validates every bundled file, requires the exact source revision and a clean checkout, records the environment, builds a test-enabled editor, and runs the focused portable tests.
4. Until a Vulkan replay runner exists, the harness then stops with `vulkan_replay_executable_missing`; this is deliberate and cannot be interpreted as a partial pass.
5. The future replay runner must consume `reference.scene_capture.ptc` unchanged and output both ray-query and RT-pipeline stage measurements in the format enforced by `validate_renderer_report.py`.
6. A passing renderer-component report still does not pass M3. Metal/Vulkan parity, OpenXR, physical Vision Pro streaming, tracking, reconnect, and telemetry evidence remain separate required gates.

The current build recipe targets the upstream-conventional Vulkan-enabled Windows editor. It does not enable DLSS, CloudXR, or another restricted dependency.

## Report contract

The renderer replay report is schema 1 JSON with `passed`, `selected_path`, and an `executions` object containing exactly `ray_query` and `rt_pipeline`. Each execution must report two distinct views, correct dynamic refit, valid guides, and finite non-negative GPU milliseconds for deformation, BLAS build/refit, TLAS update, trace, guide generation, reconstruction, and composition. This proves that both paths ran; it does not prescribe which path wins.

Optional proprietary adapters must be installed outside the checkout and recorded in the environment/output report. Never add their SDKs, binaries, licenses, credentials, or generated redistributables to this bundle.
