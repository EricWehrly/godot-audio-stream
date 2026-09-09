---
id: tls-streams
type: feature
epic: phase-stations
status: done
---

# TLS (`https://`) Streams

**Goal:** Connect to stations served over `https://`.

**Why:** `open()` currently rejects `https://` outright. That's an honest POC limitation, but
browsers and platforms have been pushing everything to secure contexts for years and a
growing share of stations are https-only. Without this, a large and increasing fraction of
real station URLs simply don't work.

## Scope

- Wrap the connected `StreamPeerTCP` in a `StreamPeerTLS` via `connect_to_stream()`, using
  the hostname for certificate validation.
- Default port 443 when the scheme is `https` and no port is given.
- Both peers need polling; the worker loop must poll the TLS layer, and read/`get_available_bytes`
  go through it rather than the raw socket. Abstract the peer behind a small interface so the
  read loop doesn't grow a branch per call site.
- Surface TLS handshake failures as a **terminal** error (bad certificate is not retryable),
  distinct from the retryable network failures `reconnect-resilience` handles.
- Godot ships a bundled certificate store; confirm it's used by default and decide whether to
  expose a custom `X509Certificate` option (probably not, for D1 scope).

## Non-goals

- Certificate pinning.
- An option to skip verification. Tempting for debugging, but a footgun that tends to ship.
  If it's ever needed, it should be loud and clearly test-only.

## Acceptance

- A known https station streams with the same stability as http (a short soak, 0 starvations).
- An invalid certificate fails fast with a clear message and does **not** enter a retry loop.
- `http://` behaviour is completely unchanged — this must not regress the working path.

## Built 2026-09-08

Reused Godot's own mechanism — the docs confirm `HTTPClient`/`HTTPRequest` support https via
this same wrapper: `StreamPeerTLS::connect_to_stream(stream, common_name)` wraps the already-
connected `StreamPeerTCP`, validating the certificate against `common_name` (the host).

