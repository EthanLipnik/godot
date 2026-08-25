# Flux virtual geometry implementation architecture

## 1. Status and authority

This document is the implementation architecture hypothesis for product-neutral virtual geometry in Godot's Flux renderer. It expands the streamed static-cluster boundary in `ADVANCED_RENDERING_ARCHITECTURE_HYPOTHESIS.md` into an executable compiler, package, runtime, raster, ray-tracing, stereo, editor, diagnostic, and validation program.

The architecture is intentionally falsifiable. Named algorithms, cluster sizes, page sizes, API extensions, file locations, and milestone ordering are provisional until representative scenes demonstrate better image quality, performance, memory use, streaming behavior, and backend parity than the alternatives. The invariants and acceptance outcomes are authoritative; a mechanism may be replaced when measured evidence preserves them more effectively.

The initial production targets are:

- Native macOS Metal on modern Apple silicon.
- Windows Vulkan with ordinary cross-vendor ray tracing.
- An optional NVIDIA RTX fast path on supported hardware.
- True stereo rendering with a shared view-independent scene and independent per-eye visibility and history.

This is an unshipped engine subsystem. Its schemas and APIs may be hard-cut while the implementation remains experimental. Format versions, compiler generations, renderer feature levels, package revisions, product versions, and toolchain versions remain independent contracts.

## 2. Executive decision

Flux will implement a renderer-owned virtual geometry system based on an offline-generated hierarchy of small triangle clusters. The hierarchy becomes the normal raster representation for supported static opaque geometry and removes the routine need for manually authored `LOD0`, `LOD1`, `LOD2`, and similar raster mesh chains.

It does not remove level of detail as a renderer concept. It makes raster LOD automatic, continuous, GPU-selected, streamable, and driven by projected error. Separate representations remain mandatory where their semantics differ:

- Ray tracing uses transport-aware geometry selection and backend-specific acceleration structures.
- Collision uses stable simulation-appropriate shapes.
- Gameplay and navigation use stable semantic representations.
- Block, neighborhood, skyline, impostor, voxel, and radiance HLODs remain available for city-scale aggregation and extreme distance.
- Textures, materials, emission, animation, and simulation retain independent detail policies.

The shared contract is a deterministic graph of ordinary triangle clusters, hierarchy metadata, material semantics, stable identities, dependency hashes, and page residency. It never exposes Metal objects, Vulkan acceleration structures, NVIDIA CLAS objects, mesh-shader payloads, runtime RIDs, or machine-specific binary acceleration structures in portable content.

The implementation has three principal consumers:

1. A GPU-driven raster path selects and renders the visible cluster cut.
2. A ray-world builder derives stable transport cuts and acceleration structures from the same authored source and hierarchy.
3. Offline and runtime tools consume stable cluster identities for visibility, diagnostics, incremental compilation, and validation.

## 3. Required outcomes

The system is successful only if it provides all of the following:

- Higher supported geometric fidelity without proportional growth in scene nodes, draw calls, CPU submission, resident memory, or rasterized triangles.
- No routine artist-authored raster mesh LOD chains for supported static geometry.
- Stable image quality under continuous camera motion, rapid rotation, altitude change, and stereo head motion.
- Crack-free and hole-free replacement between coarse and fine cluster cuts.
- Bounded disk, CPU, GPU, transfer, acceleration-structure, and descriptor working sets independent of total source-scene size.
- A persistent coarse raster and ray world during streaming delays or failures.
- Native Metal performance rather than a Vulkan-through-Metal compatibility layer.
- Equivalent named behavior on Metal and Vulkan/RTX even where implementation techniques differ.
- Independent, correct per-eye visibility with shared static geometry and acceleration structures wherever valid.
- Ray-traced shadows, reflection, and indirect transport that never lose an off-screen contributor merely because raster culling rejected it.
- Deterministic compilation and stable identity suitable for incremental city packages and baked visibility.
- Complete diagnostics that distinguish source geometry, resident geometry, selected raster geometry, ray geometry, fallbacks, misses, rebuilds, and actual backend path.
- Measured improvement at equal quality, or materially higher quality at equal performance and memory, on representative production scenes.

## 4. Non-goals

The first accepted implementation does not need to:

- Reproduce Unreal Engine's Nanite file format, public API, raster pipeline, or feature matrix.
- Make a vendor extension part of Godot's shared engine contract.
- Replace collision, navigation, simulation, or semantic city records with render triangles.
- Treat photogrammetry as a sufficient semantic world representation.
- Convert transparent blended surfaces, particles, decals, volumes, or arbitrary procedural shaders automatically.
- Eliminate city-scale HLODs, virtual textures, material mipmapping, ray proxies, or world-space radiance representations.
- Make every deforming mesh virtual in the first production slice.
- Guarantee that more source triangles are always faster. Unsupported or adversarial content must fall back explicitly.
- Hide unsupported material, alpha, deformation, or topology behavior behind a default material or silent conventional-mesh path.
- Claim the RTX 5080 true-stereo 90 Hz target from a compiler test, a static screenshot, a Mac-only run, or an isolated microbenchmark.

## 5. Terminology

### 5.1 Source primitive

An indexed triangle primitive with one canonical topology and a declared vertex/material schema before virtual-geometry compilation. A source mesh may contain multiple source primitives.

### 5.2 Cluster

A small spatially coherent group of triangles with a bounded number of vertices and primitives. A cluster is the finest independently culled geometry unit, not necessarily the disk-transfer unit or ray-acceleration unit.

### 5.3 Cluster group

An atomic refinement unit. Its coarse cluster set and fine cluster set cover the same bounded source region and share a protected boundary. A group changes representation only when the complete replacement set is active.

### 5.4 Hierarchy cut

A non-overlapping set of active clusters that covers the supported source geometry exactly once at the selected detail. Raster views and ray roles may choose different cuts from the same hierarchy.

### 5.5 Page

An independently addressable, checksummed, compressed transfer unit containing cluster payloads and any tightly coupled tables. Pages are sized for useful storage and upload behavior; a page may contain many clusters.

### 5.6 Root fallback

A persistent coarse cut that covers the entire virtual-geometry resource or spatial partition. A missing descendant page may reduce detail but may not create a hole.

### 5.7 Raster cut

The cluster cut selected for one raster view or the conservative union of multiple compatible views.

### 5.8 Transport cut

A cluster or proxy cut selected for a ray role such as sharp reflection, shadow, diffuse indirect, or distant transport. It is not derived solely from raster visibility.

### 5.9 Ray structure group

A stable set of geometry compiled or assembled into one backend acceleration-structure ownership unit. On Metal and portable Vulkan this normally maps to an immutable compacted BLAS. On supported NVIDIA hardware it may map to a collection of CLAS objects assembled into a cluster BLAS.

### 5.10 Macro HLOD

A block, neighborhood, skyline, canopy, impostor, voxel, or radiance representation that aggregates semantics beyond ordinary cluster simplification. Macro HLODs remain distinct because geometric simplification alone cannot preserve every distant material, emission, foliage, or transport requirement efficiently.

## 6. Architectural invariants

1. Portable packages contain ordinary geometry semantics, never machine-specific acceleration structures.
2. Every active virtual resource has a persistent coarse raster fallback.
3. Every occupied transport region has a conservative coarse ray fallback.
4. A partial fine upload never replaces a complete coarse group.
5. A page becomes active only after IO, decode, validation, GPU upload, descriptor publication, and the relevant completion token have succeeded.
6. A page is freed only after the last possible GPU reference and acceleration-structure dependency have retired.
7. Raster invisibility never proves ray-transport irrelevance.
8. Static geometry, material tables, and acceleration structures may be shared across eyes; visibility tests and temporal state remain per view.
9. Stereo residency and shared cuts use the conservative union of active views.
10. Stable identity is derived from canonical source identity and compiler semantics, never array position, runtime RID, node allocation order, or load timing.
11. Material boundaries that change opacity, sidedness, ray behavior, or closure semantics cannot be erased by cluster simplification.
12. Unsupported geometry fails visibly or uses a declared conventional path; it never silently loses material or transport behavior.
13. Baked primary visibility may reduce raster candidates only. Baked transport visibility remains a separate conservative closure.
14. Total source-scene size does not determine runtime cost; explicit working-set budgets do.
15. Quality changes caused by thresholds, proxy selection, quantization, or omitted features are named, diagnosable, and compared with a reference.
16. Metal and Vulkan/RTX implement equivalent feature names and scene semantics, not necessarily identical GPU algorithms.
17. The renderer never creates a scene node per cluster or per page.
18. A virtual-geometry feature is not complete until editor, runtime, streaming, stereo, raster, ray transport, diagnostics, and fallback behavior agree.

## 7. Existing implementation baseline

The current repository contains a bounded proof rather than a production virtual-geometry renderer:

- `StreamedClusterMesh` builds a deterministic two-level hierarchy using meshoptimizer meshlets, cluster partitioning, simplification, protected borders, stable IDs, serialized bounds, and persistent coarse pages.
- `StreamedClusterResidencyManager` implements explicit `unloaded -> requested -> loading -> resident -> active -> retiring` states, byte/task budgets, atomic group activation, stereo-union input regions, and explicit completion tokens.
- Large external `EXT_meshopt_compression` buffers can be range-read without materializing their complete logical fallback.
- `StreamedClusterMeshInstance3D` synchronously reads and decodes pages into `ArrayMesh` objects, creates a `MeshInstance3D` for every active page, and uses synthetic completion tokens.
- Active streamed surfaces can be appended to the native Metal Flux frame request as ordinary triangle surfaces.
- RenderingDevice supports indirect indexed draws, but its public raster pipeline does not yet implement mesh/object shader stages.
- The generic Vulkan driver exposes ordinary BLAS/TLAS and ray-tracing primitives.
- The generic Metal RenderingDevice driver still reports ray tracing unsupported; the current Flux prototype owns a separate native Metal ray-tracing path.

