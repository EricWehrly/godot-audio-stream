---
id: web-audio-fallback
type: feature
epic: phase-reach
design: [docs/design/decisions.md]
status: planned
---

# Web Audio Fallback

**Goal:** Play a radio stream in a Godot web export, where this extension fundamentally
cannot run.

**Why:** Surfer's renderer decision wants a viable web tier. Without this, a web build of
surfer simply has no internet radio — which is an acceptable outcome, but worth having a
documented path out of rather than a dead end.

## Why the normal path is impossible (D3)

Both halves of the architecture are unavailable on web, per Godot's own `exporting_for_web`
documentation:

- *"Low-level networking is not implemented due to lacking support in browsers."* Only
  HTTPClient, HTTPRequest, WebSocket and WebRTC exist — **no `StreamPeerTCP`**.
- *"Procedural audio generation is not supported."* — **no `AudioStreamGenerator`**.

Input path blocked, output path blocked, independently. **No amount of tuning reaches web from
this architecture**, and a GDExtension wasm build wouldn't help — the restriction is the
browser's, not the engine's.

## The approach

Let the browser do both jobs. Create an HTML `<audio>` element via `JavaScriptBridge`, point
it at the stream URL, and control it from GDScript. Browsers stream and decode MP3/AAC/Opus
natively and handle buffering and reconnection themselves.

## Scope

- A GDScript-only `WebRadioStream` with an API mirroring `RadioStream` where it can
  (`open`/`close`/status/`metadata_changed`), so callers can pick an implementation at
  runtime by platform without branching everywhere.
- `JavaScriptBridge.eval` / `create_callback` to build the element, wire `play`/`pause`/
  `volume`, and receive `error`/`stalled`/`playing` events.
- Handle the browser autoplay policy: audio can't start without a user gesture. Godot's docs
  call this out explicitly, and it means the consuming game needs a "click to start" moment —
  a real UX constraint, not just a technical one.
- Document the honest limitation: **audio bypasses Godot's bus graph entirely**, so no Godot
  `AudioEffect`s and no per-bus volume through the usual path. (Web supports no `AudioEffect`s
  or reverb regardless, so less is lost than it first appears.)

## Spectrum analysis is recoverable — correcting an earlier claim

An earlier draft of this doc asserted that a browser `<audio>` element is opaque to spectrum
analysis and that surfer's audio-reactive visuals would therefore sit idle on web. **That was
wrong.** The Web Audio API can analyze a media element directly:

```js
const src = audioCtx.createMediaElementSource(audioEl);
const analyser = audioCtx.createAnalyser();
src.connect(analyser); analyser.connect(audioCtx.destination);
// analyser.getByteFrequencyData(...) -> back to GDScript via JavaScriptBridge
```

There is also **prior art in this project**: the retired Three.js web implementation did
browser-side audio analysis. So this is a solved problem here, not a research question.

**The real caveat is CORS, not capability.** `createMediaElementSource` on a *cross-origin*
stream produces silence unless the server sends permissive CORS headers and the element sets
`crossOrigin="anonymous"` — the node outputs zeros as a security measure, which will look
like "the analyser is broken" rather than a permissions problem.

Encouragingly, our own probe of SomaFM showed it already sends them:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Headers: *
```

So analysis works for that station. It will **not** work universally, and stations can't be
assumed to send CORS headers. Treat per-station analyser availability as a runtime
capability to detect, not a guarantee — and note this makes it a *different* failure mode
from the native path, where we always own the PCM.

## Non-goals

- Feature parity with the native path. It won't be, and pretending otherwise is worse than
  documenting the gap.
- Web Audio API integration to recover spectrum data. Possible in principle; a much larger
  project.

## Acceptance

- A web export plays a stream after a user gesture.
- The platform split is clean — the same calling code works on both, selecting the
  implementation by platform.
- Limitations are documented where a consumer will actually hit them.

## Links

up → `phase-reach` · decision → D3 · relates → `surfer-integration`
