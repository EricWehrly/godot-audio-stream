---
id: platform-matrix
type: feature
epic: phase-ship
design: [docs/design/decisions.md]
status: planned
---

# Build Command & Platform Reach

**Goal:** A clean, documented, one-command build producing a vendorable artifact. Additional
platforms and CI automation only when something actually needs them.

**Why (D7):** *Do no more work than surfer needs.* Surfer needs one thing from this repo — a
built artifact it can vendor, without dragging SCons or MSVC into its own pipeline. That's a
build command and a README section, not a release pipeline.

Deliberately de-scoped from an earlier "build all platforms in CI on tag" plan. That's the
conventional GDExtension endpoint (see D7) and remains the destination, but it's automation
we haven't earned yet.

## Scope (now)

- **One command produces the artifact**, laid out ready to vendor:
  `addons/godot_audio_stream/` containing the `.gdextension` and `bin/`.
- A short build section in the README: prerequisites, the command, where the output lands.
  It should be honest about the local Visual Studio workaround `SConstruct` carries.
- Build `template_release` as well as `template_debug` — currently only debug has ever been
  built, and the release path is therefore unverified.
- Verify a **clean clone builds** with only the documented steps. The most likely failure is
  an undocumented assumption about this machine.

## Scope (deferred until triggered)

| Deferred | Trigger |
|---|---|
| Linux / macOS builds | Surfer targets that platform, or someone asks |
| GitHub Actions CI | Hand-building becomes a real burden or a stale binary ships |
| Tagged releases with archives | There's an external consumer to release *to* |
| Godot asset library entry | D1 changes — no third-party promises today |

Web is impossible (**D3**) and must never appear in the matrix.

## Notes for when platforms do get added

- `SConstruct`'s `find_vcvars()` exists for a broken local VS install and returns `None` on
  non-Windows — a path that has **never been exercised**. Confirm it before assuming the
  Linux build is a no-op change.
- macOS is unverifiable locally, so it would be CI-only with no local debugging. Worth
  questioning whether it's worth carrying at all under D1.

## Acceptance

- A fresh clone builds with the documented command, no undocumented setup.
- Both debug and release targets build and load (`test_load.gd` passes).
- The output directory is directly vendorable — copy it into a Godot project and it works.

## Links

up → `phase-ship` · decisions → D7 (scope), D3 (no web) · unblocks → `surfer-integration`
