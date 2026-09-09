---
id: icy-metadata
type: feature
epic: phase-stations
design: [docs/design/architecture.md]
status: planned
---

# ICY Metadata — Now Playing

**Goal:** Read the artist/title a station broadcasts, and surface it as a signal.

**Why:** Deliberately skipped in the POC — requesting metadata is what *breaks* naive
streaming, so omitting it was the right call to get audio working. But a music game that can
show "now playing" is meaningfully better than one that can't, and surfer already has a music
browser UI with a Now Playing label that currently only knows about local files.

## How it works (and why it was skipped)

Send `Icy-MetaData: 1` and the server responds with an `icy-metaint: N` header, then
**interleaves** a metadata block into the audio every N bytes:

```
[N bytes audio][1 byte length/16][length*16 bytes metadata][N bytes audio]...
```

If you don't strip those blocks, they land in the MP3 byte stream and corrupt framing. This
is why the POC omits the request header entirely — a clean byte stream needed no
de-interleaving. Adding it means the read path must become metadata-aware.

## Scope

- Send `Icy-MetaData: 1`; parse `icy-metaint` from the response headers.
- De-interleave in the socket-read path, **before** bytes reach the decoder — maintain a
  running counter to the next metadata boundary. This must be exact; an off-by-one corrupts
  audio framing, and the symptom will look like a decoder bug rather than a parsing bug.
- Parse the payload: `StreamTitle='...';StreamUrl='...';`, semicolon-separated, single-quoted.
  Handle empty blocks (the common case — a zero length byte means "nothing changed"), and
  quotes inside titles.
- Decode text as UTF-8, falling back to latin-1: stations are inconsistent and a mis-decode
  should degrade to mojibake, never to a crash or dropped connection.
- Emit `metadata_changed(title, url)` on the main thread (same marshaling rule as
  `reconnect-resilience`'s signals). Also expose the station name from `icy-name`, which
  arrives once in the headers.
- **Keep it optional** (`request_metadata`, default on). The POC's no-metadata path is proven;
  it should remain available as a fallback if a station's interleaving turns out to be broken.

## Non-goals

- Album art / external metadata lookup.
- Ogg/Vorbis comment metadata — that's a different mechanism, deferred to `opus-codec`.
- Scrobbling.

## Acceptance

- Titles update as tracks change on a real station.
- **Byte accounting still closes** with metadata enabled — this is the key regression check.
  If de-interleaving is off by even one byte, skipped bytes will climb and the accounting will
  stop balancing, exactly as it did for the lookahead bug.
- Zero-length metadata blocks (the common case) are handled without a spurious signal.
- With `request_metadata = false`, behaviour is byte-identical to today.
- A station that ignores the request and sends no `icy-metaint` degrades gracefully to no
  metadata.

## Links

up → `phase-stations` · tested via → `fake-icecast-server` (optional interleaving) ·
consumer → `surfer-integration` (surfer's music browser Now Playing label)
