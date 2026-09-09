---
id: perf-harness
type: feature
epic: phase-measure
design: [docs/design/performance.md]
status: planned
---

# Performance Harness

**Goal:** Know what this library actually costs — worker CPU, main-thread cost, and memory —
with numbers instead of intuition.

**Why:** Everything currently known about cost is inference. The 330s soak showed the decoder
*keeping up*, which is throughput, not cost. "It keeps up" and "it costs 0.4% of a core" are
different claims, and only the second matters against a frame budget.

The full analysis of what's measurable here versus what needs surfer lives in
[`design/performance.md`](../design/performance.md). Short version: **most of it is
measurable here, and several parts are measurable better here** — isolation is this repo's
advantage. Only frame-time percentiles under real contention need surfer.

## Scope

**Instrumentation** (added to `RadioStream`, cheap enough to leave compiled in):

| Metric | How |
|---|---|
| `get_worker_cpu_ms()` | True per-thread CPU time: `GetThreadTimes` (Windows), `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` (POSIX). Separates real work from sleeping. |
| `get_worker_busy_ms()` | Wall time inside read+decode; against elapsed, gives a busy fraction. |
| `get_worker_wakeups()` | Loop iterations — tests hypothesis 2 below. |
| `get_decode_ms()` / `get_socket_ms()` | Splits worker time between the two candidates. |

**Harness** (`demo/test_perf.gd`, headless, fed by `fake-icecast-server`):
- Worker CPU per stream-minute.
- `pop_frames()` cost per call and allocation volume per frame.
- Memory/RSS across a long run (target: 1 hour, flat).
- Scaling with N concurrent streams.
- A **synthetic main-thread load knob** (busy-spin) to approximate contention — explicitly an
  approximation, not a substitute for surfer.

## The three hypotheses to confirm or kill

1. **Decode is cheap.** minimp3 runs far faster than real time. Expected well under 1% of a
   core — but unmeasured.
2. **The poll loop may cost more than the decode.** The worker sleeps 5ms whenever no bytes
   are available: ~200 wakeups/second regardless of bitrate, a *fixed* cost independent of
   load, and on Windows the real sleep granularity may be coarser than requested (~15.6ms
   default timer resolution) — so the actual rate is currently a guess. Also a laptop battery
   concern. If wakeups dominate, the fix is a blocking read or adaptive sleep, **not** faster
   decoding.
3. **Main-thread marshaling is small but strictly per-frame.** At 60fps: 735 frames × 8 bytes
   ≈ **5.9 KB/frame, ≈353 KB/s** of guaranteed per-frame allocation. Not alarming in absolute
   terms, but exactly the shape that shows up in frame-time *consistency* rather than average.
   This is the number that would trigger D2's migration contingency.

## Budgets

Proposals to measure against, so results are pass/fail rather than a shrug. Revise once real
numbers exist — do not treat as validated.

| Metric | Budget |
|---|---|
| Worker CPU, sustained | < 1% of one core |
| `pop_frames()` main-thread cost | < 0.05 ms/frame (~0.3% of a 16.67ms frame) |
| Memory growth over 1 hour | 0 |
| Starvations under synthetic load | 0 |

## Non-goals

- Optimising anything. This feature **measures**; fixes are separate work justified by its
  output.
- Frame-time percentiles under real game load — that's `surfer-integration`, using surfer's
  own already-built instruments.

## Acceptance

- Every metric above is collected against `fake-icecast-server`, repeatably.
- Each of the three hypotheses is confirmed or killed with a number.
- Results recorded **in this doc** so later work argues against measurements, not memories.

## Dependency

**Do not start before `fake-icecast-server`.** Measuring against a live station means every
number carries that station's jitter and burst behaviour, and hostile conditions (starved
feed, bursty feed, stall) can't be produced on demand.

## Links

up → `phase-measure` · needs → `fake-icecast-server` · feeds → `audiostreamplayback-migration`
(D2 trigger), `surfer-integration` · design → `performance.md`