The production architecture must preserve the validated identity, hierarchy, bounds, lifecycle, and fallback concepts while replacing the synchronous node-based consumer, two-level limit, position-only payload, and synthetic GPU lifetime.

## 8. System overview

```text
High-quality source meshes / procedural city geometry / terrain / authored overrides
                                      |
                                      v
                         Canonical geometry normalization
                                      |
                   +------------------+------------------+
                   |                  |                  |
                   v                  v                  v
            Material partitions   Instance families   Semantic sidecars
                   |                  |                  |
                   +------------------+------------------+
                                      |
                                      v
                         Virtual-geometry compiler
                 leaf clusters -> groups -> deep hierarchy
                                      |
                         page packing and compression
                                      |
                                      v
                   versioned manifest + independently loadable pages
                                      |
                        runtime request/residency scheduler
                                      |
                 +--------------------+--------------------+
                 |                    |                    |
                 v                    v                    v
        GPU raster selection   ray-structure builder   diagnostic/visibility IDs
                 |                    |                    |
       Metal mesh shaders       Metal compact BLAS         editor overlays
       Vulkan mesh/indirect     Vulkan KHR BLAS            baked visibility
                                RTX CLAS fast path          capture/replay
                 \                    |                    /
                  +-------------------+-------------------+
                                      |
                                      v
                   Flux visibility, lighting, guides, reconstruction
```

## 9. Ownership boundaries

### 9.1 Importer and compiler ownership

The importer owns source decoding, canonical streams, provenance, material classification, instance discovery, and optional authored fallback metadata. The virtual-geometry compiler owns cluster formation, hierarchy construction, simplification, error bounds, page layout, compression, and deterministic IDs.

The compiler does not own runtime budgets, camera prediction, current-frame occlusion, ray contribution, temporal history, or device-specific acceleration structures.

### 9.2 Renderer ownership

The renderer owns:

- Per-view projected-error evaluation.
- Frustum, cone, backface-cone, and current-frame occlusion tests.
- Stereo cut union and per-eye visibility.
- Page requests and priorities derived from actual views and transport roles.
- GPU buffer allocation and upload.
- Descriptor and material-table publication.
- Raster draw/mesh dispatch generation.
- Ray-structure construction, compaction, reuse, and retirement.
- History invalidation when geometry or material identity changes visibly.
- Diagnostics and frame-stage timings.

### 9.3 World streamer ownership

A city or large-world streamer chooses which spatial packages are eligible and predicted. It does not select individual visible clusters. It supplies coarse package candidates, traversal prediction, altitude context, and separate raster, transport, collision, and simulation budgets.

### 9.4 Scene ownership

One scene instance references one virtual-geometry resource plus transform, visibility layers, material overrides allowed by the format, and stable semantic identity. Clusters and pages never become user-visible child nodes. Editor selection resolves a cluster hit back to the source instance and feature identity.

## 10. Supported content classes

| Content | Initial virtual raster path | Initial virtual ray path | Required fallback or special handling |
|---|---:|---:|---|
| Static opaque building shells | Required | Required | None beyond persistent coarse cuts |
| Static opaque landmarks and props | Required | Required | Conventional mesh only for unsupported attributes |
| Roads, curbs, sidewalks, and terrain | Required after seam validation | Required through stable spatial ray groups | Collision remains independent |
| Repeated rigid modules | Required with instancing | Shared BLAS or CLAS templates | Preserve per-instance semantic identity |
| Opaque geometric foliage | Experimental, then required if it wins | Bounded triangle/cluster ray form | Canopy HLOD remains required |
| Masked foliage and fences | Experimental | Any-hit or opacity-micromap path | Explicit alpha candidate limits and opaque coarse fallback |
| Transparent blended surfaces | Not in initial path | Separate transparent/ray closure path | Conventional forward/overlay representation |
| Decals | Receiver/material indirection only | Explicitly supported ray material policy | Mesh decals remain conventional initially |
| Static displacement baked to triangles | Required | Required | Source/compiler quality setting |
| Runtime displacement | Experimental | Capability-gated | Conventional tessellation or bounded proxy |
| Rigid moving instances | Required after static path | Shared static geometry with dynamic transforms | TLAS update only |
| Skinned or morphing meshes | Later milestone | Updateable conventional or clustered path after proof | Conventional deformation path remains authoritative initially |
| Destructible geometry | Later milestone | Partitioned immutable pieces or rebuildable groups | Conventional geometry until topology lifecycle is certified |
| Particles, volumes, hair, curves | No | Separate renderer-specific representations | Never coerced to triangle clusters without a measured reason |

## 11. Canonical input contract

Every source primitive presented to the compiler declares:

- Stable source asset identity and primitive identity.
- Source content digest.
- Indexed triangle topology.
- Position stream and coordinate domain.
- Normal stream or permission to generate it.
- Tangent frame, tangent-generation rule, and handedness where required.
- UV sets and wrap/precision requirements.
- Vertex color and custom supported attributes.
- Skin and morph streams if the selected compiler profile permits them.
- Per-triangle or per-range material identity.
- Opacity class: opaque, masked, or blended.
- Sidedness and winding.
- Deformation/topology class: immutable, rigid-instance, deforming-fixed-topology, or changing-topology.
- Lightmap or other editor-only channels, if retained.
- Semantic feature identity and provenance mapping where available.
- Collision, navigation, ray, emission, and interaction sidecar references.

The compiler rejects non-finite positions, invalid indices, degenerate required bounds, unsupported mixed topology, unbounded attribute ranges, and material declarations that cannot be represented truthfully. Repair policies such as welding, degenerate removal, normal regeneration, or winding correction are explicit import steps whose results affect the source digest.

## 12. Offline compiler pipeline

### 12.1 Normalization

1. Decode only required source ranges.
2. Convert positions into a stable primitive-local coordinate system.
3. Normalize winding and declared sidedness.
4. Validate finite bounds and index ranges.
5. Canonicalize vertex streams without erasing intentional seams.
6. Partition topology by incompatible material and renderer semantics.
7. Record the reversible mapping from output triangles to source feature/material identities.

Vertex welding may occur only across attributes and semantics that are identical under the declared tolerances. UV seams, hard normals, material boundaries, emission identity, alpha class, ray flags, and authored feature boundaries are not welded accidentally.

### 12.2 Semantic partitioning

Before cluster formation, triangles are partitioned when crossing a boundary would break any of:

- Opaque versus masked versus blended behavior.
- Single- versus double-sided behavior.
- Ray visibility or instance mask.
- Material closure family or hit-group requirement.
- Emissive sampling identity.
- Deformation ownership.
- Persistent semantic selection or authored override.
- Required tangent/UV schema.
- Collision or interaction sidecar mapping when triangle correspondence is required.

Clusters should normally use one material slot for the fastest raster and ray path. A bounded local material palette is permitted only after Metal, portable Vulkan, and RTX measurements show that it reduces duplication without harming shading or acceleration-structure performance.

### 12.3 Leaf clustering

The baseline portable leaf profile targets at most 64 unique vertices and 124 triangles because it fits the existing meshoptimizer proof and common meshlet workflows. These numbers are compiler-profile defaults, not public engine promises.

Leaf formation optimizes a weighted objective:

- Spatial locality.
- Vertex reuse.
- Compact bounds.
- Compact normal cone.
- Material coherence.
- Protected boundary size.
- Page locality.
- Ray-structure locality.

Device adapters query their limits. If a backend benefits from another cluster size, it may repack clusters in a derived device cache, but portable identities and source coverage remain based on the canonical clusters.

### 12.4 Group construction

Spatially adjacent clusters are partitioned into refinement groups. A group records:

- The fine child cluster set.
- The coarse replacement cluster set.
- Conservative group bounds.
- Protected external boundary edges.
- Maximum geometric deviation.
- Normal and silhouette deviation bounds where available.
- Material and semantic dependencies.
- Parent and child group relationships.

The fine and coarse sets must cover the same source region. Group replacement is atomic. Internal boundaries may simplify; the external boundary is locked or otherwise constructed to remain compatible with neighboring groups at every allowed cut.

### 12.5 Hierarchy generation

The current two-level proof becomes a deep hierarchy:

1. Begin with leaf clusters.
2. Partition adjacent clusters into bounded groups.
3. Simplify each group while protecting its external boundary and semantic constraints.
4. Recluster the simplified result.
5. Record conservative error and coverage mappings.
6. Repeat until reaching one or more persistent root groups of acceptable size.

The representation is a group DAG when sharing or many-to-many replacements improve packing, but runtime cuts must remain easy to validate. A tree is preferred unless a DAG provides a measured material storage, crack prevention, or streaming advantage. Cycles, unreachable nodes, duplicate stable identities, and non-conservative parent bounds are invalid.

### 12.6 Simplification constraints

Simplification must preserve, within the selected error policy:

- External boundaries shared with independently selected groups.
- Material/opacity/sidedness partitions.
- Building and terrain silhouette.
- Major façade recesses, ledges, entrances, window planes, and rooftop forms when their removal exceeds projected error.
- Road/curb/sidewalk ownership boundaries.
- Terrain seams and water boundaries.
- Emissive region identity needed for distant aggregation.
- Authored landmark feature locks.
- Valid UV parameterization and tangent behavior.
- Conservative bounds.

