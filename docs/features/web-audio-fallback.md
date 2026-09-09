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
- Document the honest limitations loudly: **audio bypasses Godot's bus graph entirely**, so
  no effects, no per-bus volume, no spectrum analysis. Surfer's audio-reactive visuals would
  not work from a web radio stream.

## Notable consequence

That last point may matter more than the feature. Surfer drives visuals from audio spectrum
analysis; a browser `<audio>` element is opaque to that. So on web, radio would play but the
game's audio-reactive systems would sit idle. Web supports no `AudioEffect`s or reverb anyway,
so some of this is lost regardless — but it should be a conscious tradeoff, not a surprise
discovered after building it.

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
