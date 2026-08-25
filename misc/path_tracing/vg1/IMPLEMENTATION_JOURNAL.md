# VG1 portable virtual geometry implementation journal

## 2026-08-24 — portable format/compiler tranche

Delivered a separate `VG1` portable package identity under
`servers/rendering/virtual_geometry/` and an offline compiler in
`modules/meshoptimizer/`. The package has independently versioned manifest,
page payload, compiler semantic, compression, material-schema, and ray-hint
contracts. It contains static stream schemas, material semantic partitions,
logical clusters, refinement groups, persistent root sets, checked 64-bit page
ranges, dependency hashes, and explicit conventional-path diagnostics.

The compiler validates canonical static opaque inputs, keeps all declared
position/normal/tangent/UV/color/joint/weight streams in the cluster payload,
partitions triangles by material semantic, creates bounded leaves, and builds a
deterministic deep, group-atomic hierarchy. VG1 coarse groups are
coverage-preserving high-precision reclusters with zero geometric error: every
mixed cut is therefore exact and crack-free by construction. This is a
correctness-first baseline; protected-boundary simplification and nonzero
conservative errors remain a quality improvement, not a hidden fallback.

Pages are deterministic packed compressed Zstd buffers. Package validation
checks descriptor IDs and references, page and payload ranges, checksums and
compression round trips, roots, reachability, and parent-cycle failures before
any renderer consumes the data. Logical cluster IDs are derived from source
identity/digest, canonical triangle coverage, material semantic partition, and
compiler semantic generation, not physical page packing.

Validation on the local macOS tests-enabled editor build:

- Build: `platform=macos arch=arm64 target=editor dev_build=yes tests=yes vulkan=no angle=no accesskit=no -j4`.
- Focused suite: `--test --test-case='*VirtualGeometry*'`.
- Result: 3 test cases, 760 assertions, all passed.

The suite covers malformed topology, conventional-path declaration, deep
hierarchy and mixed-cut structure, repeated byte determinism, stable cluster
IDs across page repacking, high-precision decode bounds, compression checksum
failure, and checked payload-range rejection.

Limitations: VG1 deliberately has no importer, scene resource, IO worker,
renderer residency, GPU upload, device cache, backend adapter, dynamic
geometry, or performance measurement. It also uses high-precision positions;
quantized payload profiles require a separate error-bound proof.

Next executable VG2 step: add renderer-owned `VirtualGeometryStorage` that
parses this manifest, asynchronously reads/validates/decompresses pages into
renderer-owned allocations, and activates group cuts atomically while retaining
persistent coarse coverage on every failure path.

## 2026-08-24 — hierarchy simplification correction

The initial VG1 hierarchy intentionally used exact coarse reclusters. That was
safe for mixed cuts but did not reduce primitive work, so it was not an
acceptable production LOD hierarchy. The compiler now invokes the in-repo
`meshopt_simplify` with `meshopt_SimplifyLockBorder` for every multi-child,
single-semantic group. It uses the simplifier's measured relative error scaled
by `meshopt_simplifyScale`, adds it to the maximum inherited child error, and
stores the resulting conservative nonnegative world-space group error. Coarse
cluster bounds retain the fine-group bounds, preserving conservative selection
and quantization coverage.

If the locked-border simplifier cannot produce a smaller valid triangle set,
the compiler preserves exact current-cut coverage and emits a diagnostic rather
than claiming a reduction. This is expected for all-boundary, disconnected,
or topology-constrained groups. The focused tests now include both a
simplifiable grid reduction/nonzero-error check and an explicit
protected-border fallback check.

The deterministic hierarchy fixture was expanded to a 17×17 curved grid after
a 9×9 input produced only two groups under the bounded leaf/group profile. The
assertion remains `groups > 2`; the fixture now forces multiple refinement
rounds rather than weakening the depth contract. The rebuilt focused
`*VirtualGeometry*` suite completed in 0.4 s: 11 cases and 654 assertions,
all passed.
