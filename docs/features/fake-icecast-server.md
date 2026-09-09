---
id: fake-icecast-server
type: feature
epic: phase-trust
design: [docs/design/testing-strategy.md, docs/design/performance.md]
status: planned
---

# Fake Icecast Server

**Goal:** A local test server that speaks enough of the Icecast/Shoutcast protocol to stand
in for a real station — serving synthetic MP3 at a controlled rate, with fault injection on
demand.

**Why:** This is the keystone of the roadmap. Every test today needs the open internet and a
third party's uptime, and every perf number today carries that station's jitter and
burst-on-connect. One piece of work fixes both. It also sidesteps the fixture-licensing
question entirely (Q1) by generating its own audio.

## Scope

- **Synthetic MP3 generation.** Encode a known signal (tone, sweep, or silence) to MP3 frames
  at a chosen bitrate/sample rate. Generated from source, never committed as an audio file —
  so the bytes are reproducible and nothing copyrighted goes near the repo. Resolves Q1.
- **A controlled feed.** Serve at an exact byte rate, so a test can assert real-time behaviour
  without network variance. Configurable burst-on-connect to reproduce the real Icecast
  behaviour the FIFO trim logic exists to handle.
- **Icecast-shaped handshake.** `HTTP/1.0 200 OK`, `Content-Type: audio/mpeg`, `icy-*`
  headers, `Connection: close`. Enough to exercise our real parser.
- **Fault injection**, each independently switchable:
  - disconnect mid-stream (drives `reconnect-resilience`)
  - stall the feed without disconnecting (drives starvation handling)
  - truncated final frame / garbage bytes injected mid-stream (decoder resync)
  - HTTP error statuses (404, 503) and a redirect (drives `url-resolution`)
  - slow-loris headers, to exercise the handshake timeout
  - **optional ICY metadata interleaving**, so `icy-metadata` can be built and tested
    without needing a real station that happens to send titles

## Implementation notes

Language is open. A small Python script is the least-friction option (Python is already a
build dependency via SCons) and keeps the server out of the Godot process entirely, which
makes "kill the server mid-test" trivial. A GDScript `TCPServer` alternative would keep
everything in-engine but makes hard-kill faults awkward.

Whichever is chosen, tests must be able to start it, point `RadioStream` at
`http://127.0.0.1:<port>/...`, and tear it down deterministically.

## Non-goals

- Being a real streaming server. It serves one synthetic stream to a test.
- Ogg/Opus output — until `opus-codec` needs it.
- TLS — `tls-streams` can decide whether it needs a certificate story or tests against a real
  host.

## Acceptance

- A test can start the server, stream from it, and assert exact decoded frame counts —
  offline, with the same result every run.
- Each fault above can be triggered on demand and produces the documented `RadioStream`
  behaviour rather than a hang or crash.
- Burst-on-connect is reproducible, so the FIFO trim path is exercised by a test rather than
  only by a live station.
- No audio file is committed to the repo.

## Links

up → `phase-trust` · unblocks → `unit-test-suite`, `reconnect-resilience`, `perf-harness`,
`icy-metadata` · design → `testing-strategy.md`, `performance.md`
