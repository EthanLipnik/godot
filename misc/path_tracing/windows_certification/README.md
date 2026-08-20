# Windows path-tracing certification harness

This directory prepares the bounded M3 Windows/Vulkan certification pass. It is source-complete only for environment capture, the baseline Godot editor build, canonical packet/layout replay inputs, and result provenance. Vulkan ray-tracing execution, query-versus-pipeline comparison, DLSS, CloudXR, and parity remain unverified until implemented and run on the target PC.

The harness deliberately contains no proprietary SDK, binary, credential, endpoint, or machine-specific path. Run it from a clean checkout at the revision being certified.

## Prepared workflow

1. Run `probe_environment.ps1 -OutputPath <path>` from a Visual Studio developer PowerShell.
2. Review the exact OS, Visual Studio/MSVC, Windows SDK, Python, SCons, Vulkan, NVIDIA driver, and GPU report.
3. Run `build_editor.ps1 -GodotRoot <checkout>`.
4. Transfer the generated canonical packet and SPIR-V layout fixture produced by `../m0/prepare_windows_certification_bundle.sh`.
5. Add M3 Vulkan replay executables without changing the packet, expected invariants, or result schema.
6. Record each GPU stage separately and compare against the Metal captures from the same source revision.

The current build recipe targets the upstream-conventional Vulkan-enabled Windows editor. It does not enable DLSS, CloudXR, or another restricted dependency.
