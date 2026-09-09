---
id: audiostreamplayback-migration
type: feature
epic: phase-reach
design: [docs/design/decisions.md, docs/design/performance.md]
status: contingency
---

# AudioStreamPlayback Migration (Contingency)

> ⏸ **Do not start this without a measured trigger.** This documents an alternative
> architecture and the conditions that would justify it. Per **D2**, the current poll-based
> boundary stays until numbers say otherwise.

**The alternative:** replace the `RefCounted` + `pop_frames()` design with a native
`AudioStreamPlayback` subclass, so the engine pulls PCM directly from C++ and `RadioStream`
becomes an `AudioStream` usable by an ordinary `AudioStreamPlayer`.

## What it would buy

- **No per-frame marshaling.** Today every frame allocates a `PackedVector2Array` of ~735
  frames (≈5.9 KB/frame, ≈353 KB/s at 60fps) and crosses the binding.
- **Correct thread model by construction.** The engine's audio thread pulls when it needs
  samples, instead of a `_process` loop pushing on a best-effort basis. Removes an entire
  class of "the main thread was busy so audio starved" failure.
- **It behaves like a normal Godot audio resource** — buses, effects, `AudioStreamPlayer3D`,
  all for free, with no bespoke plumbing in the consuming game.
- Frame-rate independence: audio stops depending on `_process` being called on time.

## What it costs

- Substantially more godot-cpp surface (`AudioStream` + `AudioStreamPlayback`, virtual method
  binding, the engine's mixing contract).
- **Failures move inside compiled code.** The current design's real advantage is that the
  audio plumbing is visible, editable GDScript — every bug found during the POC was diagnosed
  by adding a counter and re-running, and that loop gets slower here.
- The mix callback runs on the audio thread with hard real-time expectations: **no locks, no
  allocation, no blocking** in that path. Today's `std::mutex` FIFO would need to become a
  genuine lock-free ring buffer, which is real work and easy to get subtly wrong.

## Trigger conditions

Any **one** of these justifies revisiting. All are things `perf-harness` measures:

1. **`pop_frames()` exceeds ~0.05 ms/frame** (~0.3% of a 16.67ms frame budget).
2. **Per-frame allocation shows up in surfer's frame-time consistency** — the p99 metric its
   perf gate already tracks, not the mean.
3. **Underruns occur in-game that don't occur in isolation**, indicating the `_process`-driven
   push is losing to frame-budget pressure. This is the most likely trigger and the most
   compelling, because it's the failure the migration structurally eliminates.
4. **A consumer needs real bus/effect integration** that the generator path can't provide.

## Why the risk of waiting is acceptable

The C++ core — socket handling, incremental decode, FIFO — is **independent of how audio
leaves it**. A migration replaces the output surface and leaves the guts intact. So features
built in the meantime (reconnect, metadata, TLS, codecs) are not wasted work and would carry
over essentially unchanged.

That's the specific reason D2 judged deferring this to be cheap rather than a gamble.

## If triggered

Do it as a *parallel* implementation behind a flag, not a rewrite in place — keep the working
poll path until the native one matches it on the existing soak criteria. The harnesses already
exist to compare them on identical terms.

## Links

up → `phase-reach` · decision → D2 · triggered by → `perf-harness`, `surfer-integration` ·
design → `architecture.md`, `performance.md`
