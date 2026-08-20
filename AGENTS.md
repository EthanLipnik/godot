# Repository agent rules

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
- Do not weaken requirements because hardware, SDKs, assets, credentials, or licenses are unavailable. Complete productive prerequisites, document the exact blocker, and identify the smallest action needed to unblock it.
