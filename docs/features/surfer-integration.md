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

### 1. Consumption — settled (D7)

Surfer's two existing addon patterns both fit awkwardly: a `lib/` submodule + symlink leaves
nowhere sensible for a built `.dll` (symlinked dirs are gitignored), and vendoring the folder
directly means rebuilding a binary inside surfer on every change.

**Resolved: the built artifact is the deliverable.** This repo supports a documented
one-command build (`platform-matrix`); surfer references the repo by link and vendors the
output like any other third-party addon.

The property that matters: **surfer's pipeline never needs SCons or MSVC.** Its
`verify.sh`/`test.sh` have no C++ in them today and shouldn't grow any.

This is the conventional GDExtension lifecycle with the automation deferred — normally CI
builds per-platform on tag and publishes an `addons/<name>/` archive to a release, which is
where this ends up if it's ever worth automating. Until then, a build command and a link is
genuinely enough.

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
- **Station selection (D8):** a directory browser backed by
  [Radio Browser](https://api.radio-browser.info/) (~58k stations, no API key, explicitly
  permits use in non-free software) plus a user-URL field. **Ship no curated station list** —
  that's the one option with real downside, and it inherits both maintenance and per-station
  ToS review. Radio Browser needs a descriptive `User-Agent`, dynamic server resolution, and
  has no uptime guarantee, so it must degrade gracefully.
  - This is surfer-side work: it's HTTP + JSON in pure GDScript, and stays a non-goal for
    this library.
  - Worth weighing against D8's alternative: a small set of **owned or CC0 streams** carries
    none of the third-party risk and may suit the aesthetic better.
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
