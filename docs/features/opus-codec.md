---
id: opus-codec
type: feature
epic: phase-reach
status: planned
---

# Ogg/Opus Support

**Goal:** Decode Ogg-container stations (Opus, and Vorbis if it comes cheaply) alongside MP3.

**Why:** The permissive expansion path. AAC and HLS are excluded on licensing and complexity
grounds (**D5**), which leaves Opus as the way to cover more stations without a patent
question — Icecast serves Ogg widely, and Opus is BSD-licensed. Opus also sounds
better than MP3 at the low bitrates radio uses.

## Scope

- Add an Ogg page/packet demuxer, then an Opus decoder (`libopus` + `libopusfile`, or
  `opus` alone with our own Ogg layer). All permissive; verify each dependency's license
  before vendoring, per the ecosystem rule.
- **Select the codec from `Content-Type`** (`audio/ogg`, `application/ogg`, `audio/opus`) —
  not from the URL extension, which stations get wrong.
- Refactor the decode path behind a small internal interface so `RadioStream` doesn't grow a
  codec branch through the middle of the read loop. The MP3 path must remain byte-identical.
- **The lookahead invariant needs re-deriving, not copying.** `DECODE_LOOKAHEAD` exists
  because of how *minimp3* validates frames. Ogg framing is completely different (pages with
  checksums), so it needs its own analysis of how much buffer a decoder needs before it can
  make progress. Assuming 4 KiB carries over would be exactly the kind of unexamined
  assumption that caused the original bug.
- Vorbis comments (Ogg's metadata mechanism) as a parallel to `icy-metadata` — different
  mechanism, same signal.

## Non-goals

- AAC / HLS (**D5**).
- FLAC streams — rare for radio, large bandwidth.
- Transcoding.

## Acceptance

- A real Ogg/Opus station streams with the same stability criteria as MP3 (soak, 0
  starvations, byte accounting closes).
- MP3 behaviour is provably unchanged — the existing test suite passes untouched.
- Codec selection works from `Content-Type` even when the URL extension disagrees.
- Every added dependency's license is verified permissive and its notice preserved.

## Links

up → `phase-reach` · relates → `icy-metadata` (parallel mechanism for Ogg) · decision → D5
