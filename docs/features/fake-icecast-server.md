---
id: fake-icecast-server
type: feature
epic: phase-trust
design: [docs/design/testing-strategy.md, docs/design/performance.md]
status: in-progress
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

- **Synthetic MP3 fixture** (**D6**). A short oscillator piece — deliberately a real signal
  rather than silence, since silence decodes fine while hiding exactly the corruption we care
  about — rendered to MP3 via ffmpeg/libmp3lame. Both the generator script and its `.mp3`
  output are committed: reproducible *and* no encoder needed at test time. Content is ours,
  so redistribution is unencumbered.
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

## Implementation: script vs. Docker

Docker was proposed for simplicity. Worth examining, because **real Icecast in Docker is
probably not the simpler option here** — for a reason that isn't obvious until you try it:

> **Icecast doesn't generate audio.** It's a *relay*. To serve a stream you also need a source
> client (`ices2`, `ezstream`, `liquidsoap`, …) plus audio to feed it. "Icecast in Docker" is
> really three moving parts and a compose file, not one container.

And the thing we most need — fault injection — is what real Icecast is *worst* at. There's no
way to tell it "stall this client now" or "drop this connection after 10 seconds". Killing the
container is crude, slow, and can only produce one of the faults on our list.

**Recommendation — a small script as the primary tool:**

- Python, no dependencies (already a build dependency via SCons), running outside the Godot
  process so hard-kill faults are trivial.
- Every fault above becomes a flag: `--drop-after 10s`, `--stall-at 30s`, `--burst 512k`.
- Starts in milliseconds; tests can spin one per case.
- MP3 bytes generated with **ffmpeg** (`libmp3lame`), confirmed present on this machine — see
  Q1's resolution for what gets committed.

**Docker is still worth having, as an optional conformance tier.** If we want offline
verification against the genuine article, the compose file is `icecast` + a source client +
[**toxiproxy**](https://github.com/Shopify/toxiproxy) (MIT) in front. Toxiproxy is the right
tool for the fault half — latency, bandwidth throttling, connection drops, slow-close, all
controllable over an API at the TCP layer, which is exactly our fault list and is
*better*-controlled than a script could manage at that level.

But note the marginal value is smaller than it looks: **we already get real-Icecast fidelity
for free** from the network-tagged tests against a live station. Docker's genuine additions
are offline-ness and reproducible TCP-level faults.

Docker is confirmed available on this machine, so this stays cheap to add later. Start with
the script; add the compose tier if the script's fidelity proves insufficient.

Whichever is used, tests must start it, point `RadioStream` at `http://127.0.0.1:<port>/...`,
and tear it down deterministically.

## Non-goals

- Being a real streaming server. It serves one synthetic stream to a test.
- Ogg/Opus output — until `opus-codec` needs it.
- TLS — `tls-streams` can decide whether it needs a certificate story or tests against a real
  host.

## Built 2026-09-08

`tools/fake_icecast/` — `server.py` (stdlib only, `http.server.ThreadingHTTPServer`) +
`generate_fixture.py` (committed alongside its output, per **D6**) + a `README.md` with the
flag reference. All eight faults from the scope above are implemented as independent flags.

Spot-checked against the **real** `RadioStream`, not just curl:

- **25s soak, byte accounting closed exactly**:
  `536300 recv = 532317 audio + 0 skipped + 3983 backlog`, 0 starvations, FIFO stable ~3s,
  1 buffer trim (0 after settling) — the burst-on-connect path fires correctly against a
  synthetic server, matching the live-station soak's behaviour.
- **`--drop-after`** against a live `RadioStream` reaches `STATUS_ERROR` /
  `"Stream disconnected"` — confirmed as the exact baseline `reconnect-resilience` needs to
  fix, not just that the server closes a socket.
- `--http-error` and `--redirect` return the correct status/header.

**A real bug found along the way, not yet root-caused:** `ThreadingHTTPServer` lives in
`http.server`, not `socketserver` (an easy mixup — `BaseHTTPRequestHandler` is in
`http.server` but most `Threading*` mixins are in `socketserver`). Also: the fixture's ID3v2.4
tag (ffmpeg adds one by default) reappears every loop since the server just cycles the raw
fixture bytes — the 25s soak shows `skip=0` throughout despite this, so it's evidently being
absorbed cleanly, but *why* wasn't traced (candidates: minimp3 detects `ID3` bytes anywhere,
not just at stream start; or the interruption lands in the `frame_bytes==0`/needs-more-data
path, which isn't counted by our skip instrumentation at all — a possible gap in that
counter, not necessarily a decoder gap). Worth a closer look whenever `unit-test-suite`
formalizes decode-regression tests against this server.

## Acceptance

- ✅ A test can start the server, stream from it, and assert exact decoded frame counts —
  demonstrated via a manual soak; not yet wired into an automated `test.sh`-run suite (that's
  `unit-test-suite`'s job).
- ✅ Each implemented fault triggers on demand and produces the documented `RadioStream`
  behaviour rather than a hang or crash — spot-checked for drop/http-error/redirect; the
  remaining faults (stall, truncate, garbage, slow-headers, icy-metadata) are implemented but
  only exercised via manual curl, not yet against a live `RadioStream`.
- ✅ Burst-on-connect is reproducible and exercises the FIFO trim path.
- ✅ The committed fixture is regenerable from its committed script; no third-party audio
  enters the repo.
- ⚑ Not yet done: wiring this into `unit-test-suite`'s actual `test.sh` run (spawning the
  server as a subprocess from a GDScript test and tearing it down deterministically).

## Links

up → `phase-trust` · unblocks → `unit-test-suite`, `reconnect-resilience`, `perf-harness`,
`icy-metadata` · design → `testing-strategy.md`, `performance.md`
