# VG3 implementation journal

## 2026-08-24 — compute/indirect reference contract

Delivered the renderer-owned, product-neutral reference selection contract in
`servers/rendering/virtual_geometry/virtual_geometry_raster.*` and the paired
`virtual_geometry_select.glsl` compute source. It consumes VG1 hierarchy
descriptors and VG2 active-page state, selects an atomic coarse/fine cut by
projected error, keeps a conservative stereo refinement union, preserves
per-eye visibility masks, and emits fixed-capacity 20-byte indexed-indirect
commands. Unused command records are zero-count no-ops.

Occlusion is strictly raster-only and fails open for camera cuts, near-plane
uncertainty, invalid history, and stereo-boundary uncertainty. VG1 has no
serialized conservative normal cone, so cone rejection is deliberately
disabled rather than inferred from non-conservative vertex data. Diagnostics
report selected/fallback/culled/overflow counts, eye disagreement, the actual
`compute_indirect` path, and `native_mesh_shader=false`.

CPU/source tests cover bounded commands, atomic fallback, per-eye occlusion,
and cut fail-open behavior. No GPU fixture or performance result is recorded.

## Native mesh/object path status

Not implemented. Current public RenderingDevice exposes indexed indirect draws
but no public mesh/object shader pipeline/dispatch API or validated Metal mesh
limits. The native capability is explicitly false; the compute/indirect
reference path remains the only declared VG3 raster execution path.

## Flux primary-surface submission gate

VG2 now publishes real position/index/attribute/descriptor storage-buffer RIDs
and an active descriptor generation. That removes the former upload blocker,
but not the draw contract:

- `RenderingDevice::draw_list_bind_vertex_buffers_format()` accepts only RIDs
  owned by `vertex_buffer_owner`; VG2 publishes `storage_buffer_owner` RIDs.
- `draw_list_bind_index_array()` accepts only an `IndexArray`; no public draw
  API binds an index-buffer-compatible shared heap at an arbitrary byte offset.
- `VirtualGeometryGPUClusterDescriptor` provides position/index/attribute
  offsets and material slots, but not the complete per-stream vertex layout
  required by Flux's existing material shader variants. The attribute heap is
  packed by decoded source schema, so treating it as a conventional vertex
  layout would misread normal/tangent/UV data.
- Authored bindings currently live on `VirtualGeometry` resources. Flux has no
  virtual-geometry instance submission in `RenderDataRD`, so no material record
  can be selected per cluster without adding that scene-to-renderer boundary.

`VirtualGeometryInstance3D` currently derives from `Node3D`, not a rendering
instance class, and creates no render-server RID. Consequently it cannot
contribute its transform, visibility layer, resource revision, or authored
material binding RIDs to a Flux frame. This is the current blocking boundary;
it is independent of the now-resolved heap buffer capability.

The narrow next capability is a product-neutral RenderingDevice draw binding
for shared buffers plus a descriptor-driven vertex-fetch shader variant that
declares its complete schema. It must then be joined to the existing Flux
material-record lookup through an explicit renderer instance submission record.
Until that exists, issuing an indirect draw would either fail RD ownership
validation or bypass Flux materials/PrimarySurfaceV1, so it would be synthetic
success rather than a valid VG3 submission.

## 2026-08-24 — Flux compute/indirect material submission

VG3 now consumes `RenderDataRD::virtual_geometry_instances` in Flux's opaque
primary passes. Each record requires an active VG2 descriptor generation,
whole-heap vertex/index/attribute/descriptor RIDs, an indexed whole-heap view,
and an authored material RID for every selected material slot. Selection uses
the bounded CPU reference cut as the deterministic candidate producer, then a
Flux-owned compute pipeline validates the published descriptor generation and
writes ordinary indexed-indirect commands. A compute-to-raster barrier precedes
each draw.

Commands are partitioned by authored material slot and use that material's
existing `SceneShaderFlux` shader, material uniform set, and matching opaque or
PrimarySurfaceV1 pipeline. The same per-instance transform, previous transform,
visibility layer, and stable semantic identity record is appended to Flux's
opaque instance buffer; raster selection remains per eye while residency is
shared. Pages referenced by a submitted batch receive the pending submission
serial. Alpha, vertex-deforming, position-writing, point-size, missing, stale,
or invalid material bindings are counted and rejected; VG3 never substitutes a
default material.

The path binds the entire canonical VG2 position/index/attribute heaps plus the
descriptor table to the selector and binds the position/attribute vertex buffers
and indexed whole-heap view to the Flux draw. Native mesh shaders remain false.
This is still a reference submission path: a rendered Metal fixture and quality
measurement are not yet evidence of backend completion.
