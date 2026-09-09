---
id: surfer-integration
type: feature
epic: phase-ship
design: [docs/design/performance.md]
status: planned
---

# Surfer Integration

**Goal:** Land this in surfer as a real music source alongside local files — and get the
in-game performance verdict that no isolated harness can give.

**Why:** This library exists to serve surfer. Everything before this phase is preparation.

## Two halves

### 1. Consumption (Q2 — unresolved)

Surfer's addon taxonomy offers two patterns, and **neither cleanly fits a binary artifact**:

| Pattern | Fit | Problem |
|---|---|---|
| Submodule in `lib/` + `link-addons.sh` symlink | For repos under active iteration — that's us | Symlinked dirs are gitignored, so where does the built `.dll` live and who builds it? |
| Vendor the `addons/<name>/` folder directly | How third-party addons are installed | Means committing a binary into surfer and rebuilding it there on every change |

Neither is obviously right. A third option: treat the **built artifact** as the deliverable —
CI (`platform-matrix`) publishes per-platform binaries to a GitHub release, and surfer vendors
a specific released version like any other third-party addon. That decouples surfer from this
repo's build toolchain entirely, which matters because surfer's `verify.sh`/`test.sh` have no
C++ in them today and shouldn't grow any.

**Recommendation to confirm when the time comes:** the release-artifact route, precisely
because it keeps SCons and MSVC out of surfer's pipeline.

### 2. The performance verdict

This is the part `phase-measure` explicitly **cannot** answer (see
[`design/performance.md`](../design/performance.md)). Three questions need the real game:

- **Frame-time p99/p99.9 with radio playing** during real traffic + city streaming load.
- **Does it trip surfer's perf gate?** `perf-threshold-gate` already gates draw calls, frame
  mean, TTI and `hitch_ms_total` with per-origin attribution.
- **Audio underruns under real load** — the real audio thread competing with a real frame
  budget.

**Reuse surfer's instruments, don't build new ones.** `perf-threshold-gate`,
`frame-time-overlay` and `startup-profiling` already exist there and are the tools the project
trusts.

> ⚠️ **Capture noise is a documented trap.** Surfer's perf captures are known to be unreliable
> under machine contention (`perf-gate-noise-robustness`), and this project has already been
> bitten once — a real change was blocked by a `stall_ms_total` regression while another agent
> session was running Godot on the same machine. Any number taken here needs an uncontended
> capture, or it will mislead.

## Scope

- Wire `RadioStream` as a source in `LocalTrackPlayer`/`MusicLibrary` alongside the existing
  local sources, so radio appears in the existing music browser rather than as a parallel
  system.
- Station URLs as user-supplied config, not hardcoded (see Q3 — the licensing question is
  about *shipping* a station list, and user-supplied URLs sidestep it).
- `icy-metadata` feeds surfer's existing Now Playing label.
- Graceful absence: no network, or a dead station, must degrade exactly as gracefully as
  surfer's local-music path already does with no tracks present — that posture is established
  and shouldn't regress.

## Acceptance

- Radio plays in a real cruise, selectable from the music browser.
- Booting with no network is clean — no hang, no error spam, local music unaffected.
- An uncontended perf capture shows the frame-time impact, measured against surfer's gate.
- ⚑ Human checkpoint: does radio-as-a-music-source actually feel good in a cruise, and does
  the reconnect behaviour hold up over a long session?

## Links

up → `phase-ship` · needs → `reconnect-resilience`, `perf-harness`, `platform-matrix` ·
relates → surfer's `music-library-discovery`, `music-menu` · design → `performance.md`
