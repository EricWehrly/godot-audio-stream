---
id: platform-matrix
type: feature
epic: phase-ship
status: planned
---

# Platform Matrix & CI Builds

**Goal:** Builds for the platforms surfer targets, produced by CI rather than by hand.

**Why:** Only a hand-built Windows x86_64 debug binary exists. Two problems: the artifact
isn't reproducible by anyone else, and hand-building is exactly how a stale binary ends up
shipped. It also unblocks `surfer-integration`'s recommended consumption route (vendor a
released artifact, keeping SCons and MSVC out of surfer's pipeline).

Scoped to `phase-ship` rather than earlier per **D1** — surfer-first means platform breadth
follows evidence of usefulness rather than preceding it.

## Scope

- **Targets:** `template_debug` and `template_release` for Windows x86_64, Linux x86_64,
  macOS (universal). Web is impossible (**D3**) and must not appear in the matrix.
- **CI** (GitHub Actions, since the repo is on GitHub) building all targets on tag, publishing
  a release archive laid out for direct vendoring: `addons/<name>/` with the `.gdextension`
  and `bin/`.
- Extend the `.gdextension` `[libraries]` block to list every built target.
- **Verify the toolchain workaround is CI-safe.** `SConstruct`'s `find_vcvars()` exists for a
  broken local VS install; on a clean CI runner it should find the normal instance, but that
  must be confirmed rather than assumed — and the non-Windows path (`os.name != "nt"` →
  `None`) must be exercised, which it never has been.
- Run `test_load.gd` on each platform in CI as a smoke test.

## Non-goals

- Godot asset library publication (**D1** — no third-party promises yet).
- 32-bit, ARM Windows, or mobile until something needs them.
- Web (**D3**).

## Acceptance

- A tagged release produces downloadable archives for all three platforms.
- Each loads and passes `test_load.gd` on its own platform.
- The non-Windows build path works without the MSVC workaround interfering.
- A fresh clone builds with documented commands and no undocumented local setup.

## Open

macOS is unavailable locally, so it will be CI-verified only — no local debugging if it
breaks. Linux is verifiable through WSL if needed. Worth deciding whether macOS is worth
carrying at all under D1, given surfer targets Windows today; it may be honest to ship
Windows + Linux and add macOS when someone actually needs it.

## Links

up → `phase-ship` · unblocks → `surfer-integration` (release-artifact consumption route)
