# VG8 implementation journal

## Scope

VG8 currently certifies the portions of dynamic geometry that have a real
engine contract: rigid instances keep immutable package/cluster/ray-group
identity while their current and previous transforms move. The test also
exercises the compiler's explicit conventional-path rejection for fixed-topology
deformation and topology-changing mutation.

## Local evidence

- `tests/servers/rendering/test_virtual_geometry_dynamic_certification.cpp`
  compiles a real VG1 package, owns it through `VirtualGeometry`, publishes a
  real `VirtualGeometryInstance3D` resource boundary, and checks the existing
  `RendererSceneRender::VirtualGeometryInstance` transport record over 48
  moving frames.
- Fixed-topology deformation is not promoted: `VirtualGeometryCompiler::Input`
  with `immutable == false` produces an explicit conventional-path diagnostic
  and no virtual pages. A topology-changing index mutation follows the same
  explicit path. There is no existing clustered-deformation eligibility API to
  certify, so the test does not invent one.
- No clustered deformation, destruction partition, TLAS update, or performance
  claim is made by this milestone.

## External blockers

This repository does not contain the representative dynamic city content,
physical XR device, Windows Vulkan/RTX runtime, or frozen 90 Hz measurement
conditions needed for production VG8 promotion. Those require a separate
content/compiler integration and hardware run. The local test is deterministic
CPU contract evidence only.

## Next executable step

Provide representative fixed-topology and topology-changing assets plus a
Windows RTX capture harness, then measure bounds, raster/ray final-deformation
agreement, TLAS/refit cost, motion vectors, stereo history, and fallback quality
before promoting either deformation class.
