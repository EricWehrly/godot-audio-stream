# Performance: what can be measured here, and what needs surfer

Direct answer to "can the CPU worker stuff be tested in this repo, or do we need surfer?"

**Most of it is measurable here — and several parts are measurable *better* here.** Exactly
one class of question needs surfer, and surfer already owns the instrument for it.

The organising principle:

> **Build the instrument here. Take the verdict in surfer.**
> Isolation is what this repo is *for*. A number collected with nothing else running is a
> clean number. But "does this cause a hitch during real play" is definitionally a question
> about contention, and contention can't be faked convincingly.

## The split

| Question | Where | Why |
|---|---|---|
| Worker thread CPU per stream-minute | ✅ **here, better here** | Nothing else running to confound the measurement. In surfer this number is contaminated by everything else. |
| Cost of `pop_frames()` per call | ✅ here | Pure microbenchmark. Doesn't need a game. |
| Allocation churn per frame | ✅ here | Arithmetic plus a counter; see below. |
| Memory growth / leaks over hours | ✅ **here, better here** | A multi-hour soak is trivial headless and obnoxious inside a game. |
| Scaling with N concurrent streams | ✅ here | Purely a property of this library. |
| Wakeup frequency of the poll loop | ✅ here | Suspected fixed cost; see "What we already suspect". |
| Behaviour when the main thread is loaded | ◐ **approximable here** | A synthetic busy-load knob gets close, but it isn't real contention. |
| Frame-time p99 / p99.9 during play | ❌ **surfer** | Depends on core contention with `WorkerThreadPool` (city gen, traffic), cache and memory-bandwidth pressure, and real render load. |
| Does it trip surfer's perf gate | ❌ **surfer** | `perf-threshold-gate` is the instrument, and it's already built. |
| Audio underruns under real load | ❌ **surfer** | Needs the real audio thread competing with a real frame budget. |

## Why surfer is genuinely required for that last group

Not bureaucracy — three specific reasons an isolated harness cannot answer them:

1. **Core contention is a property of the whole program.** A decode thread that costs 0.5% of
   a core in isolation may still cause a hitch if it wakes on a core surfer's
   `WorkerThreadPool` is already saturating. Surfer runs real background work (city
   generation, traffic) that this repo has no way to reproduce honestly.
2. **Surfer's own priority is frame *consistency*, not average cost.** Its stated perf order
   is "frame consistency > avg framerate". That is a percentile question over a real
   workload — a mean measured in isolation cannot answer it.
3. **The instrument already exists there.** Surfer has `perf-threshold-gate` (per-metric
   drift gate with a `hitch_ms_total` bucket and per-origin attribution),
   `frame-time-overlay`, and `startup-profiling`. Integration perf work should **reuse those**
   rather than build a parallel harness — see `surfer-integration`.

There's also a documented trap: surfer's own perf captures are known to be noisy under
machine contention (`perf-gate-noise-robustness`), and this project has already been bitten
once by attributing a slowdown to a change when another session was competing for the
machine. Any surfer-side number for this library needs an uncontended capture.

## What we already suspect (to be confirmed, not assumed)

Three hypotheses worth stating up front so the harness is built to test them:

**1. The decode itself is probably cheap.** minimp3 decodes far faster than real time, and the
330s soak sustained 104% of real-time output while idle. But *throughput headroom is not
cost* — "it keeps up" and "it costs 0.4% of a core" are different claims, and only the second
one matters for a frame budget. **Unmeasured.**

**2. The polling loop may cost more than the decoding.** The worker sleeps 5ms whenever no
bytes are available, so it wakes ~200×/second regardless of bitrate — a *fixed* cost
independent of how much audio is flowing, and on Windows the actual sleep granularity may be
coarser than requested (~15.6ms default timer resolution), making the real wakeup rate
something we're guessing at. This is also a battery concern on laptops. If measurement shows
wakeups dominating, the fix is a blocking read or an adaptive sleep, not faster decoding.

**3. Main-thread marshaling is small but strictly per-frame.** At 60fps we pop 44100/60 ≈ 735
frames per call, allocating a fresh `PackedVector2Array` of 735 × 8 bytes ≈ **5.9 KB/frame,
≈353 KB/s**. That's not alarming in absolute terms, but it is a guaranteed per-frame
allocation, which is exactly the shape of thing that shows up in frame-time *consistency*
rather than average. This is the number that would trigger D2's migration contingency.

## Instrumentation to add

Small additions to `RadioStream`, all cheap enough to leave compiled in:

| Metric | How |
|---|---|
| `get_worker_cpu_ms()` | True per-thread CPU time — `GetThreadTimes` on Windows, `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` on POSIX. Distinguishes real work from sleeping. |
| `get_worker_busy_ms()` | Wall time spent inside read+decode. Compared against elapsed, gives a busy fraction. |
| `get_worker_wakeups()` | Loop iteration count. Together with the two above, separates "decode is expensive" from "we wake too often" — hypotheses 1 and 2. |
| `get_decode_ms()` / `get_socket_ms()` | Splits the worker's own time between the two candidates. |

On the GDScript side the harness measures `pop_frames()` call cost and allocation volume
directly.

## Proposed budgets

Targets to measure against, so results are pass/fail rather than a shrug. Surfer runs a
16.67ms frame at 60fps:

| Metric | Budget | Rationale |
|---|---|---|
| Worker CPU, sustained | **< 1% of one core** | It's decoding 128 kbps. Anything near 5% means something is wrong, most likely wakeups. |
| `pop_frames()` main-thread cost | **< 0.05 ms/frame** | ~0.3% of the frame budget. Above this, D2's contingency is on the table. |
| Steady-state memory growth | **0 over 1 hour** | The FIFO is bounded by construction; any growth is a leak. |
| Starvations under synthetic load | **0** | Same criterion the existing soak uses. |

These are proposals, not measurements. Revise them once real numbers exist rather than
treating them as having been validated.

## Dependency

**`perf-harness` should not start before `fake-icecast-server`.** Measuring against a live
third-party station means every number carries that station's jitter, its burst-on-connect,
and the local network. A local server that emits a known byte pattern at an exactly
controlled rate makes perf numbers repeatable and lets us test deliberately hostile
conditions (starved feed, bursty feed, mid-stream stall) that a real station won't produce
on demand.
