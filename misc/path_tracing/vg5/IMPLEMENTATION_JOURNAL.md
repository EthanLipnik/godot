# VG5 spatial runtime core

This journal records the product-neutral portion of VG5 implemented in the
engine.  It deliberately does not select a city package serialization,
baked-visibility schema, material/texture adapter, or product compiler.

## Implemented

- Deterministic package-cell identities from authored package identity and
  integer cell coordinates. Repeated family and instance identities are
  derived from canonical authored identities, not node allocation order.
- Floating-origin state with camera-relative position and transform helpers.
  Rebasing changes only the origin/revision and camera-relative transforms;
  cell IDs, local coordinates, and topology are not rewritten.
- Conservative bounded traversal prediction using sampled
  position/velocity/acceleration, corridor and stereo margins, declared
  coarse corridors, and angular-turn detection. Rapid turns retain an
  omnidirectional coarse reserve; overflow is diagnosed and requests coarse
  fallback.
- Independent bounded working-set reservations for geometry, material,
  emission, ray, collision, visibility, and semantic data. Persistent-coarse
  reserve bytes are protected from ordinary detail requests. Optional
  per-semantic-class quotas prevent a repeated or dominant class from
  consuming its entire pipeline pool.
- Repeated-family accounting stores family/instance bookkeeping rather than
  creating one runtime scene node per repeated module.
- Conservative coarse eligibility with explicit fail-open behavior for
  invalid visibility or unavailable persistent coarse coverage.
- Deterministic tests cover rebasing invariance, 180-degree turns, vertical
  launch, diagonal crossings, repeated families, class/semantic starvation,
  and coarse fallback.

## Evidence and limits

The runtime core has no representative city package, city compiler output,
material/texture/emission dependency records, collision records, or external
baked-visibility schema in this repository. Those external assets and schema
decisions are therefore absent; full VG5 street/roof/canopy/night/traversal
scene acceptance remains blocked until a product-neutral package contract and
representative fixtures are supplied. No performance or city-scene acceptance
claim is made here.

## Next executable step

Define and validate the external city-package dependency adapters and fixtures
against this core, then integrate them at the world-streamer/scene boundary
without changing stable identity or budget semantics.
