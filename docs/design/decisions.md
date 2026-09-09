# Recorded Decisions

Numbered so commits and docs can cite them. Check here before re-litigating scope,
architecture, or platform questions.

---

## D1 — Surfer-first scope, public but unpromised

**Decided 2026-09-08.** Public on GitHub, but priorities follow what surfer needs. Windows
first. The API is free to churn; no compatibility guarantees to third parties.

**Why:** the alternative (aim at the Godot asset library) front-loads a build matrix, CI
binaries, API stability and stranger-facing docs *before* the library has proven useful in
even one game. Hardening can follow evidence rather than precede it.

**Consequences:** `platform-matrix` sits in `phase-ship`, not phase 1. Breaking API changes
are acceptable and don't need a deprecation cycle. If the library later proves broadly
useful, revisit — this decision is cheap to reverse, which is most of its appeal.

---

## D2 — Keep the poll-based boundary; migration is a contingency, not a plan

**Decided 2026-09-08.** `RadioStream` stays a `RefCounted` that GDScript polls via
`pop_frames()` into an `AudioStreamGenerator`. A native `AudioStreamPlayback` subclass is
documented in `audiostreamplayback-migration.md` as a **contingency with measured triggers**,
not scheduled work.

**Why:** the current shape works, is easy to debug (the audio plumbing is visible GDScript),
and its main theoretical cost — per-frame PCM marshaling — is *measurable* before it's paid
for. `phase-measure` exists partly to decide this with numbers instead of taste.

**Trigger conditions** (any one justifies revisiting) are recorded in that feature doc.

**Risk accepted:** if migration does prove necessary, more code will have been built against
the polling shape. Judged acceptable because the C++ core (socket, decode, FIFO) is
*independent* of how audio leaves it — a migration replaces the output surface, not the guts.

---

## D3 — Web is a documented non-goal for the native path; a browser fallback is a separate feature

**Decided 2026-09-08.** This extension cannot work on the web platform. Both halves of the
design are unavailable there, per Godot's own `exporting_for_web` documentation:

- *"Low-level networking is not implemented due to lacking support in browsers."* Only
  HTTPClient, HTTPRequest, WebSocket and WebRTC exist — **no `StreamPeerTCP`**.
- *"Procedural audio generation is not supported."* — **no `AudioStreamGenerator`**.

So the input path and the output path are each independently blocked. No amount of tuning
reaches web from this architecture.

**Consequence:** `web-audio-fallback` is scoped as a genuinely separate implementation —
hand the URL to a browser `<audio>` element through `JavaScriptBridge` and let the browser
fetch and decode. It would play, but sits outside Godot's audio bus graph entirely (which
matters less than it sounds, since web supports no `AudioEffect`s or reverb anyway).

**Recorded because this is exactly the kind of thing that gets re-derived.** Surfer's D2 wants
a viable web tier, so "why is there no radio on web?" will be asked again.

---

## D4 — `api_version=4.6`, built against godot-cpp `master`

**Decided 2026-09-08.** godot-cpp has no 4.6 or 4.7 branch (upstream tops out at
`godot-4.5-stable`), but `master` ships one `extension_api` JSON per engine version and
*requires* an explicit `api_version=`. Building with `api_version=4.6` means the generator
binds only 4.6 API surface, making `compatibility_minimum = "4.6"` an enforced property
rather than a hope.

**Verified, not assumed:** one binary loads and passes `test_load.gd` in both Godot 4.6 and
4.7. Surfer is on 4.7 while the sibling projects stay on 4.6, so one artifact must serve both.

---

## D5 — MP3 only; AAC/HLS excluded on licensing grounds

**Decided 2026-09-08.** minimp3 is CC0 (public-domain dedication in the header itself),
satisfying the ecosystem's permissive-licenses-only rule. AAC carries patent encumbrance and
HLS adds real complexity for a segmented-playlist protocol we'd have to implement.

**Consequence:** stations serving only AAC are unsupported and will stay that way.
`opus-codec` covers the permissive expansion path instead — Icecast serves Ogg widely, and
Opus is BSD-licensed.

---

## Open questions

**Q1 — Test fixture audio.** Offline decoder tests need MP3 bytes. Surfer's hard rule
("never commit audio files") is about *that* repo and about licensing, but the spirit
applies. Leading option: generate synthetic MP3 frames programmatically at test time (a
tone or silence), so nothing copyrighted is ever committed and the fixture is reproducible.
Alternative: commit a few hundred bytes of self-generated CC0 tone. Resolve in
`fake-icecast-server`.

**Q2 — How does surfer consume this?** The addon taxonomy in surfer's CLAUDE.md offers a
submodule-in-`lib/`-plus-symlink pattern (for repos under active iteration) or vendoring it
like a third-party addon. This has a binary artifact, which neither pattern handles cleanly.
Resolve in `surfer-integration`.

**Q3 — Whose responsibility is stream licensing?** Whether a shipped game may play a given
third-party station is a ToS question, wholly separate from whether the bytes decode. It
likely argues for user-supplied URLs over anything hardcoded. Not an engineering decision,
but it should be made before shipping.
