# VG7 implementation journal — RTX cluster acceleration

## Status

VG7 is not accepted and no RTX/CLAS runtime path is implemented in this
repository. Ordinary KHR BLAS/TLAS remains the required comparison and
fallback path.

## Implemented prerequisites

- VG1's portable package deliberately contains ordinary clusters, stable
  identities, material/opacity semantics, bounds, hierarchy groups, and ray
  grouping hints; it contains no CLAS, cluster-BLAS, opacity-micromap, or
  machine-specific acceleration-structure objects.
- VG2's page lifecycle and VG4's transport-group lifecycle provide the
  residency, complete-group activation, coarse fallback, completion-token,
  and last-use-retirement semantics an adapter must preserve.
- The Vulkan driver exposes ordinary KHR acceleration-structure capability
  discovery and BLAS/TLAS build commands. Prior SDK/header inspection also
  confirmed that NVIDIA CLAS/opacity-micromap declarations are available to
  investigate, but no renderer adapter, device-driven construction, cache, or
  runtime validation exists in this checkout.

## Missing work and required comparison

The architecture's optional path requires a capability-gated Vulkan adapter
that can:

- repack canonical clusters into CLAS objects, preserve portable 32-bit
  identity mappings, assemble cluster BLAS, and support repeated topology
  templates;
- construct resident structures device-driven where beneficial, maintain a
  device cache, and attach opacity micromaps only for explicitly supported
  masked content;
- expose actual-path diagnostics and atomically disable itself back to
  ordinary KHR BLAS/TLAS without changing package content or scene semantics.

None of those behaviors has been measured or validated. The ordinary KHR
path must be implemented and retained as the correctness baseline before an
RTX fast path can be preferred. CLAS may become preferred only after an A/B
run with identical geometry, materials, ray workloads, quality thresholds,
and working-set budgets shows a win in total build cost, trace cost, memory,
and frame pacing—not from an isolated build or trace number.

## Why blocked

The current environment has no Windows RTX runtime, RTX 5080 target, or
representative virtual-city package with masked foliage/fence workloads. Mac
Metal execution cannot validate Vulkan CLAS, cluster-BLAS, or opacity-micromap
semantics. Header presence is therefore only a source-availability fact, not
evidence that the extensions are enabled, callable, correct, or faster.

## Next executable steps

1. Finish the ordinary KHR Vulkan ray-group path and diagnostics first,
   including the fallback switch and identical capture format.
2. Add a capability-gated CLAS/cluster-BLAS adapter behind the same logical
   resource and stable identity tables; add template, device-cache, and
   opacity-micromap experiments only where the target device advertises the
   required features.
3. Run paired RTX captures with ordinary KHR and CLAS paths, then retain KHR
   as preferred unless total-cost and quality gates pass. Re-run with the
   adapter disabled to prove content-preserving fallback.
