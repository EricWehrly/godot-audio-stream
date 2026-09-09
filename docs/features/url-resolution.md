---
id: url-resolution
type: feature
epic: phase-stations
status: planned
---

# URL Resolution — Redirects and Playlists

**Goal:** Accept the URLs stations actually publish, not just the direct stream endpoint
they resolve to.

**Why:** `open()` currently requires the final `http://host/mount` endpoint. That is *not*
what stations hand out. Directories and station websites publish `.pls` or `.m3u` playlist
files, and load balancers redirect. A user pasting a station URL from anywhere will, more
often than not, hand us something we currently reject — and the failure looks like "the
library is broken" rather than "that's a playlist".

## Scope

**HTTP redirects:**
- Follow `301`, `302`, `303`, `307`, `308` via the `Location` header.
- Cap the redirect chain (5 is conventional) and detect loops.
- Handle relative `Location` values, and cross-scheme redirects (http → https, which needs
  `tls-streams`).

**Playlist files:**
- Detect by `Content-Type` (`audio/x-scpls`, `audio/x-mpegurl`, `application/pls+xml`) and by
  extension as a fallback, since servers are unreliable about content types here.
- **`.pls`** — INI-shaped: parse `File1=`, `File2=`… in order.
- **`.m3u` / `.m3u8`** — line-based: non-comment lines are URLs. Note `.m3u8` may indicate
  **HLS**, which is explicitly out of scope (D5); detect and reject it with a clear message
  rather than trying to stream a segment manifest as audio.
- Try entries in order, falling through to the next on failure — playlists exist precisely to
  list fallback mirrors, so using only the first entry wastes their purpose.
- Guard against a playlist pointing at another playlist (bounded depth).

## Non-goals

- HLS (`.m3u8` segment manifests) — D5. Detect and report, don't implement.
- Station directory APIs. The consuming game's job.
- Caching resolved URLs across runs.

## Acceptance

- A `.pls` and an `.m3u` URL both stream successfully.
- A redirect chain (including http → https) resolves and streams.
- A redirect loop and an over-long chain both fail with a clear message, not a hang.
- A playlist whose first entry is dead falls through to a working later entry.
- An HLS `.m3u8` is rejected with a message naming HLS, not a generic decode failure.

## Links

up → `phase-stations` · needs → `tls-streams` (for cross-scheme redirects) · tested via →
`fake-icecast-server` (redirect + error statuses)