The compiler may use attribute-weighted geometric error, but it cannot hide an invalid material or emission transition inside a scalar position error.

### 12.7 Error metric

Every group stores a conservative world-space error `E_world`. For each active view, runtime estimates screen error using a conservative near depth to the group's bounds:

```text
projection_scale = 0.5 * viewport_height / tan(vertical_fov / 2)
near_depth       = max(distance_to_center - bounds_radius, camera_near)
E_pixels         = E_world * projection_scale / near_depth
```

Orthographic, asymmetric, oblique, foveated, and projection-jittered views use their exact renderer projection rather than assuming the perspective expression above. Selection uses the maximum error over all eyes and required shadow/auxiliary views sharing the cut.

The threshold is expressed in output-pixel or perceptual space. Dynamic resolution and foveation must not silently reduce geometry quality: the selected policy declares whether it follows internal resolution, submitted resolution, or a foveated regional quality target.

Hysteresis uses separate refine and coarsen thresholds. The system never oscillates around one threshold every frame.

### 12.8 Attribute encoding

The portable payload supports, by declared stream schema:

- Quantized cluster-local positions with conservative decode bounds.
- Octahedral or equivalently bounded normals.
- Tangent direction plus handedness where required.
- Half or quantized UVs with declared scale/bias and wrap behavior.
- Vertex colors.
- A bounded local index format.
- Optional source-triangle or feature remap.
- Optional static secondary attributes required by supported materials.

Quantization error contributes to the stored group error. The compiler validates that decoded bounds still enclose the original supported geometry. A high-precision position profile remains available for large local extents, adversarial thin geometry, and reference comparison.

Static cluster payloads do not store previous-frame positions. Rigid motion derives previous transforms per instance. A later deforming-cluster format may store current/previous output buffers owned by the deformation stage.

### 12.9 Instancing

Identical canonical geometry and material schemas compile once. Instance records carry stable identity, transform, previous transform, visibility layers, material-parameter overrides allowed by the resource, and semantic feature identity.

The compiler distinguishes:

- Exact instances sharing geometry and material tables.
- Geometry instances with bounded material parameter variation.
- Similar but nonidentical geometry that cannot safely share topology.

RTX cluster templates, ordinary shared BLAS, and Metal shared primitive acceleration structures are adapter optimizations derived from this portable identity. They do not alter package semantics.

### 12.10 Special geometry

#### Terrain and roads

Terrain, roads, curbs, and sidewalks use ownership-aware partitions and shared boundary vertices before clustering. Simplification cannot reopen cracks or allow suppressed terrain to reappear through authored urban surfaces. Macro terrain and skyline HLODs remain independently compiled.

#### Foliage

Opaque geometric foliage is preferred where measured. Aggregate foliage uses area-preserving simplification, explicit canopy HLODs, and bounded overdraw. Masked foliage records alpha coverage semantics and never becomes an unlimited any-hit workload. RTX opacity micromaps are an optional adapter. Metal retains bounded candidate traversal and coarse opaque canopy fallbacks.

#### Thin geometry

Wires, fences, rails, signs, and fire escapes require a minimum projected-width or topology-preservation policy. A simplifier may replace them with a declared impostor or aggregate representation, but may not silently erase interaction-critical or silhouette-critical features.

#### Emissive geometry

Cluster simplification preserves a mapping from visible emissive triangles to stable emitter hierarchy records. Distant group parents may carry aggregate flux, area, directionality, and occupancy rather than retaining every luminous triangle. Raster emission and direct-light selection remain energy-consistent.

#### Transparent geometry

Blended transparent geometry is excluded from the initial virtual raster path and rendered through the existing transparent overlay path. Opaque architectural frames and glass planes may be separated so frames virtualize while glass retains its supported Flux closure.

### 12.11 Page packing

Hierarchy nodes are packed into pages based on:

- Common parent/child activation.
- Spatial locality.
- Expected request correlation.
- Material and texture dependency locality.
- Ray-structure group locality.
- Transfer and decompression size targets.
- Avoidance of one small dependency forcing a very large page.

Root fallback pages are independently loadable and marked persistent. Fine pages never contain the sole copy of data required to render a coarse fallback.

Page size targets are measured per storage and platform. The manifest permits variable page sizes under explicit maximum decoded, compressed, upload, and allocation bounds. No portable blob or RenderingDevice allocation relies on an unchecked truncation to `uint32_t`.

### 12.12 Compression

The baseline uses meshoptimizer-compatible index/vertex compression where its ratified modes preserve the declared streams. Page compression is independently selectable for disk and transfer behavior. The runtime records compressed bytes read, decoded bytes produced, upload bytes, and decode time separately.

Compression dictionaries or platform caches are keyed by format version, stream schema, compiler generation, source digest, and compression generation. A dictionary mismatch rejects the page; it never decodes approximately.

### 12.13 Determinism and incrementality

Given identical canonical inputs, compiler settings, and compiler generation, output manifests, IDs, page contents, dependency tables, and hashes are byte deterministic.

Stable IDs derive from:

- Source namespace and asset identity.
- Source primitive or semantic feature identity.
- Canonical topology/content digest.
- Hierarchy coverage identity.
- Stream schema.
- Material semantic partition.
- Compiler semantic generation.

They do not derive from worker scheduling, hash-map iteration, output array position, or physical page filename.

A local material change rebuilds affected material bindings and dependent cluster pages, ray structures, visibility signatures, and emission records without rebuilding unrelated source primitives. A topology change rebuilds the affected primitive hierarchy and dependency closure. Page packing changes may alter physical pages without changing logical cluster identities when their content is unchanged.

## 13. Portable package format

### 13.1 Manifest

The versioned manifest contains:

- Format version and compiler semantic generation.
- Source asset identities and digests.
- Resource-local coordinate frame and bounds.
- Stream schemas.
- Material table and semantic closure classes.
- Instance-family declarations.
- Cluster descriptors.
- Refinement-group descriptors.
- Root fallback sets.
- Page descriptors and content hashes.
- Ray-structure group hints and proxy references.
- Texture, material, emitter, visibility, collision, and simulation dependencies.
- Feature/provenance remap tables where required.
- Declared unsupported or conventional-path content.
- Build settings relevant to output semantics.

### 13.2 Cluster descriptor

A logical cluster descriptor includes at least:

- Stable cluster ID.
- Topology revision.
- Attribute revision.
- Material semantic revision.
- Page ID and payload range.
- Vertex and triangle counts.
- Stream schema ID.
- Material slot or local palette ID.
- Conservative AABB and bounding sphere.
- Optional normal cone and backface-culling data.
- Local quantization decode parameters.
- Source/feature remap range.
- Ray visibility, opacity, sidedness, and deformation flags.

### 13.3 Refinement-group descriptor

A group descriptor contains:

- Stable group ID and revision.
- Coarse cluster set.
- Fine cluster set.
- Parent and child group links.
- Conservative bounds.
- World-space geometric and optional attribute errors.
- Atomic dependency set.
- Persistent-root flag.
- Optional shadow/ray/detail policy hints.

### 13.4 Page descriptor

A page descriptor contains:

- Stable page ID and content hash.
- Compressed and decoded byte counts.
- File or package range using checked 64-bit offsets.
- Compression scheme and generation.
- Cluster and table payload ranges.
- Required parent/root pages.
- Material and texture dependency hashes.
- Optional ray-structure dependency hints.
- Priority class and persistent flag.

### 13.5 Independent version contracts

The following are versioned independently:

- Portable manifest format.
- Page payload format.
- Compiler semantic generation.
- Compression generation.
- Material/closure schema.
- Ray-structure hint schema.
- Baked visibility format.
- City-package format.
- Backend device cache.

Changing one does not authorize changing the others merely to make numbers match.

## 14. Runtime resource model

### 14.1 Public scene resource

The provisional engine-facing objects are:

- `VirtualGeometry`: an immutable logical resource containing the manifest reference and editor metadata.
- `VirtualGeometryInstance3D`: one scene instance referencing the resource, transforms, layers, and allowed overrides.
- Renderer-internal `VirtualGeometryStorage`: parsed descriptors, page state, GPU allocations, and dependency generations.

Names remain provisional until implementation review. The production path must not expose `StreamedClusterMeshInstance3D`'s page-level child nodes.

### 14.2 RenderingServer boundary

The scene registers one geometry instance with stable resource and instance revisions. Renderer dirty propagation distinguishes:

- Transform change.
- Visibility/layer change.
- Material parameter change.
- Resource manifest change.
- Topology or stream-schema change.
- Residency-only change.
- Ray-structure generation change.

A residency-only change updates renderer-owned tables and GPU resources without reconstructing the scene node or walking the complete scene tree.

### 14.3 Page lifecycle

```text
unloaded
   -> requested
   -> io_pending
   -> decoding
   -> upload_pending
   -> resident
   -> descriptor_pending
   -> active
   -> retiring
   -> unloaded
```

Error states retain enough information for bounded retry and diagnostics. They always fall back to an already active ancestor when available.

Activation requires:

1. Manifest and dependency validation.
2. Complete IO with hash verification.
3. Successful bounded decode.
4. GPU allocation within budget.
5. Complete upload and visibility of writes.
6. Published descriptors/material references.
7. Complete atomic replacement group.
8. A real GPU completion primitive.

Retirement requires:

