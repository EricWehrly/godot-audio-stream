---
id: tls-streams
type: feature
epic: phase-stations
status: planned
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

## Open

Testing TLS against `fake-icecast-server` needs a self-signed cert and therefore a way to
trust it, which reintroduces the verification-skip footgun. Likely resolution: test the TLS
path against a real station (tagged network-dependent), and keep the offline suite on plain
http. Decide when building.

## Links

up → `phase-stations` · relates → `reconnect-resilience` (error classification),
`url-resolution` (redirects are commonly http→https)
