# Repository agent rules

## Fork purpose

- This is the custom Godot fork for a Spider-Man simulator, not a general-purpose upstream checkout. Future work in this repository should be evaluated in the context of building that simulator.
- Its central technical goal is a real-time stereoscopic hybrid/path-traced renderer for VR, targeting sustained true-stereo 90 Hz on an RTX 5080 without frame generation. macOS Metal is the current development backend; Windows Vulkan/RTX is required for production parity.
- Keep reusable engine-facing architecture and naming product-neutral even though this fork and its priorities are specific to the simulator.

## Renderer goal and authoritative documents

- The renderer program targets a reusable, product-neutral Godot hybrid/full path-tracing architecture for large dynamic environments. Its production performance gate is sustained true-stereo 90 Hz on an RTX 5080 without frame generation, while preserving equivalent named features on native macOS Metal and Windows Vulkan/RTX.
- The current many-light hypothesis is ReSTIR DI fed by a camera-centered ReGIR structure, with environment importance sampling. ReSTIR GI, ReSTIR PT, and a shared world-space radiance cache are competing or complementary hypotheses for indirect transport; select among them with isolated correctness, quality, memory, and GPU-stage measurements rather than treating an NVIDIA technique or SDK as a fixed requirement.
- Share view-independent scene acceleration structures, light distributions, ReGIR data, and world-space caches where valid. Keep screen-space reservoirs, visibility, motion/disocclusion validation, jitter, reconstruction, and temporal history independent per eye. Cross-eye sample reuse is experimental until it passes explicit stereo-occlusion tests.
- The authoritative architecture hypothesis is `/Users/ethan/Developer/godot/ADVANCED_RENDERING_ARCHITECTURE_HYPOTHESIS.md`.
- The active Mac hybrid implementation journal is `/Users/ethan/Developer/godot/misc/path_tracing/m2_5/IMPLEMENTATION_JOURNAL.md`. Milestone-specific evidence belongs in the matching `/Users/ethan/Developer/godot/misc/path_tracing/<milestone>/IMPLEMENTATION_JOURNAL.md`; do not create a parallel undocumented plan.
- The current provisional implementation is centered in `/Users/ethan/Developer/godot/servers/rendering/renderer_rd/effects/metal_hybrid_effect.{h,cpp}` and its Forward+ scheduling/integration under `/Users/ethan/Developer/godot/servers/rendering/renderer_rd/forward_clustered/`. These locations describe current ownership, not permission to make vendor-specific semantics the shared contract.

## Scope and naming

- Build reusable Godot engine capabilities. Product-specific projects may motivate and consume the work, but must not own the engine architecture.
- Keep engine directories, modules, classes, symbols, APIs, project settings, diagnostics, validation scenes, and tooling product-neutral. Do not use game titles, character names, trademarks, proprietary asset names, or gameplay terminology in reusable engine code.
- Keep product plans, licensed assets, gameplay logic, and application-specific integration outside generic renderer and platform infrastructure.
- Prefer established Godot subsystem names. Experimental path-tracing work belongs under `misc/path_tracing` until an accepted architecture justifies production placement.

## Architectural evidence

- Treat architecture documents as falsifiable hypotheses, not fixed specifications.
- Before changing architecture, reproduce cited local-code findings and verify material assumptions against current primary documentation, installed SDK headers, target toolchains, and measured prototypes.
- Record hypotheses, evidence, falsification tests, and fallbacks. Update the relevant architecture document when evidence materially refines or disproves it.
- Use capability detection and clean adapter boundaries instead of hard-coded device or vendor assumptions.

## Renderer parity and milestones

- Path-tracing renderer work targets native macOS Metal and Windows Vulkan/RTX implementations behind shared semantic contracts.
- Do not declare a renderer feature complete unless both backends satisfy the documented parity definition for scenes, materials, lights, cameras, dynamic geometry, stereo, guide outputs, diagnostics, and validation.
- Do not move past a milestone gate when a required backend or acceptance test has failed. A prototype must remain clearly labeled as a prototype.
- Preserve independent per-view camera state, outputs, motion, disocclusion, jitter, and temporal history. Copying one monoscopic result to both eyes is not stereo support.

## Validation and measurement

- Add deterministic validation fixtures, diagnostics, and automated checks with implementation work.
- Measure GPU stages separately, including deformation, BLAS build/refit, TLAS update, tracing, denoising/upscaling, and composition.
- Investigate correctness failures before optimizing performance. Never hide stale geometry, invalid guides, unsupported materials, or reconstruction defects behind fallbacks.
- Record exact repository/donor revision, OS, SDK, compiler, GPU, driver/runtime, resolution, sample count, scene, run count, warmup, and benchmark conditions.
- Maintain a concise implementation journal with decisions, evidence, measurements, unresolved risks, failed gates, and the next executable step.

## Dependencies and reference repositories

- Keep proprietary SDKs optional and isolated behind generic adapters. Never commit restricted binaries, credentials, signing material, or machine-specific secrets.
- Before inspecting `/Users/ethan/Developer/Flow Engine`, read its `AGENTS.md` completely. Treat that repository as read-only unless the user explicitly authorizes changes.
- Pin and record donor revisions for adapted code, preserve applicable license notices, and prefer reimplementation from verified behavior when donor coupling is excessive.

## Change discipline

- Preserve upstream Godot conventions and keep changes narrow, reviewable, and reusable.
- Do not make unrelated changes, version bumps, commits, pushes, publications, or external releases unless explicitly requested.
- Treat marketing versions, build numbers, package versions, protocol versions, packet schemas, and SDK compatibility as independent contracts.

## Unshipped renderer contracts

- This custom hybrid renderer is still in development and has not shipped. Renderer-facing settings, APIs, and schemas may be hard-cut over without compatibility or migration paths unless the user explicitly requests one.
- Do not weaken requirements because hardware, SDKs, assets, credentials, or licenses are unavailable. Complete productive prerequisites, document the exact blocker, and identify the smallest action needed to unblock it.

## Local editor deployment

- After finishing a task that changes engine or editor behavior, make a fresh macOS editor app build from the task's worktree, replace `/Applications/Godot.app` with that build, and verify the installed executable against the task's relevant project or fixture before handoff.
- Replace the installed app directly; do not retain the previous installation as a backup unless the user explicitly asks for one.
- Do not treat a matching Godot version string as deployment evidence. Verify the installed executable itself, because separate worktrees can produce different binaries with the same source revision and version label.