1. Removal from future selection.
2. An active covering ancestor or replacement.
3. Completion of every raster command referencing the page.
4. Completion or retirement of dependent BLAS/CLAS/TLAS structures.
5. Completion of any async copy or device-cache operation.
6. Descriptor invalidation after the last reader.

### 14.4 Threading

The main thread performs only bounded scene notifications, request publication, and diagnostics snapshots. IO, decompression, validation, page assembly, and acceleration-structure preparation use worker tasks or backend queues.

Worker completion never mutates active renderer tables directly. It publishes immutable completion records consumed at a frame boundary. Cancellation leaves no partially active page or partially updated group.

### 14.5 Allocation

GPU page data uses large renderer-owned heaps or suballocated buffers rather than one allocation per cluster. Allocation metadata includes alignment, residency generation, last-use token, and ray dependency count.

The allocator must handle fragmentation explicitly. Compaction or relocation is permitted only through generation-safe table updates and completed GPU copy tokens. A page address never changes while an in-flight raster command, ray structure, or shader table may reference it.

## 15. Residency and request scheduling

### 15.1 Separate budgets

At minimum, runtime tracks separate budgets for:

- Compressed CPU cache.
- Decoded CPU staging.
- GPU position/index/attribute data.
- Cluster and hierarchy tables.
- Material and texture descriptors.
- Raster working set.
- Metal primitive acceleration structures.
- Vulkan BLAS/CLAS and TLAS storage.
- Acceleration-structure scratch.
- Per-frame transfer bytes and task counts.
- Baked visibility pages.
- Persistent fallbacks.

One budget cannot silently consume another. Diagnostics expose current, peak, reserved, in-flight, persistent, evictable, and failed bytes.

### 15.2 Request sources

Requests may originate from:

- Active raster views.
- Both stereo eyes.
- Shadow views and reflection captures with declared policies.
- Predicted translation, rotation, and altitude changes.
- Baked primary visibility candidates.
- Ray transport volumes and role-specific closures.
- Editor selection or bounded pinning.
- Reference capture and deterministic validation.
- Persistent root requirements.

Every request records its source, priority, deadline, quality benefit, dependency cost, and conservative fallback.

### 15.3 Priority

Priority is based on a deterministic score containing:

- Missing coarse coverage before extra detail.
- Predicted time to visibility.
- Maximum projected error across views.
- Screen coverage.
- Semantic importance where declared.
- Shadow or transport contribution.
- Camera velocity and angular velocity.
- Altitude and traversal corridor.
- Page/dependency cost.
- Already resident sibling and parent data.
- Starvation age.

The scheduler reserves capacity for different semantic and pipeline classes so a large building page cannot indefinitely starve collision, a landmark, emission, or a small visible prop.

### 15.4 Eviction

Eviction considers:

- Complete covering fallback availability.
- Last raster, ray, shadow, and editor use.
- Predicted reuse.
- Page and dependent acceleration-structure cost.
- Persistent and pinned status.
- Group atomicity.
- In-flight GPU references.
- Material/texture dependency sharing.

Eviction never leaves a child active without its required hierarchy tables or removes a coarse ray fallback while the occupied region remains transport-relevant.

### 15.5 Prediction

Prediction uses position, velocity, acceleration, angular velocity, altitude, view direction, stereo head pose bounds, and known traversal corridors. It maintains an omnidirectional coarse reserve for rapid turns. Prediction quality is measured by late-page count, coarse-fallback exposure, wasted bytes, and request accuracy rather than only cache hit rate.

## 16. GPU hierarchy selection

### 16.1 Candidate reduction stages

Selection proceeds through explicit stages:

1. World/package streamer supplies eligible resources and spatial cells.
2. Baked primary visibility optionally reduces static raster candidates.
3. CPU or coarse GPU tests reject resources outside all active view influence regions.
4. GPU hierarchy traversal evaluates projected error and frustum bounds.
5. Per-eye current-frame occlusion rejects primary-invisible clusters for that eye.
6. Selected clusters are compacted into mesh-shader work or indirect indexed commands.
7. The renderer validates group coverage and records fallback use.

Baked visibility is a prior, not the sole authority. Missing, stale, or invalid pages fail open to spatial candidates.

### 16.2 Stereo selection

Residency and shared hierarchy refinement use the maximum detail required by either eye. Each eye still performs its own frustum and occlusion tests against its own depth hierarchy. A cluster occluded in the left eye may remain visible in the right eye.

A backend may render the conservative union in one layered/multiview dispatch if profiling proves it cheaper and per-eye output remains correct. It may not copy one eye's visibility or color into the other.

### 16.3 Occlusion

The depth hierarchy is built from appropriate current or previous depth with conservative motion and disocclusion handling. Near-plane intersections, camera cuts, rapid turns, invalid history, large dynamic occluders, and stereo boundary uncertainty fail open.

Occlusion culling affects raster primary work. It does not remove ray geometry, emitters, shadow casters, or reflection contributors unless a separate representation-specific proof permits it.

### 16.4 Backface cones

Clusters may store conservative normal cones for backface rejection. The test is disabled for double-sided, deforming, invalid, or non-conservative clusters. Negative scaling and transform handedness are handled explicitly.

### 16.5 Atomic cuts

GPU traversal selects complete coarse or fine sets per group. If any required child page or descriptor is unavailable, it emits the complete active coarse set. Debug validation checks that the selected cut has no duplicate coverage, missing coverage, or simultaneous overlapping parent and child sets beyond declared transition behavior.

### 16.6 Shadow and auxiliary views

Raster shadow maps select geometry using shadow-projected error and caster significance rather than reusing the camera cut blindly. Directional cascades or clipmaps, local lights, probes, planar reflections, and editor thumbnails have explicit quality and budget policies.

Where Flux uses ray-traced shadows, the ray transport cut owns shadow geometry instead.

## 17. Raster backend architecture

### 17.1 Shared raster contract

The shared renderer provides:

- Cluster and group descriptor buffers.
- Resident-page address tables.
- Per-instance transform and material records.
- Per-view projection and selection parameters.
- Candidate lists.
- Selected-cluster output and counters.
- Visibility/material output contract.
- Indirect or mesh-dispatch generation.

The result is ordinary primary visibility, depth, motion, material identity, normal/tangent data, and any guide data required by Flux. Virtual geometry does not create a second material model.

### 17.2 Metal native path

On devices supporting Metal object and mesh shaders:

1. An object shader traverses or consumes compacted hierarchy candidates.
2. It performs coarse frustum, error, normal-cone, and optional occlusion tests.
3. It emits mesh threadgroups for selected clusters.
4. Mesh shaders decode cluster-local streams and emit vertices/primitives.
5. Fragment shading writes the Flux primary-visibility or thin-G-buffer contract.

The adapter queries device limits for mesh output vertices/primitives, payload size, threadgroup count, and supported indirect behavior. Canonical clusters that exceed a limit are split or use the fallback path; they are not truncated.

Metal heaps, fast resource loading where supported, argument buffers/tables, binary archives, and residency APIs are evaluated as backend optimizations. Their availability cannot change portable correctness.

### 17.3 Metal fallback path

On Metal devices without the required mesh-shader feature or where mesh shading loses the A/B benchmark:

1. Compute traverses hierarchy candidates and writes compact indexed indirect commands.
2. Conventional vertex/fragment pipelines consume the same resident page buffers.
3. Draw calls are issued in bounded batches by pipeline/material class.

This path is also the bring-up and correctness reference for the mesh-shader path.

### 17.4 Vulkan path

The preferred Vulkan raster adapter uses `VK_EXT_mesh_shader` when supported and measured. A compute-cull plus indirect indexed path remains the portable fallback. Device-generated commands are optional after capability and performance validation.

The Vulkan implementation consumes the same logical clusters and selection rules as Metal. Backend repacking is allowed in a content-addressed device cache.

### 17.5 Visibility buffer versus material G-buffer

Flux should evaluate two primary paths with the same selected cluster stream:

- A visibility buffer containing instance, cluster/primitive, and barycentric identity followed by material evaluation.
- Direct thin-G-buffer emission from the mesh/fragment pipeline.

The decision is measured for material diversity, overdraw, guide generation, bandwidth, alpha behavior, stereo, and secondary-hit reuse. Virtual geometry does not prescribe one permanently.

### 17.6 Material execution

Cluster selection groups work by compatible material pipeline without duplicating visible geometry unnecessarily. Bindless or argument-table access uses stable material records. Unsupported materials are counted and rendered through a visible declared fallback.

Material simplification at coarse hierarchy levels is optional and explicit. It may replace high-frequency normals, parallax, or layered evaluation with a validated macro material, but cannot change opacity, emission energy, or supported closure semantics silently.

## 18. Ray-tracing architecture

### 18.1 Shared ray contract

The portable virtual-geometry resource supplies ordinary triangle streams, stable cluster IDs, bounds, material mappings, opacity/sidedness flags, group hierarchy, and ray-structure grouping hints.

The ray-world scheduler independently chooses geometry for:

- Camera/primary rays when enabled.
- Sharp and mirror reflection.
- Rough reflection.
- Direct-light visibility and shadows.
- Diffuse indirect and ambient/contact transport.
- Emissive visibility.
- Reference/path-tracing modes.

Selection considers ray footprint, roughness, distance, path depth, expected contribution, off-screen influence, and persistent coarse coverage. It never begins with "whatever raster drew."

### 18.2 Transport cuts

The baseline defines at least three logical transport tiers:

