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

## D6 — Commit a generated MP3 fixture, and the generator that made it

**Decided 2026-09-08, resolving Q1.** Committing audio is fine *in this repo* — surfer's
"never commit audio files" rule is that project's, and is about licensing rather than file
type. We sidestep licensing entirely by generating our own:

- A short **oscillator piece** (a few seconds; something not-entirely-unpleasant rather than a
  bare sine, so a human debugging by ear can actually tell it's playing correctly and hear
  glitches) rendered to MP3 with **ffmpeg/libmp3lame**, confirmed available locally.
- **Both** artifacts are committed: the generator script *and* its `.mp3` output. The script
  keeps it reproducible and auditable; the committed output means tests need no encoder at
  runtime and CI needs no ffmpeg.
- Content is ours, so it's freely redistributable and no third-party audio ever enters the
  repo. Worth marking CC0 explicitly in a note beside it.

**Why not generate at test time:** it makes ffmpeg a test dependency on every machine and in
CI, for a file that never changes. Committing a few hundred KB is cheaper than that.

**Deliberately a real signal, not silence.** Silence would decode fine while hiding exactly
the corruption we care about — the lookahead bug produced *audio*, just less of it.

---

## D7 — Documented one-command build; surfer references the repo

**Decided 2026-09-08, resolving Q2.** Scope is deliberately minimal: **do no more work than
surfer needs.**

- This repo supports a documented one-command build producing the artifact.
- Surfer references the repo by link and vendors the built artifact.
- **No CI, no release automation, no platform matrix until something actually needs them.**

**How GDExtensions normally lifecycle** (the question behind Q2): the conventional pattern is
exactly the endpoint we sketched — repo with a godot-cpp submodule and a build script → CI
builds each platform on tag → a GitHub release ships an `addons/<name>/` folder containing the
`.gdextension` and `bin/` → consumers unzip it into their project. Godot asset-library entries
are essentially that zip. We already have the build half; only the release half is missing.

So D7 isn't a different architecture from the normal lifecycle — it's the same shape with the
automation deferred until it's earned. The important property either way: **surfer's pipeline
never needs SCons or MSVC**, since it consumes a built artifact.

**Consequence:** `platform-matrix` is demoted from "build all platforms in CI" to "make the
build command clean and documented", with CI as a later trigger-based addition.

---

## D8 — No station URL ships with this repo; directories over curated lists

**Decided 2026-09-08, resolving Q3.** Research findings and the resulting policy.

### The finding that forced this

**SomaFM — the station our POC was hardcoded against — explicitly forbids exactly what we
were doing.** Verified at the primary source
([somafm.com/contact/tos.html](https://somafm.com/contact/tos.html)):

> *"We can't grant permission for third-party SomaFM clients or applications, even
> noncommercial ones."*

They prohibit *"Embedding the Content in any website, application, or platform"* and
*"Re-streaming, retransmitting, or broadcasting the Content"* without written permission,
because their own music licensing doesn't permit them to authorize third-party use of their
streams, branding, or metadata. There are *"a small number of longstanding exceptions … that
predate this policy, but we're not adding new ones."* **No exception for development,
testing, or non-commercial use.**

**Do not write to them asking for permission.** Their site notes that AI tools keep
generating permission requests at their expense. The answer is already published; respect it.

### The decision

- **No station URL ships in this repo, ever.** The demo's `stream_url` defaults to empty and
  `test_stream.gd` requires `--url=`. Removed 2026-09-08.
- Once `fake-icecast-server` exists it becomes the default test source, and the offline suite
  needs no third-party station at all.
- **For surfer: a directory browser plus a user-URL field. No curated shipped list.**
  Shipping station names/URLs is precisely what draws objections, and it inherits both
  maintenance and per-station ToS review.

### Radio Browser is the recommended directory

[api.radio-browser.info](https://api.radio-browser.info/) — ~58,000 stations, free JSON REST
API, **no key**, and it explicitly permits use *"in free and non free software"*. It also
solves the dead-station problem that plagues baked-in lists. Requirements from
[their docs](https://docs.radio-browser.info/): send a descriptive `User-Agent`
(`appname/version`), **resolve the server list dynamically** rather than hardcoding a host,
and POST the click endpoint so popularity stats work.

**Unverified, treat with caution:** the data license (probably public domain, but only
reachable via search snippets — the site is a JS SPA that resisted fetching) and the
commonly-cited 2–3 req/s rate limit (from third-party doc mirrors, *not* official docs, which
state no number). There is **no uptime guarantee**, so radio must degrade gracefully to local
files.

Alternatives are weak: Xiph's `dir.xiph.org` publishes a heavy XML dump with no published
terms (absence of terms is not permission), and Shoutcast's API needs a per-partner key that
forum history suggests is no longer issued.

### Precedent

**Euro Truck Simulator 2 / American Truck Simulator** is the direct analogue: a curated list
shipped in-game, refreshable via "Update From Internet" from an SCS-hosted file, plus
user-editable entries. Notably, SCS's **1.60 update (May 2026)** added *Game Radio* — licensed,
curated in-house stations built partly on streamer-safe libraries — *alongside* the existing
online-radio system. Read that as a studio hedging toward owned content.

Given surfer's aesthetic, a small set of **owned or CC0 vaporwave streams** may beat internet
radio outright, and carries none of this risk.

### Where liability actually sits

Music licensing is consistently the **station operator's** responsibility — they hold the
PRO and sound-recording licenses. A player that opens a public URL and decodes locally is
plausibly distinguishable from proxying or rebroadcasting.

**But this genuinely needs a lawyer, not more searching.** Every source found addresses
*broadcasters*, not *player apps*; none squarely answers whether a commercial game bears
liability for pointing at someone else's stream. The likelier exposure is **contract** (a
station's ToS, as SomaFM demonstrates) than copyright. "The user chose the URL" is modestly
safer because we distribute no station identifiers — but it is not a shield.

### Not this library's job

Directory browsing stays a documented non-goal here: it's HTTP + JSON and pure GDScript, with
no C++ involved. It belongs in the consuming game or the shared toolkit. This library plays a
URL; something else decides which.
