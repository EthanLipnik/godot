# VG9 implementation journal

## Scope

VG9's locally decidable certification is implemented in
`tests/servers/rendering/test_virtual_geometry_stereo_long_run.cpp`. It uses
the real compiler package, `VirtualGeometryStorage`, `VirtualGeometryRayHierarchy`,
and `VirtualGeometryRasterSelector` APIs in a deterministic 240-frame traversal
and cache-pressure loop.

## Local evidence

- Stereo views have independent history state, rapid camera/foveation-threshold
  transitions, eye-separated occlusion, and fail-open camera-cut/boundary cases.
- The loop asserts fixed-capacity indirect command output, persistent coarse
  page coverage, bounded active-page count, generation increments after page
  retirement, and retention of an active ray generation after a failed
  replacement.
- Eye visibility is derived from two view masks; no output is copied from one
  eye to the other. The test is a CPU/portable contract test and does not claim
  a physical XR display result.

## External blockers

Physical XR stereo execution, foveation hardware integration, representative
city traversal/content, RTX 5080/Vulkan execution, and frozen-condition
full-frame 90 Hz/p99 runs are unavailable in this macOS development workspace.
They are required before VG9 can be called production-certified. No mock
physical certification or performance claim is recorded here.

## Next executable step

Run the same fixture with a physical XR device and representative city package
on the Windows RTX target under frozen resolution, quality, driver, scene,
warmup, duration, and p99 conditions. Capture raster, AS, tracing,
reconstruction, composition, residency, history, and stereo diagnostics.