- **Near/detail:** shell, major recesses, glass planes, silhouette features, and detail required by sharp reflections or close shadows.
- **Middle:** simplified shells, façade planes, roofs, and dominant opacity/material regions.
- **Far/aggregate:** conservative block geometry, boxes, skyline forms, or macro occluders.

A fourth voxel/radiance representation may serve very distant diffuse and visibility work.

These are compiler-derived cuts from the same source and stable feature identities. Authored ray proxies remain allowed where automatic simplification cannot preserve desired semantics efficiently.

### 18.3 Ray-structure groups

Fine raster clusters are aggregated into stable ray-structure groups. The grouping balances:

- Acceleration-structure build and compaction cost.
- Trace quality.
- Spatial locality.
- Material/opacity classes.
- Streaming activation granularity.
- TLAS instance count.
- Reuse across rigid instances.
- Backend limits.

Ray groups change less frequently than the raster cluster cut. A camera moving a few centimeters may refine raster clusters without rebuilding any ray structure.

### 18.4 Metal acceleration structures

Metal has no portable Flux requirement for an NVIDIA-style cluster acceleration structure. The native Metal adapter therefore uses conventional Metal primitive and instance acceleration structures:

- Static ray groups build immutable primitive acceleration structures.
- Builds prefer intersection performance unless measured streaming latency requires a different declared mode.
- Completed structures are compacted when that reduces total cost.
- A persistent coarse structure is active before detailed structures.
- Detailed structures are built asynchronously when their geometry pages become resident.
- A complete structure replaces another at a frame boundary; partial BLAS contents never become visible.
- TLAS updates change instance transforms, masks, and complete structure references.
- Static topology LOD changes swap complete structures rather than attempting an invalid refit that adds or removes geometry.
- Refit is reserved for eligible fixed-topology deformation or instance structures and is rebuilt when quality degradation or change magnitude crosses measured limits.

The baseline avoids rebuilding Metal BLAS from every frame's raster-visible cluster list. That would convert fine raster culling into acceleration-structure churn and can be slower than the conventional renderer.

Role-specific ray geometry may use:

- One shared conservative TLAS containing the strictest active ray cut.
- One TLAS with mask-separated role instances.
- Separate small TLAS objects for different ray roles.

The choice is made by trace/build/memory measurement. The portable contract supports all three without duplicating source identity.

### 18.5 Portable Vulkan acceleration structures

The baseline Vulkan adapter uses `VK_KHR_acceleration_structure` with ordinary compact static BLAS and a TLAS. It follows the same ray-structure group lifecycle as Metal:

- Prefer-fast-trace compact static groups.
- Explicit build scratch budgets.
- Async build and completion tracking.
- Shared BLAS for exact rigid instances.
- Complete LOD group substitution.
- Bounded TLAS instance count.
- Deferred destruction after the last trace.

This path is the correctness and cross-vendor baseline even on NVIDIA hardware.

### 18.6 NVIDIA RTX cluster fast path

On supported hardware and drivers, the Vulkan adapter may use `VK_NV_cluster_acceleration_structure`:

- Canonical or backend-repacked clusters create CLAS objects.
- Cluster templates may accelerate repeated topology.
- Resident CLAS objects assemble into cluster BLAS.
- Device-driven multi-indirect construction reduces host work.
- Stable portable cluster identity maps to the adapter's 32-bit cluster ID table.
- Position quantization is permitted only within the portable error and bounds contract.
- Opacity micromaps may attach to supported masked clusters.

CLAS is an adapter fast path, never the only valid representation. The renderer can switch to ordinary Vulkan BLAS without changing scene content or feature names. The implementation records which path actually executed.

### 18.7 Material and hit identity

Every ray hit resolves:

- Stable instance identity.
- Stable geometry/cluster or proxy identity.
- Primitive identity within that geometry.
- Material closure record.
- Interpolated supported attributes.
- Current topology/material generation.

Raster and ray representations may have different topology, so screen-space identity is not assumed to index a ray triangle directly. Mapping tables preserve feature/material agreement and diagnostics.

### 18.8 Raster/ray mismatch policy

Some mismatch is intentional because raster and transport use different detail. Acceptance requires:

- No silhouette disagreement above the declared projected or ray-footprint threshold.
- No missing glass plane, emitter, shadow blocker, or reflection feature required by the selected ray tier.
- No light leaks caused by an under-conservative far proxy.
- No self-intersection or detached reflection caused by incompatible surfaces.
- No screen-trace patch used to hide an undocumented ray-geometry error.

Sharp nearby reflections request a stricter ray cut. Rough/diffuse rays may use coarser geometry because their footprint and reconstruction cannot resolve the omitted frequency. The active tier is visible in diagnostics.

### 18.9 Dynamic geometry

Rigid moving instances share immutable geometry acceleration structures and update transforms in the TLAS.

Fixed-topology deformation remains on the existing deformation -> current/previous vertex buffers -> BLAS refit/rebuild path until a clustered-deformation milestone proves:

- Bounds remain conservative.
- Cluster topology and material identity remain stable.
- Raster and ray use the same final deformation.
- Build/refit cost improves.
- Motion vectors and per-eye histories remain correct.

Topology-changing destruction activates bounded immutable pieces or rebuildable partitions. It never mutates a shared static cluster resource in place.

## 19. Baked visibility and transport integration

Baked visibility records stable resource, instance, group, and optional cluster identities plus dependency hashes. It has two distinct products:

- Primary-visible sets reduce raster candidates.
- Hybrid-transport sets retain conservative off-screen ray geometry and emitters.

A primary set may point to a coarse group and allow runtime refinement. It need not list every possible leaf cluster. A transport set points to ray-structure groups or stable semantic features rather than the current raster cut.

Compiler or package changes invalidate only affected pages and dependency closure. A missing identity, stale digest, invalid blocker, changed opacity, changed transform, or unavailable page fails open.

Current-frame GPU occlusion remains authoritative for fine raster work. Baked visibility never certifies dynamic or transparent occlusion without a separate proven contract.

## 20. City and large-world integration

### 20.1 Spatial hierarchy

The city retains detail, neighborhood, and skyline packages. Virtual geometry operates inside and across their raster resources while preserving deterministic boundary ownership.

```text
World streamer
  -> skyline package and persistent skyline fallback
  -> neighborhood package and block HLODs
  -> detail package with virtual cluster hierarchies
  -> near hero/landmark detail pages
```

Macro HLOD and virtual cluster simplification overlap in distance but solve different problems. The compiler chooses the cheaper representation based on silhouette, material, emission, instance count, page cost, and transport needs.

### 20.2 Building generation

Procedural building generation emits high-quality canonical geometry without a manually maintained raster LOD chain. It labels semantic partitions for:

- Shells and roof forms.
- Glass planes and frames.
- Entrances and storefronts.
- Fire escapes, ledges, cornices, and balconies.
- Rooftop equipment.
- Window emission regions.
- Authored landmark locks.

The virtual-geometry compiler produces the raster hierarchy. The city compiler separately produces ray tiers, collision, emitter hierarchy, traffic/simulation records, and macro HLODs.

### 20.3 Repeated modules

Windows, frames, lamps, signals, furniture, vehicles, vegetation archetypes, and rooftop modules use shared virtual geometry where appropriate. Instance buffers remain bounded and renderer-owned. Semantic prioritization reserves capacity so one repeated class cannot starve all others.

### 20.4 Fast traversal

The request scheduler consumes city velocity, acceleration, rotation, altitude, and traversal corridors. Vertical movement widens coarse coverage and requests skyline/neighborhood roots before façade detail. Street traversal requests forward detail while retaining an omnidirectional coarse reserve.

Acceptance includes rapid 180-degree turns, vertical launches, diagonal cell crossings, rooftop-to-street transitions, and stereo head motion without holes, stale ray geometry, or main-thread upload hitches.

### 20.5 Floating origin

Packages use cell-local coordinates. Instance transforms apply the current floating-origin offset. Stable IDs, page contents, local quantization, and acceleration-structure topology do not change merely because the origin rebases. TLAS transforms and per-view camera-relative data update coherently.

## 21. Texture, material, and emission residency

Geometry refinement requests its required material and texture dependencies with deadlines. A fine geometry page cannot activate if doing so would reference an invalid descriptor or unavailable required material page.

Fallback rules are explicit:

- Coarse geometry uses its valid coarse material dependencies.
- Fine geometry waits or uses a declared compatible lower texture mip/material tier.
- Missing textures never become arbitrary white or default material acceptance.
- Glass, metal, foliage, entrance, asphalt, terrain, and emissive semantics remain distinguishable.

Virtual textures are orthogonal but coordinated. Geometry page priority can request virtual-texture pages based on projected coverage. Texture eviction does not invalidate geometry topology, but it advances the material-residency generation consumed by diagnostics and history policy.

Emission uses a hierarchy parallel to geometry. Fine resident emissive triangles may provide exact sampling records; coarse parents provide aggregate power and occupancy. Refinement preserves total expected energy within the validation tolerance.

## 22. Stereo, multiview, and XR

### 22.1 Shared state

The following are shared when valid:

- Portable manifests and page payloads.
- GPU geometry buffers.
- Material and texture tables.
- Static BLAS/CLAS and normally the TLAS.
- World-space ray caches and emitter distributions.
- Page IO and residency.

### 22.2 Per-eye state

Each eye retains:

- Current and previous view/projection transforms.
- Frustum and occlusion hierarchy.
- Visible-cluster output or per-eye bitset.
- Primary depth, identity, barycentrics, and material guides.
- Motion/disocclusion.
- Jitter and sample sequence.
- Ray reservoirs with screen-space identity.
- Denoiser/upscaler history and output.

