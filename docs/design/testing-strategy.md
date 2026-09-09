# Testing Strategy

## Where we are

Two headless harnesses exist (`test_load.gd`, `test_stream.gd`) and both **require the open
internet and a third party's uptime**. There are zero offline tests. That's acceptable for a
POC and is the first thing `phase-trust` fixes.

The soak harness did prove one idea worth keeping: it drains the FIFO at exactly the stream's
sample rate to simulate a real-time consumer, which makes starvation measurable **without an
audio device**. The core property is therefore CI-able rather than something only a human
with speakers can check.

## Three tiers

Adapted from surfer's own three-tier model, which exists because machine-green was never
enough on its own there.

### 1. Machine — offline, deterministic, every change

Must run with no network and give the same answer every time.

- **Pure logic** (`unit-test-suite`): URL parsing, HTTP response-header parsing, FIFO
  trim/compaction arithmetic, status transitions. All currently untested and all trivially
  testable — they just need to be reachable from GDScript or exercised through a test seam.
- **Decode against a fixture**: feed known bytes, assert frame count and sample rate. Also
  the natural home for a **regression test on the lookahead invariant** — decode a buffer fed
  in small increments and assert the skip counter stays at zero. That bug cost 43% of the
  stream and produced no error; it deserves a test that fails loudly if reintroduced.
- **Fault injection** (`fake-icecast-server`): mid-stream disconnect, stalled feed, truncated
  frame, garbage bytes, HTTP error status, slow-loris headers.

### 2. Sanity check — the agent looks before handing over

Run the demo, read the counters, confirm nothing is obviously broken. An agent may report
anything it dependably observes (starvation counts, byte accounting, status) but **must never
certify that the audio sounds good** — that's tier 3, always.

### 3. Human review — does it sound right

The one thing no counter answers. A starvation count of 0 says the buffer stayed fed; it says
nothing about audible glitches, level, or whether a reconnect is smooth or jarring.

Human checkpoints belong in each feature doc, and today there is one standing open: **nobody
has listened to this yet.**

## The keystone: a local stream server

`fake-icecast-server` is the single highest-leverage item in the roadmap because it converts
most of tier 1 from impossible to routine. It buys:

- **Determinism** — the same bytes every run, so a failure means a real regression.
- **Offline CI** — no dependency on somebody else's server staying up.
- **Fault injection on demand** — real stations won't drop mid-stream when you ask.
- **Honest perf numbers** — a controlled feed rate removes network jitter from every
  measurement (see `design/performance.md`).
- **No licensing question** — synthetic audio, nothing copyrighted anywhere near the repo.

## Open: fixture audio (Q1)

Offline decode tests need MP3 bytes from somewhere. The ecosystem rule against committing
audio is about licensing, and the cleanest way to honor it is to sidestep the question
entirely: **generate MP3 frames programmatically at test time** — a tone or silence — so
nothing copyrighted is ever committed and the fixture is reproducible from source.

Resolved in `fake-icecast-server`.

## What stays network-dependent

A small set of tests should still hit a real station, because a synthetic server can only
prove we handle the protocol *as we understand it*:

- real Icecast handshake and header shape
- burst-on-connect behaviour
- long-run stability against a real feed (the existing 330s soak)

These belong in a separate, explicitly-tagged group that CI may skip — never mixed into the
offline suite, so a network outage can't be mistaken for a code regression.
