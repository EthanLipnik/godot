# VG2 implementation journal

## 2026-08-24 — renderer-owned residency/upload baseline

- Added the immutable `VirtualGeometry` resource and single-node `VirtualGeometryInstance3D` boundary. No virtual page or cluster is a scene child.
- Added `VirtualGeometryStorage`: portable worker decode/validation completions are generation checked, renderer-side allocation is non-relocating and aligned across separate position/index/attribute heaps, activation waits for a monotonic GPU completion serial, and retirement waits for upload, raster, ray, and dependency use serials.
- Corrected the initial CPU-only placeholder: the storage now owns persistent `RenderingDevice` position, index, attribute, and cluster-descriptor heaps; each worker-decoded `VGC1` payload is parsed for exact stream/index ranges before its render-thread upload plan is enqueued. Descriptors become active only after the owning real submission serial completes. The no-device mock preserves the same parsed plans for deterministic tests.
- Added explicit authored material bindings on `VirtualGeometry`; missing slots are observable as incomplete bindings rather than a default-material substitution.
- Added monotonic `RenderingDevice` submission serial queries. A serial is assigned as the internal frame is submitted and advances to complete only when that frame's existing fence retires. It exposes no backend fence object.
- VG3 selection remains separate. VG2 only reports raster integration ready after all four RD heaps exist and descriptor publication has completed.

## Known boundary

- VG3 still needs to bind the exposed heap RIDs and descriptor generation to its Flux draw records; VG2 deliberately does not submit draws or implement selection shaders.

## 2026-08-24 — bindable heap contract correction

- Position and canonical-attribute heaps are now `RenderingDevice` vertex buffers with `BUFFER_CREATION_AS_STORAGE_BIT`; the UINT32 index heap is an index buffer with the same storage-readable bit. The descriptor heap remains a storage buffer.
- Worker decode repacks every source schema into one fixed product-neutral attribute record. Missing normal, tangent, UV, color, joint, and weight streams receive named defaults while the original stream flags remain in the descriptor.
- Descriptor records expose byte offsets plus `base_vertex` and `first_index` derived from the whole heaps, matching indirect indexed command addressing.

## 2026-08-24 — scene-node boundary

- `VirtualGeometryInstance3D` now derives from `VisualInstance3D`, so it owns one normal RenderingServer instance RID, propagates layer changes through the visual-instance API, and returns the immutable virtual resource bounds without materializing page children.

## 2026-08-24 — renderer scene contract completion

- `VirtualGeometry` now owns one dedicated RenderingServer resource RID, uploads validated immutable packages and authored material RIDs with monotonic revisions, and frees that RID on resource teardown. `VirtualGeometryInstance3D` assigns that dedicated RID as its sole visual-instance base; no page or cluster becomes a node.
- Scene culling preserves virtual instances as immutable frame records containing resource ID/revision, instance revision, transforms, bounds, layers, and material bindings. Dependency refresh routes back to the virtual resource dependency instead of generic resource storage, so package and material changes invalidate bounds/culling safely.
- The RD backend and dummy backend both own, query, update, and free virtual-geometry resources. RD teardown notifies dependency consumers before destroying residency storage; frame record lifetime ends only after synchronous `render_scene` consumption. Draw submission remains intentionally deferred to VG3.