### 22.3 Selection policy

Shared hierarchy refinement uses the strictest projected error requested by either eye. Per-eye occlusion is applied afterward. Foveated rendering uses the strictest quality required for any overlapping eye region and provides conservative transition bands.

Cross-eye occlusion reuse is experimental. It remains disabled until explicit stereo-occlusion fixtures prove that thin nearby objects, eye-separated disocclusions, and rapid head motion do not disappear.

### 22.4 History invalidation

A hierarchy transition does not inherently reset history if stable surface mapping, motion, depth, and material identity remain valid. History invalidates locally or globally when:

- Surface mapping cannot relate the old and new cuts.
- A silhouette or depth discontinuity appears beyond tolerance.
- Material or opacity semantics change.
- A page generation mismatch occurs.
- Camera/origin discontinuity requires it.

The compiler should emit coarse-to-fine correspondence sufficient for stable motion and disocclusion where practical. History may never reuse a parent primitive ID as if it were the same triangle as a child.

## 23. Editor integration

The editor treats virtual geometry as one resource/instance, not a generated node hierarchy. It provides:

- Enable/disable and declared fallback status.
- Source and compiled statistics.
- Cluster, group, page, and macro-HLOD visualization.
- Geometric-error heatmap.
- Resident/requested/active/retiring page visualization.
- Per-eye selected and occluded clusters.
- Raster versus ray representation comparison.
- BLAS/CLAS/TLAS ownership and active transport tier.
- Material, texture, and emission dependency status.
- Streaming misses, coarse fallback exposure, and prediction accuracy.
- Stable feature/provenance inspection from a picked pixel or ray hit.
- Bounded pinning of a resource, feature, group, or cell.
- Conventional-mesh fallback reason.

Opening an editor scene does not load complete source geometry or all descendant pages. Editor Flux Preview uses the same streamer and renderer path as runtime. Reference inspection can explicitly request a bounded high-detail region.

Import settings distinguish source intent from device caches. Artists normally choose quality constraints and semantic locks, not hand-author cluster sizes for each backend.

## 24. Capability detection and fallback matrix

| Capability | Preferred path | Fallback |
|---|---|---|
| Metal mesh/object shaders | Native mesh-shader raster | Compute cull plus indirect indexed raster |
| Metal hardware ray tracing | Native Flux Metal AS/intersection path | Explicit unsupported or declared lower feature mode; never fake ray support |
| Vulkan mesh shader | `VK_EXT_mesh_shader` | Compute cull plus indirect indexed raster |
| Vulkan ray tracing | KHR BLAS/TLAS | Explicit unsupported Flux ray mode |
| NVIDIA cluster AS | CLAS/cluster BLAS | Ordinary KHR BLAS |
| Opacity micromaps | Hardware alpha acceleration | Bounded any-hit/candidate path or opaque proxy |
| Fast resource loading/device IO | Direct backend upload path | Worker IO, staging, and async copy |
| Sparse/placement resources | Measured sparse residency | Heap/suballocation residency |

Capability detection records feature availability and the selected execution path. Merely compiling an API symbol does not certify performance or correctness.

## 25. RenderingDevice and backend work

### 25.1 Shared RenderingDevice additions

The production program likely requires product-neutral additions for:

- Mesh and task/object shader stages.
- Mesh pipeline creation and resource bindings.
- Direct and indirect mesh dispatch.
- Backend limits for mesh outputs and payloads.
- Explicit upload/completion/fence tokens suitable for page activation.
- Buffer device-address or equivalent table access where portable.
- Acceleration-structure build/update/compaction lifecycle completeness.
- Acceleration-structure memory and timing diagnostics.
- Optional capability query for cluster acceleration without exposing vendor object types to scene code.

These additions are engine capabilities, not Flux-only shortcuts. Forward renderers may consume them later, but Flux supplies the first implementation and validation.

### 25.2 Metal driver boundary

The current native Flux Metal implementation and generic Metal RenderingDevice ray stubs need an explicit convergence decision:

- Either implement the required Metal AS/intersection primitives in RenderingDevice and migrate Flux to them.
- Or retain a documented native Metal Flux adapter behind a product-neutral renderer interface while RenderingDevice gains only the shared resource and mesh-shader functions.

The decision is based on API completeness, upstream suitability, performance, command-buffer ownership, MetalFX integration, and ability to expose real completion/timing. Two uncoordinated Metal AS owners are not an accepted final architecture.

### 25.3 Vulkan driver boundary

The ordinary KHR path extends the existing RenderingDevice BLAS/TLAS implementation with any missing update, compaction, query, timing, deferred-destruction, and diagnostic primitives. The NVIDIA cluster path remains a Vulkan-driver adapter with capability-gated renderer hooks.

## 26. Frame graph

An illustrative Flux frame executes:

1. Publish camera, stereo, origin, animation, and world-streaming state.
2. Complete prior IO/decode/upload/AS tokens.
3. Atomically activate complete page and ray-structure groups.
4. Predict and prioritize future requests.
5. Build coarse candidate lists from world cells and baked primary visibility.
6. Run per-view virtual-geometry hierarchy selection and current-frame occlusion.
7. Compact mesh/indirect raster work.
8. Rasterize primary visibility, depth, motion, material identity, and guides.
9. Update eligible dynamic geometry.
10. Build/refit/swap ray structures whose inputs completed without blocking primary work.
11. Update the shared scene TLAS and transport tables.
12. Trace selected Flux effects per eye.
13. Reconstruct/denoise per eye.
14. Composite transparent overlays, editor overlays, and XR outputs.
15. Record last-use tokens and schedule safe retirement.
16. Publish bounded diagnostics and stage timings.

Async compute and copy overlap are introduced only after dependencies and profiler evidence show they reduce the critical path without queue contention or hidden synchronization.

## 27. Failure behavior

### 27.1 Missing or corrupt page

- Reject the page.
- Keep the complete active ancestor.
- Record page ID, dependency, source, retry state, and visible impact.
- Retry under a bounded policy when the cause is transient.
- Never activate a partial payload.

### 27.2 GPU allocation failure

- Stop lower-priority refinement.
- Evict eligible detail only after safe retirement.
- Preserve persistent coarse raster and ray worlds.
- Report requested, resident, reserved, fragmented, pinned, and dependent bytes.

### 27.3 Unsupported material or geometry

- Select a declared conventional representation or visible diagnostic material.
- Preserve semantic identity.
- Count affected instances, surfaces, pixels, and rays where practical.
- Do not claim the virtual path for that content.

### 27.4 Late acceleration structure

- Retain the active coarse ray structure.
- Raster detail may refine independently if the raster/ray mismatch remains within the declared quality contract.
- Sharp effects that require unavailable detail use the named coarse tier or wait; they do not trace empty space.

### 27.5 Invalid visibility data

- Fail open to spatial raster candidates and conservative transport closure.
- Record dependency mismatch and affected cells.

### 27.6 Device loss or backend reset

- Discard device caches, runtime RIDs, completion tokens, and acceleration structures.
- Retain portable manifests and CPU content cache where valid.
- Reestablish persistent roots before detail.

## 28. Diagnostics

### 28.1 Compiler diagnostics

- Source and canonical triangle/vertex counts.
- Supported, rejected, repaired, and conventional-path primitive counts.
- Cluster/group/page counts by level.
- Maximum and distribution of vertices/triangles per cluster.
- Error distributions.
- Protected boundary and simplification failure counts.
- Quantization error and bound expansion.
- Compressed/decoded bytes and ratios by stream.
- Material, opacity, semantic, and instance partitions.
- Determinism hash.
- Peak RSS and stage timings.

### 28.2 Runtime residency diagnostics

- Pages by lifecycle state.
- Resident/reserved/in-flight/persistent/evictable bytes.
- Read/decode/upload bytes and time.
- Request sources, priorities, misses, cancellations, and starvation.
- Coarse fallback exposures and duration.
- Fragmentation and relocation.
- Prediction hit/waste rates.

### 28.3 Raster diagnostics

- Candidate, frustum-rejected, error-coarsened, cone-rejected, occlusion-rejected, and selected clusters per eye.
- Selected source and rendered triangles.
- Mesh or indirect dispatch counts.
- Material batches and descriptor misses.
- Depth-pyramid and selection timings.
- Overdraw and shaded pixel counts where supported.
- Active backend path.

### 28.4 Ray diagnostics

- Ray groups and triangles by transport tier.
- Ordinary BLAS, Metal primitive AS, CLAS, cluster BLAS, and TLAS counts.
- Build, refit, compaction, swap, reuse, and retirement counts.
- Build scratch and final AS bytes.
- Build/refit/trace timings.
- Off-screen retained geometry.
- Raster/ray proxy substitutions and rejection reasons.
- Rays by role and selected tier.
- Missing-detail fallbacks.

### 28.5 Stereo diagnostics

- Per-eye selected/visible clusters.
- Shared-union overhead.
- Eye-exclusive clusters and disocclusions.
- Per-eye history invalidations caused by hierarchy changes.
- Shared AS identity and per-view tracing execution.

## 29. Validation program

### 29.1 Compiler unit tests

- Malformed indices, non-finite positions, overflow, sparse offsets above 4 GiB, and truncated buffers.
- Deterministic output under different worker counts and source ordering.
- Deep hierarchy reachability and cycle rejection.
- Conservative parent bounds after serialization and quantization.
- Group coverage, protected boundaries, and crack-free mixed cuts.
- Material/opacity/sidedness partition preservation.
- Stable identity across physical page repacking.
- Incremental rebuild dependency scope.
- Compression/decompression round trip.
- Unsupported attribute and topology reporting.

