---
id: tls-streams
type: feature
epic: phase-stations
status: in-progress
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

## Open issue found integrating into surfer (2026-09-08) — NOT resolved

TLS works reliably in **this repo's own demo** (25s + 90s soaks above) but **fails
deterministically, 3/3, inside surfer's actual project**:

```
ERROR: SSL module failed to initialize!
   at: init_client (modules/mbedtls/tls_context_mbedtls.cpp:208)
   at: connect_to_stream (modules/mbedtls/stream_peer_mbedtls.cpp:104)
```

Isolated, not guessed at:
- Retried 3x in surfer, deterministic every time — not contention (the demo project, tested
  moments later on the identical machine state, succeeded immediately).
- `HTTPRequest`-based https (Radio Browser's own search, which also goes through
  `StreamPeerTLS` internally) **works fine** in surfer, at the same time. So TLS itself isn't
  broken project-wide.
- No `network/tls/*` project setting exists in either project — checked, not assumed.

**The one confirmed structural difference**: `HTTPRequest`'s TLS init happens on Godot's main
thread. `RadioStream`'s happens on its own worker `std::thread` — and that's the case that
fails, only inside surfer's larger project (more autoloads, more concurrent engine activity),
never in the minimal demo. Leading hypothesis, **unconfirmed**: a thread-safety issue in
Godot's own mbedtls wrapper that a quiet demo project never has enough concurrent activity to
trigger. Not chased further this pass — would need reading Godot engine source
(`tls_context_mbedtls.cpp`) or a minimal repro project graduated in complexity between "the
demo" and "all of surfer" to actually localize.

**Consequence:** the surfer-side UI change un-gating https results was reverted before
committing — shipping "https now works" into the one place that's actually supposed to
consume it, while it demonstrably doesn't work there, would be worse than the gate it was
replacing. `station-tuner`'s https gate stays in place until this is root-caused.

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
