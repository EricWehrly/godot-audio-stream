---
id: unit-test-suite
type: feature
epic: phase-trust
design: [docs/design/testing-strategy.md]
status: planned
---

# Unit Test Suite

**Goal:** Offline, deterministic tests for the logic that doesn't need a network at all —
plus a regression test for the one bug that cost 43% of the stream.

**Why:** There are currently zero automated tests. Several pieces of `RadioStream` are pure
functions of their input and could have been tested from day one; they weren't, because the
POC was chasing a working stream. Now that the shape is settled, they should be pinned before
`reconnect-resilience` starts adding state machine complexity on top.

## Scope

**Pure logic, no network:**
- **URL parsing** — host/port/path split, default port 80, `https://` rejected (until
  `tls-streams`), malformed input rejected cleanly rather than crashing. `test_load.gd`
  already asserts one case of this; it deserves a real table of cases.
- **HTTP response-header parsing** — the CRLFCRLF scan, status extraction, leftover-body
  handoff (an off-by-one here silently corrupts the first audio frames), the 64 KiB header
  cap, split-across-reads headers.
- **FIFO arithmetic** — `pop_frames` short reads, read-cursor compaction, and the
  trim-oldest-on-overflow path. Currently only exercised incidentally by a live burst.
- **Status transitions** — IDLE → CONNECTING → PLAYING → ERROR, and that `close()` on a
  never-opened stream is safe.

**Decoder regression tests** (need `fake-icecast-server` for byte generation):
- **The lookahead invariant.** Feed a stream in small increments and assert the skipped-byte
  counter stays at zero. This is the highest-value test in the suite: that bug discarded 43%
  of the stream, produced no error, and left every individual health signal looking fine.
- **Byte accounting closes** — `received == audio + skipped + backlog` — which is the
  property that exposed the bug in the first place.

## Test seams needed

Most of the above is `private` in C++ today. Options: expose the pure helpers as static
methods on the bound class (simple, slightly widens the public API), or add a
test-only build flag. Prefer the former unless the API surface becomes embarrassing —
`parse_url` in particular is plausibly useful to callers anyway.

## Non-goals

- Testing the audio graph. That's the demo's job and ultimately a human's.
- Chasing coverage numbers. The list above is chosen by risk, not by line count.

## Acceptance

- `test.sh`-equivalent runs green with **no network available**.
- Reintroducing the lookahead bug (setting `DECODE_LOOKAHEAD` to 0) makes a test fail loudly.
- Header-parsing tests cover the split-across-reads case, which a live fast connection rarely
  produces naturally.

## Links

up → `phase-trust` · needs → `fake-icecast-server` (byte generation) · design →
`testing-strategy.md`