### 29.2 Runtime lifecycle tests

- Every legal and illegal state transition.
- Cancellation at IO, decode, upload, descriptor, AS build, and retirement stages.
- Real completion before activation.
- No free before last raster or ray use.
- Atomic group fallback during partial child arrival.
- Budget exhaustion and deterministic priority.
- Device reset and cache reconstruction.

### 29.3 Raster correctness fixtures

- Mixed hierarchy cuts across a shared planar seam.
- Thin silhouette and subpixel triangle scene.
- High-frequency normal/UV/material grid.
- Negative and nonuniform transforms.
- Large-coordinate/floating-origin scene.
- Rapid camera turn with previous-frame occlusion invalidation.
- Directional and local shadow views.
- Transparent/conventional overlay intersection.
- Foliage aggregate and masked overdraw scene.

### 29.4 Ray correctness fixtures

- Mirror reflecting detail outside the raster frustum.
- Rough reflection choosing a coarser valid tier.
- Hidden shadow blocker and hidden emitter.
- Glass plane and opaque frame split.
- Parent/child ray-tier substitution without light leaks.
- Raster-detailed versus ray-coarse self-intersection comparison.
- Metal immutable BLAS swap and deferred destruction.
- Vulkan ordinary BLAS and NVIDIA CLAS parity.
- Alpha/opacity micromap equivalence where supported.

### 29.5 Stereo fixtures

- Near occluder visible to one eye only.
- Thin geometry crossing the stereo disparity boundary.
- Rapid head rotation and translation.
- Different per-eye cluster occlusion with shared residency.
- Foveation transition crossing one eye before the other.
- Hierarchy transition with independent per-eye history.

### 29.6 City fixtures

- Street canyon with façades, windows, entrances, fire escapes, lamps, trees, and traffic props.
- Rooftop and skyline view with vertical movement.
- Continuous road/curb/sidewalk/terrain seams.
- Night emission across fine and aggregate hierarchy levels.
- Fast traversal across package and floating-origin boundaries.
- Landmark silhouette retention.
- Off-screen ray transport closure.

### 29.7 Reference comparison

Every quality-affecting test captures:

- Highest-detail conventional/source raster reference.
- Progressive high-sample Flux reference using the strict ray geometry.
- Virtual raster and selected ray tiers.
- Fixed camera and motion sequence.
- Stable exposure, seeds, materials, and lighting.

Metrics include depth/silhouette disagreement, normal/material identity, perceptual image error, temporal instability, stereo disagreement, emission energy, and ray visibility. A good average metric cannot waive a structural crack, missing object, light leak, or stereo mismatch.

## 30. Performance acceptance

### 30.1 Measurement conditions

Every result records:

- Repository and package revisions.
- OS, SDK, compiler, GPU, driver/runtime, and power mode.
- Output and internal resolution per eye.
- Foveation and reconstruction settings.
- Scene, camera path, duration, warmup, and run count.
- Source and selected geometry counts.
- Material, light, texture, and transport settings.
- Cold and warm cache state.
- Separate CPU and GPU stage timings.
- Resident and peak memory.

### 30.2 Equal-quality gate

For a supported content class, virtual geometry is the default only when at least one of these is true without a material regression in another critical stage:

- Same reference quality with lower p95/p99 CPU submission and raster GPU time.
- Same reference quality with a smaller bounded geometry working set and no worse frame time.
- Materially higher source/detail quality at the same p95/p99 frame time and memory.
- Lower streaming hitch and activation cost at the same quality.

The initial target hypothesis is at least a 30% reduction in geometry-related main-thread submission or at least twice the source geometric detail within the same raster and geometry-memory envelope. These numbers are falsifiable program gates, not marketing promises.

### 30.3 Ray gate

The Metal path must show that stable transport groups cost less overall than rebuilding from the fine raster cut. It separately measures:

- Geometry upload.
- Primitive AS build.
- Compaction.
- TLAS update.
- Trace time by role.
- Memory by tier.
- Coarse/detail swap latency.

The RTX path compares ordinary KHR BLAS with CLAS/cluster BLAS at identical geometry and ray workloads. CLAS becomes preferred only after total build, trace, memory, and frame-pacing evidence wins.

### 30.4 Stereo 90 Hz gate

The final Windows production gate remains sustained true-stereo 90 Hz, an 11.11 ms frame budget, without frame generation. Virtual geometry passes its portion only when the complete Flux frame—including scene update, raster selection, primary visibility, AS work, tracing, reconstruction, and composition—meets the frozen quality profile over the required duration and p99/worst-frame policy.

Mac performance is an independent native target and parity reference. It is not required to match RTX timing, but it must demonstrate bounded, useful interactive behavior at the declared Mac profile and may not silently reduce named features.

## 31. Implementation milestones

### VG0 — contract and frozen baselines

Deliver:

- This document ratified as the working hypothesis.
- Frozen conventional versus current streamed-cluster fixtures.
- Exact current Mac package/runtime measurements.
- Canonical capture and diagnostic schema.

Acceptance:

- Existing behavior and known limitations are reproducible.
- No performance or feature claim relies on the synchronous prototype alone.

### VG1 — production portable format and deep compiler

Deliver:

- Deep crack-free group hierarchy.
- Complete static vertex/material streams.
- Stable logical IDs independent of page packing.
- Versioned manifest and compressed pages.
- Deterministic incremental build.
- Conventional-path reporting.

Acceptance:

- Compiler suites pass on synthetic, city, and large external sources.
- Mixed hierarchy cuts are crack-free.
- Repeated builds are byte deterministic.
- Full-source processing remains within declared memory and file-range bounds.

### VG2 — renderer-owned residency and upload

Deliver:

- `VirtualGeometry` storage and one-instance scene boundary.
- Worker IO/decode.
- Renderer-owned heaps/suballocation.
- Real upload and retirement completion.
- Atomic group activation.
- No page-level nodes.

Acceptance:

- Streaming fixtures expose no holes or use-after-free.
- Main-thread work remains within its declared budget.
- Failures retain valid coarse coverage.

### VG3 — Metal GPU-driven raster

Deliver:

- RenderingDevice mesh/object shader capability or the approved native boundary.
- Metal object/mesh shader cluster path.
- Compute/indirect reference fallback.
- Per-eye selection and occlusion.
- Flux primary visibility/material integration.

Acceptance:

- Mesh and fallback paths match within reference tolerance.
- Rapid motion and stereo fixtures remain correct.
- Equal-quality performance gate passes on the reference Mac.

### VG4 — Metal ray-structure hierarchy

Deliver:

- Stable near/middle/far ray groups.
- Async immutable primitive AS build and compaction.
- Persistent coarse ray fallback.
- TLAS group substitution and safe retirement.
- Transport-role selection and diagnostics.

Acceptance:

- Off-screen transport and raster/ray mismatch fixtures pass.
- No per-frame BLAS rebuild follows ordinary raster cut changes.
- Total same-quality Flux performance improves or supports materially higher detail within the same envelope.

### VG5 — city compiler and runtime integration

Deliver:

- City-package cluster resources and dependencies.
- Building, road, terrain, repeated-module, material, texture, emission, ray, collision, and visibility integration.
- Floating-origin and traversal prediction.
- Editor inspection.

Acceptance:

- Representative street, roof, canopy, night, and traversal gates pass.
- Working sets remain bounded independently.
- No node or instance starvation removes required semantic classes.

### VG6 — portable Vulkan parity

Deliver:

- Vulkan mesh-shader or compute/indirect raster path.
- Ordinary KHR BLAS/TLAS ray-group path.
- Matching package consumption and diagnostics.

Acceptance:

- Canonical Metal/Vulkan scenes satisfy semantic and visual tolerances.
- Unsupported capabilities select declared fallbacks.
- Windows execution provides separate stage timings.

### VG7 — RTX cluster acceleration

Deliver:

- Capability-gated CLAS, template, and cluster-BLAS adapter.
- Optional opacity-micromap integration.
- Device-driven construction and device cache.
- Ordinary KHR comparison path retained.

Acceptance:

- Identical source geometry and quality compared A/B.
- CLAS path wins total build/trace/memory/frame-pacing criteria before becoming preferred.
- Adapter disablement returns to ordinary Vulkan without content changes.

### VG8 — dynamic geometry experiments

Deliver:

- Rigid moving-instance certification.
- Bounded fixed-topology clustered deformation experiment.
- Destruction/partition experiment.

Acceptance:

- Promote only classes that improve total cost while preserving motion, bounds, material, ray, and stereo correctness.
- Conventional dynamic geometry remains available for rejected classes.

### VG9 — production stereo and long-run certification

Deliver:

- Physical XR stereo execution.
- Foveation integration.
- Long traversal and cache-pressure runs.
- Final Metal native profile and RTX 5080 90 Hz profile.

Acceptance:

- No copied-eye output, geometry holes, stale ray structures, invalid history, unbounded growth, or hidden quality reductions.
- Full-frame target gates pass under frozen conditions.

## 32. Implementation ownership map

The expected ownership is provisional and should remain narrow:

