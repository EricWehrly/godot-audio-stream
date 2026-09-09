---
id: reconnect-resilience
type: feature
epic: phase-trust
design: [docs/design/architecture.md]
status: planned
---

# Reconnect & Resilience

**Goal:** Survive the network being the network. A dropped stream should recover on its own
instead of ending the session, and callers should be told what happened rather than polling
to find out.

**Why:** Today any interruption is terminal — the worker sets `STATUS_ERROR` and exits. For a
game meant to run for a long cruise, one transient blip currently kills the music until
something manually reopens the stream. This is the single biggest gap between "the POC works"
and "we can rely on it".

## Scope

**Reconnect:**
- Detect disconnect, socket error, and **stall** (connected but no bytes for N seconds — a
  distinct and more insidious failure than a clean drop).
- Reconnect with exponential backoff and jitter, capped, with a configurable maximum attempt
  count (`0` = forever, the likely default for a radio).
- Reset decoder state cleanly on reconnect: `mp3dec_init`, drop stale input bytes, **keep**
  the PCM FIFO so already-decoded audio keeps playing during the gap. Done right, a short
  blip is inaudible because the buffer covers it — that's what the ~3s FIFO is *for*.
- Distinguish retryable failures (connection reset, timeout) from terminal ones (404, 401,
  malformed URL). Retrying a 404 forever is a bug, not resilience.

**Status signals** (replacing polling):
- `connected`, `disconnected(reason)`, `reconnecting(attempt)`, `stream_failed(reason)`,
  `format_changed(hz, channels)`.
- Emitted on the **main thread**, not from the worker — signals must be marshaled across the
  thread boundary, most simply via a `call_deferred` hop. Emitting a Godot signal directly
  from the worker is the obvious mistake to avoid here.
- Polling getters stay for the demo/harness; signals are additive.

**Format changes:** a station can change sample rate or channel count mid-stream. Today that
silently updates `sample_rate` while the consumer's `AudioStreamGenerator` keeps its original
`mix_rate` — the audio would play at the wrong speed. At minimum, detect and signal it.

## Non-goals

- Failover to a *different* station URL. That's a policy decision for the consuming game.
- Buffering across a reconnect to make it seamless in the gapless sense. Covering a blip with
  the existing FIFO is in scope; seamless splicing is not.

## Acceptance

- `fake-icecast-server` drops the connection mid-stream → playback resumes automatically,
  and if the outage is shorter than the FIFO depth, **no starvation is recorded**.
- A stalled feed (connected, no bytes) is detected and triggers reconnect.
- A 404 fails fast without retrying; a connection reset retries with visible backoff.
- Backoff is bounded — a server that's down for an hour doesn't produce an unbounded retry
  storm or busy-loop.
- Signals fire on the main thread; connecting a `Callable` that touches the scene tree is
  safe.
- ⚑ Human checkpoint: does a real reconnect *sound* acceptable, or is there an audible seam?

## Links

up → `phase-trust` · needs → `fake-icecast-server` (fault injection) · design →
`architecture.md`