**Shape:** `parse_url` now accepts `https://`, sets `Url::is_tls`, defaults to port 443.
`worker_main` connects TCP as before, then — if `is_tls` — wraps it in a `StreamPeerTLS` and
polls *both* layers each tick until `STATUS_CONNECTED` (TCP pumps the raw socket, TLS pumps
its own handshake/record layer on top; deliberately conservative rather than assuming TLS's
`poll()` alone suffices, since the docs don't spell out the exact contract). From there,
`http_handshake` and the main read loop both go through a `Ref<StreamPeer> data_peer` — either
the raw TCP peer or the TLS wrapper — since `get_partial_data`/`put_data`/`get_available_bytes`
all live on the shared `StreamPeer` base; only the poll+status-check needed a small
`poll_connection(tcp, tls)` helper, since `StreamPeerTCP::Status` and `StreamPeerTLS::Status`
are different enums with no shared base.

**`STATUS_ERROR_HOSTNAME_MISMATCH` is handled as its own case** (not folded into the generic
TLS error), with a message naming it explicitly — this is the certificate-mismatch case this
doc's own Acceptance section calls out as non-retryable, distinct from a transient network
failure. `reconnect-resilience`, once built, should treat it as terminal.

**Verified against the exact real-world failure this was built to fix**, not a synthetic
case: a station a human found live-testing the Station Tuner (`https://stream.synthwaveradio.
eu/listen/synthwaveradio.eu/radio.mp3`) that previously failed with "https is not supported."
- 25s and 90s soaks, both clean: byte accounting closes exactly (e.g.
  `1492114 recv = 1487934 audio + 418 skipped + 3762 backlog`), 0 starvations, FIFO stable
  ~3.8–4.0s, `skip` frozen at 418 (one-time) the entire run.
- The 90s run rode out a real network dip (90.3 kbps against a ~130 kbps steady state at
  t=30s) without a single starvation — the FIFO did what it's for.
- A nonexistent https host fails fast and cleanly (`connect_to_host failed`) — no hang.
- Plain `http://` behaviour confirmed unchanged: the offline suite
  (`fake-icecast-server`-backed) still passes 3/3 untouched, and the extension still loads in
  both Godot 4.6 and 4.7.

## Resolved: the surfer integration failure (2026-09-08)

TLS worked reliably in this repo's own demo but failed deterministically, 3/3, inside surfer's
actual project:

```
ERROR: SSL module failed to initialize!
   at: init_client (modules/mbedtls/tls_context_mbedtls.cpp:208)
   at: connect_to_stream (modules/mbedtls/stream_peer_mbedtls.cpp:104)
```

**What it actually was — narrowed by direct experiment, not guessed at:**

- A web search surfaced [godotengine/godot#106167](https://github.com/godotengine/godot/pull/106167)
  ("mbedTLS: Fix concurrency issues with TLS"), a real, documented Godot engine bug about
  mbedTLS 3's global PSA-crypto state racing across threads. Promising, but a red herring here
  — it merged in May 2025, well before this project's 4.7, so it was already fixed in our
  build. The web search **did** surface the actually-relevant fact: `WebFetch`ing Godot's own
  `tls_context_mbedtls.cpp` source showed this exact error fires when
  `CryptoMbedTLS::get_default_certificates()` returns null — a certificate-bundle-loading
  failure, not a concurrency bug.
- **First real experiment**: a main-thread `HTTPRequest` to any https URL, completed *before*
  `RadioStream.open()` on an https station, fixed it completely. Looked like a
  main-thread-vs-worker-thread story.
- **That story turned out wrong.** Moving a warm-up handshake onto `RadioStream`'s own worker
  thread (still a background thread, just running *before* the real connection) did **not**
  fix it — same failure, twice over. So it wasn't really about which thread.
- **The actual variable, isolated by moving where `open()` gets called**: a bare
  `-s script.gd` probe calling `open()` synchronously inside `_init()` — the earliest possible
  moment, before Godot's engine has processed a single frame — fails. The identical code,
  deferred to the first `_process()` tick, succeeds. **This is a timing-relative-to-engine-
  startup issue, not a threading issue.** The earlier "background thread doesn't help" result
  was confounded: that test *also* called `open()` from `_init()`, so it never left the
  too-early window either way.

**The fix**: `warm_up_tls_once()` — a throwaway TLS handshake attempt, guarded by
`std::call_once` so it runs at most once per process, called synchronously at the top of
`open()` (before the worker thread is spawned), for any `https://` URL. It doesn't need to
*succeed* to work: in the most hostile case tested (`open()` called from `_init()`, matching
the original failing probes), the warm-up attempt itself **still logs the same SSL error** —
but the real connection immediately after it succeeds anyway. The act of attempting a TLS
handshake once is what matters, not its outcome. **Mechanism not fully understood beyond
that** — plausibly some Godot-internal lazy state finishes initializing as a side effect of
any attempt, success or failure, but that's inference, not confirmed from engine source.

**One visible side effect worth knowing, not a bug**: on that earliest-possible-call path, the
console still prints the scary-looking `SSL module failed to initialize!` line from the
warm-up's own failed attempt, even on a run that ultimately works perfectly. Don't chase it as
a regression if you see it in a log — check whether the *stream* actually played.

**Verified fixed, in the worst case, not just the easy one:**
- `open()` called synchronously in `_init()` (the original failing shape) against the real
  station that started this investigation: warm-up fails visibly, real connection succeeds,
  clean 30s soak — byte accounting closes (`530390 recv = 525792 audio + 836 skipped + 3762
  backlog`), 0 starvations.
- The real Station Tuner UI, under real timing (search round-trip, then a simulated Play
  click several seconds into a run): clean, 100k+ frames decoded, no errors.
- Plain `http://` behaviour and both engine versions reconfirmed unaffected after the fix.

**Consequence:** surfer's `station-tuner` https gate (`Station.unsupported_reason()`) can now
be safely removed — see that project's own history for the corresponding change.

## Not done this pass

- **Certificate pinning** — explicit non-goal, unchanged.
- **A verification-skip escape hatch** — explicit non-goal, unchanged (a footgun that tends
  to ship).
- **Offline TLS testing against `fake-icecast-server`** — still needs a self-signed cert and
  a way to trust it without reintroducing the skip-verification footgun. The offline suite
  stays on plain http; TLS is validated against real stations only, which is real coverage
  but not CI-safe/deterministic. Worth revisiting once `fake-icecast-server` grows more.

## Links

up → `phase-stations` · relates → `reconnect-resilience` (error classification),
`url-resolution` (redirects are commonly http→https)
