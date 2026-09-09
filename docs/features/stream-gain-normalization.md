---
id: stream-gain-normalization
type: feature
epic: phase-ship
status: planned
---

# Stream Gain & Normalization

**Goal:** Radio shouldn't be jarringly louder or quieter than local tracks when the music
source switches.

**Why:** A real mixing problem, not a nicety. Broadcast streams are typically mastered and
compressed to be loud — often several dB above a normal music file. In a game where the
player can switch from a local track to a station mid-cruise, that's an unpleasant jump, and
it's the kind of thing that reads as "this feature is broken" rather than "these are
different sources".

Surfer already has a volume slider in its music browser, but that's a user control over the
whole music bus — it doesn't equalize *between* sources.

## Scope

- **Measure** the stream's loudness continuously (a running RMS or EBU R128-style short-term
  loudness over a rolling window) and expose it.
- **Expose a gain** applied at the decode stage, so PCM leaves the library already scaled.
  Cheap — it's a multiply in a loop we already run.
- **Auto-normalize toward a target** loudness, off by default, with a slow time constant.
  Fast auto-gain sounds like pumping; this should be closer to "match the level over ten
  seconds" than to a compressor.
- Handle `icy-br` as a weak hint but **don't trust it** — bitrate says nothing about loudness,
  and stations misreport it anyway.

## Design caution

Two things worth deciding deliberately rather than by default:

- **Don't clip.** Applying gain > 1.0 to already-loud broadcast audio will clip. Either cap
  at unity (only ever attenuate) or add a limiter — and "only ever attenuate" is the simpler,
  safer default, since the problem in practice is radio being *too loud*.
- **Where does this belong?** An argument exists that normalization is the consuming game's
  job via its own audio bus, not this library's. The counter-argument: only this library sees
  the PCM before it's mixed, and doing it here is nearly free. Worth confirming rather than
  assuming — if surfer would rather own it, this feature reduces to just *exposing the
  measurement*.

## Non-goals

- Full dynamic-range compression or a broadcast-style processing chain.
- Per-station remembered levels (possible later; needs persistence that belongs in the game).

## Acceptance

- Switching between a local track and a station at the same user volume produces no jarring
  level jump.
- Auto-normalization never audibly pumps.
- Disabled by default; enabling it is a deliberate choice.
- ⚑ Human checkpoint: this is fundamentally a listening judgement. Meters can show the levels
  match; only a person can say the transition sounds right.

## Links

up → `phase-ship` · relates → `surfer-integration` (surfer's music bus and volume slider)
