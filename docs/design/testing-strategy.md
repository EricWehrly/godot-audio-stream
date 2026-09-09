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

## Fixture audio — settled (D6)

Offline decode tests need MP3 bytes. Committing audio is fine *in this repo* (surfer's rule
is that project's, and is about licensing rather than file type), and generating our own
sidesteps licensing entirely.

**Commit both the generator script and its rendered `.mp3`.** The script keeps the fixture
reproducible and auditable; the committed output means no machine — including CI — needs an
encoder to run tests.

Two deliberate choices:

- **A real signal, not silence.** Silence decodes fine while hiding exactly the corruption
  that matters — the lookahead bug produced *audio*, just less of it. A few seconds of
  oscillators is also something a human debugging by ear can actually judge.
- **ffmpeg/libmp3lame** does the encoding, as a dev-time tool. It never ships and isn't
  linked, so its licensing doesn't touch the permissive-only rule; the rendered output is our
  own content and freely redistributable.

## What stays network-dependent

A small set of tests should still hit a real station, because a synthetic server can only
prove we handle the protocol *as we understand it*:

- real Icecast handshake and header shape
- burst-on-connect behaviour
- long-run stability against a real feed (the existing 330s soak)

These belong in a separate, explicitly-tagged group that CI may skip — never mixed into the
offline suite, so a network outage can't be mistaken for a code regression.
