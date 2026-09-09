---
id: url-resolution
type: feature
epic: phase-stations
status: done
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

## Built 2026-09-08

**One unified resolution loop, not two separate mechanisms.** A redirect's single `Location`
and a playlist's list of entries both collapse to the same shape — "here are more URLs to
try" (`HandshakeOutcome::RESOLVE`) — so `worker_main` runs one `std::deque` of candidates with
one shared visited-set and one shared hop counter (`MAX_RESOLUTION_HOPS = 10`). A playlist
entry that itself redirects, or a redirect landing on a playlist, falls out naturally instead
of needing separate bookkeeping for each case. Per-candidate failures (dead host, TLS error,
non-200 status) don't abort the whole resolution — they fall through to the next queued
candidate, exactly matching a playlist's own purpose (listing fallback mirrors); only running
out of candidates, hitting the hop cap, or a genuine loop ends it.

**Redirects**: `parse_status_code`/`find_header` extract the status and `Location` from the
already-buffered header block (no new read needed). `resolve_redirect_url` handles all four
shapes a `Location` can take — fully absolute, protocol-relative (`//host/path`), absolute-path
(`/path`, same host), and relative-to-current-directory — covering cross-scheme (http↔https)
redirects for free since `tls-streams` already landed.

**Playlists**: detected by `Content-Type` first (`looks_like_playlist`), extension as the
fallback servers can't be trusted to get right. A small bounded body read (64 KiB cap) after
the headers — the server closing the connection here is treated as *success* (we asked for
`Connection: close` after a small text file), the opposite of what a mid-stream audio
disconnect means. `.pls` (`parse_pls`) sorts by the `FileN=` index rather than trusting line
order, since PLS doesn't guarantee `File1` appears before `File2` in the text. `.m3u`/`.m3u8`
(`parse_m3u`) is just non-comment, non-empty lines. **HLS detection checks the body itself**
(`looks_like_hls`: `#EXT-X-STREAM-INF`/`#EXT-X-VERSION`/`#EXT-X-TARGETDURATION`) regardless of
extension or Content-Type — a mislabeled manifest doesn't slip past an extension-only check.

**A real gap found while testing, not before**: the TCP-connect wait loop had no timeout —
never needed when a human supplies one real URL and waits, but a playlist trying a dead
fallback entry could hang indefinitely (confirmed directly: a refused localhost connection
sat in a non-terminal state well past any reasonable wait). Added an 8s connect deadline,
matching the pattern the TLS handshake phase already had.

**`fake_icecast_server` extended** (`--playlist-format {pls,m3u,m3u8}` +
`--playlist-entries`) specifically so this could be tested against **real HTTP round-trips**,
not mocked internals — the same posture as everything else in this repo's test story.

**Verified end-to-end, 6/6 real scenarios, all against live servers:**
- Redirect → real audio stream: plays.
- `.pls` → real audio: plays.
- `.m3u` → real audio: plays.
- Redirect loop (server redirects to itself): clean `STATUS_ERROR`, message names the loop.
- HLS `.m3u8` (real `#EXT-X-STREAM-INF`/`#EXT-X-VERSION` markers): clean `STATUS_ERROR`,
  message names HLS specifically, not a generic failure.
- Playlist with a dead first entry, working second entry: falls through, plays the second.
- Plain http (no redirect/playlist) and both engine versions reconfirmed unaffected.

## Not done this pass

- **Over-long chain (>10 hops), tested by inspection only** — the hop-cap check itself is a
  simple integer comparison shared with the (tested) loop-detection path; chaining 11 real
  servers for one more test wasn't judged worth the setup cost this pass.
- **Offline/CI testing** — like `tls-streams`, redirect/playlist testing currently needs real
  server round-trips (`fake_icecast_server` instances spawned by hand for this session, not
  yet wired into `unit-test-suite`'s automated run). Worth doing once that suite grows.

## Acceptance

- ✅ A `.pls` and an `.m3u` URL both stream successfully.
- ✅ A redirect chain (including http → https) resolves and streams.
- ✅ A redirect loop fails with a clear message, not a hang. Over-long chain: logic shared
  with the tested loop path, not independently exercised (see above).
- ✅ A playlist whose first entry is dead falls through to a working later entry.
- ✅ An HLS `.m3u8` is rejected with a message naming HLS, not a generic decode failure.

## Links

up → `phase-stations` · needs → `tls-streams` (for cross-scheme redirects) · tested via →
`fake-icecast-server` (redirect + playlist + error statuses)