- `modules/meshoptimizer/`: offline clustering, hierarchy generation, compression integration, and import hooks until an accepted production module boundary replaces it.
- `servers/rendering/virtual_geometry/`: portable manifest/runtime descriptors, residency scheduling, selection contracts, and diagnostics. Create this production directory only when VG1/VG2 justify it.
- `servers/rendering/renderer_rd/flux/`: Flux primary visibility, transport selection, frame-graph integration, and backend-neutral consumption.
- `servers/rendering/rendering_device*`: product-neutral mesh pipeline, completion, resource, and acceleration-structure abstractions.
- `drivers/metal/`: Metal mesh/object shader pipeline, resource upload/lifetime, and any accepted generic Metal AS primitives.
- Native Metal Flux adapter: Metal-specific ray-structure construction and trace integration until or unless it moves into RenderingDevice.
- `drivers/vulkan/`: Vulkan mesh pipeline, ordinary KHR AS improvements, and capability-gated NVIDIA cluster adapter.
- `scene/3d/` and `scene/resources/`: minimal engine-facing resource and instance APIs.
- `editor/`: import settings, visualization, inspection, and bounded pinning.
- `misc/path_tracing/`: fixtures, scripts, benchmark packets, milestone evidence, and implementation journal entries.

Product-specific city compiler work consumes the engine resource format from its own repository. Generic engine code must not contain city names, game titles, licensed asset names, or gameplay terminology.

## 33. Migration from the current prototype

The current `StreamedClusterMesh` format is an experimental two-level, position-only proof. It should not be expanded indefinitely into the production schema through compatibility fields.

Migration sequence:

1. Freeze its tests and evidence as the VG0 baseline.
2. Extract validated logical concepts: stable identities, conservative bounds, group-atomic activation, explicit lifecycle, stereo union, and persistent fallback.
3. Introduce the new production manifest/page format under a separate format identity.
4. Implement a one-way experimental converter only if it materially aids fixtures; production content is recompiled from canonical sources.
5. Replace the synchronous `ArrayMesh`/`MeshInstance3D` consumer with renderer-owned storage.
6. Remove or clearly deprecate the experimental node after all fixtures and importers use the production path.

No runtime dual decoder or long-term migration path is required for unshipped experimental packages.

## 34. Hypothesis register

| ID | Hypothesis | Falsification test | Fallback |
|---|---|---|---|
| VG-H1 | A deep group hierarchy can replace routine static raster LOD chains. | Equal-quality static city and hero-asset comparison under motion. | Retain conventional LODs for rejected content classes. |
| VG-H2 | Metal mesh shaders outperform compute/indirect or conventional draws for representative virtual geometry. | Same selected clusters and materials across both Metal paths. | Use compute-generated indirect draws. |
| VG-H3 | Stable Metal ray groups provide the geometry benefits without prohibitive BLAS churn. | Compare stable groups against fine-cut rebuild and conventional city ray proxies. | Coarser authored/compiled ray proxies with virtual raster only. |
| VG-H4 | Shared cluster semantics can serve Metal, portable Vulkan, and RTX CLAS. | Canonical package parity and total-cost profiling on both backends. | Backend-derived repacking or separate optimized adapters behind the same logical resource. |
| VG-H5 | RTX CLAS materially improves build/trace/memory for the target static and dynamic workloads. | Ordinary KHR BLAS versus CLAS A/B on RTX 5080. | Keep ordinary KHR BLAS. |
| VG-H6 | One shared stereo residency cut plus per-eye occlusion is cheaper than fully separate cuts. | Rapid-motion stereo fixture with union-overhead profiling. | Separate per-eye selected lists sharing resident buffers. |
| VG-H7 | Compiler-derived ray tiers preserve sharp reflection and shadow quality. | Progressive strict-geometry comparison across material/transport fixtures. | Authored ray proxies or stricter detailed ray cuts for failed assets. |
| VG-H8 | Virtual geometry improves complete Flux performance rather than moving cost into IO, selection, or AS work. | Full-frame stage profile at equal quality. | Limit virtualization to content classes with measured net benefit. |
| VG-H9 | Stable coarse-to-fine mappings permit temporal continuity without global resets. | Animated hierarchy-transition and stereo disocclusion suite. | Local history rejection around transitions or explicit reset for affected views. |
| VG-H10 | City macro HLODs and virtual clusters can overlap without duplicate coverage or visible popping. | Altitude/traversal fixture with coverage and material/emission checks. | Stronger distance ownership bands or explicit representation handoff. |

## 35. Principal risks

### 35.1 Acceleration-structure churn on Metal

Fine raster selection can change every frame, while Metal primitive acceleration structures favor stable immutable geometry. Building BLAS from the raster cut would likely erase the benefit. Stable ray groups, asynchronous construction, hysteresis, and persistent coarse structures are therefore architectural requirements rather than later optimization.

### 35.2 Page granularity mismatch

Clusters want fine culling; storage wants larger sequential pages; ray structures want stable medium-sized groups. Treating one granularity as all three can cause transfer amplification, excessive descriptors, or BLAS churn. The format keeps cluster, page, group, and ray-group identities separate.

### 35.3 Material complexity dominating geometry

Virtual geometry reduces geometry work, not arbitrary shader cost. Dense layered materials, transparency, parallax, or unique textures may dominate after raster optimization. Material and texture diagnostics remain separate, and coarse material tiers are explicit.

### 35.4 Aggregate and alpha geometry

Foliage, hair-like surfaces, and layered masked cards can defeat simplification and occlusion. They require dedicated fixtures, opaque geometry alternatives, area preservation, canopy HLODs, and bounded ray alpha traversal.

### 35.5 Temporal instability

Continuous geometric transitions can disturb depth, normals, motion, reflections, and denoiser histories. Stable mappings, hysteresis, local invalidation, and motion-sequence acceptance are mandatory.

### 35.6 Stereo over-refinement

The conservative union of two eyes can increase selected clusters. The overhead is expected to be small for ordinary eye separation but must be measured for near geometry and foveation. Correctness takes priority over speculative cross-eye culling.

### 35.7 Vendor path defining shared semantics

NVIDIA CLAS is unusually well matched to virtual geometry, but Metal lacks the same object. Exposing CLAS in portable resources would make parity impossible. The engine contract remains triangle clusters plus stable groups; CLAS is a device adapter.

### 35.8 Compiler cost and cache scale

Deep hierarchy generation for very large sources can become memory- or time-unbounded. Every stage uses range reads, bounded windows, deterministic partitioning, resumable content-addressed outputs where useful, and explicit peak-memory diagnostics.

### 35.9 Debugging opacity

GPU-driven geometry can make missing triangles difficult to attribute. Stable IDs, selection counters, captureable page tables, hierarchy visualization, and source-feature remaps are required before broad content adoption.

## 36. Decisions intentionally deferred to measurement

- Exact leaf vertex and triangle limits beyond the portable baseline.
- Target compressed and decoded page sizes.
- Tree versus bounded DAG hierarchy.
- Exact simplifier and attribute-error weights.
- Visibility buffer versus direct thin-G-buffer primary path.
- Metal mesh shader versus compute/indirect preference on each GPU family.
- One shared ray TLAS versus mask-separated or role-specific TLAS objects.
- Ray-structure group size and number of transport tiers.
- Runtime versus build-time backend repacking.
- Opacity and displacement micromap adoption.
- Virtualization of fixed-topology deforming geometry.
- Whether a mature subsystem remains in `modules/meshoptimizer` or moves to a dedicated production module.

Each decision receives an isolated A/B experiment using identical source geometry, camera paths, materials, lights, quality thresholds, and cache state.

## 37. Primary references

The implementation must verify assumptions against current installed SDK headers and primary documentation at the time of each backend milestone. Initial reference points are:

- Apple, Metal mesh/object shader resource commands: <https://developer.apple.com/documentation/metal/mesh-and-object-shader-resource-preparation-commands>
- Apple, mesh-shader LOD sample: <https://developer.apple.com/documentation/metal/adjusting-the-level-of-detail-using-metal-mesh-shaders>
- Apple, ray tracing with acceleration structures: <https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures>
- Apple, acceleration-structure refit tradeoffs: <https://developer.apple.com/documentation/metal/mtlaccelerationstructureusage/refit>
- Apple, Metal feature tables: <https://developer.apple.com/metal/capabilities/>
- Khronos, `VK_EXT_mesh_shader`: current Vulkan registry and proposal.
- Khronos, `VK_KHR_acceleration_structure`: current Vulkan specification.
- Khronos, `VK_NV_cluster_acceleration_structure`: <https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_NV_cluster_acceleration_structure.html>
- Khronos, opacity micromaps: current `VK_EXT_opacity_micromap` specification.
- meshoptimizer documentation and the exact vendored revision recorded in the implementation journal.

Reference behavior from another engine may motivate experiments but does not establish Godot correctness, licensing, parity, or performance.

## 38. Final recommendation

Implement the full system as a foundational Flux capability.

For supported static opaque content, author one high-quality source representation and generate the raster cluster hierarchy automatically. Keep city macro HLODs and all nonvisual representations. On macOS, use Metal mesh shaders or the measured compute/indirect fallback for raster geometry, while building stable compact ray-structure groups that do not follow every fine raster transition. On Vulkan, provide the same portable behavior through ordinary BLAS/TLAS, then use NVIDIA cluster acceleration as an optional faster backend on supported RTX hardware.

The architecture should be adopted because it improves the scaling law of geometric quality, not because "virtual geometry" is intrinsically faster. Every content class and backend must still pass equal-quality total-frame measurements. The intended result is a renderer in which geometric detail is limited primarily by visible contribution and explicit memory/transport budgets, rather than manual raster LOD authoring, scene-node count, draw submission, or total source-scene size.
