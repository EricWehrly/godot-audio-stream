# godot-audio-stream (POC)

A GDExtension that plays a **continuous internet radio stream** (Icecast/Shoutcast MP3)
in Godot 4, which the engine cannot do on its own.

Standalone POC spun out of [surfer](../../../Dropbox/Projects/surfer) — see
`docs/features/music-library-discovery.md` there for the consuming context.

## The problem

Godot 4 has no way to progressively decode compressed audio as bytes arrive:

- `AudioStreamMP3` / `AudioStreamOggVorbis` are **whole-file** decoders. You hand them a
  complete buffer; there is no "here's more data, keep going" API.
- `AudioStreamGenerator` + `AudioStreamGeneratorPlayback.push_buffer()` *is* the real-time
  streaming primitive, but it takes raw **PCM** — it can't accept compressed bytes.

The gap between those two is the entire reason this extension exists. It's a known,
unresolved engine request: [godot-proposals#11782](https://github.com/godotengine/godot-proposals/issues/11782)
asks for exactly this and confirms it is "not possible with what is already available."
The documented GDScript-only workaround (download fixed chunks, chain them with
`AudioStreamPlaylist`) produces audible stutter at every chunk boundary.

## Design

C++ owns only the part GDScript *can't* do; the audio graph stays in GDScript where it's
easy to inspect and tweak.

```
  ┌─ C++ (RadioStream) ──────────────────────┐   ┌─ GDScript (main.gd) ──────────┐
  │  worker thread:                          │   │  _process:                    │
  │    StreamPeerTCP read                    │   │    playback.get_frames_avail  │
  │      → incremental minimp3 decode        │──▶│    → radio.pop_frames(n)      │
  │      → mutex-guarded PCM FIFO            │   │    → playback.push_buffer()   │
  └──────────────────────────────────────────┘   └───────────────────────────────┘
```

`RadioStream` is a `RefCounted` with three responsibilities: the HTTP handshake, the
read+decode worker thread, and the thread-safe FIFO. It knows nothing about Godot's audio
graph. Cost of this split: PCM marshals across the binding every frame. Fine for a POC;
if it shows up in a profile, the fix is moving `push_buffer` into C++.

## Findings so far

- **godot-cpp has no 4.6/4.7 branch, but that doesn't matter.** Upstream branches/tags top
  out at `4.5`; `master` tracks newer. Crucially, master ships **one `extension_api` JSON
  per engine version** (`gdextension/extension_api-4-6.json`, `-4-7.json`, with
  `supported_api_versions = ["4.3" … "4.7"]`) and *requires* an explicit `api_version=`
  argument. So we build against `master` (pinned by submodule SHA) with `api_version=4.6`:
  the generator binds only 4.6 API surface, making `compatibility_minimum = "4.6"` an
  enforced property rather than a hope. One binary should then serve surfer (4.7) and the
  4.6 siblings — still worth verifying by actually loading it in both.
- **Do not send `Icy-MetaData: 1`.** It makes Icecast interleave title blocks into the
  audio every `icy-metaint` bytes, corrupting MP3 framing unless de-interleaved. Omitting
  the header yields a clean byte stream. Track titles are a deliberate follow-up.
- **minimp3 is CC0** (public-domain dedication in the header itself), satisfying surfer's
  permissive-licenses-only rule. Its `mp3dec_decode_frame` is built for partial buffers:
  it reports bytes consumed and returns 0 samples while skipping junk/ID3, which is
  normal at stream start and must not be treated as a decode failure.
- **https is not supported** — would need `StreamPeerTLS` wrapping. Out of POC scope.
- **minimp3 needs lookahead past the frame it is decoding.** This was the single biggest
  bug, and it is silent — no error, just quiet audio loss. minimp3 validates a frame by
  checking that a sync header follows it, so decoding the instant bytes arrive leaves one
  complete frame with nothing after it, validation fails, and it skips the frame hunting
  for sync. Measured: **43% of the stream discarded** (416,383 of 978,952 bytes) and audio
  output pinned at 69% of real time, while the input backlog read 0 and the network read a
  healthy 128 kbps — every individual signal looked fine. Fix: never decode the last
  `DECODE_LOOKAHEAD` (4 KiB) of the buffer. Skipped bytes dropped 416,383 → 836.
- **Icecast bursts on connect.** SomaFM opens at ~540 kbps for the first few seconds to
  fill a client buffer fast, then settles to 128 kbps. Two consequences: an *average*
  bitrate reading is meaningless (it read 174 kbps against a 128 kbps stream — always
  measure per-interval), and the burst overfills the FIFO. Since production then matches
  consumption exactly, the FIFO never drains on its own and sits pinned at its cap.
  Discarding the *newest* audio there would mean a fresh audible gap on every overflow, so
  the FIFO trims the **oldest** audio back to a target instead: one discontinuity during
  startup, none afterwards.
- **Lock granularity matters more than expected.** Taking the PCM mutex once per MP3 frame
  let the consumer starve the decode thread. Decoding a whole batch outside the lock and
  appending under a single lock fixed it.

## Build

Requires Python 3, SCons (`pip install scons`), and MSVC (VS 2022 with C++ tools; SCons
finds it via vswhere, no need for `cl` on PATH).

```bash
git submodule update --init --depth 1
scons platform=windows target=template_debug arch=x86_64 -j8
```

Then open `demo/` in Godot and run. The DLL lands in `demo/bin/` (gitignored).

### Build gotchas (cost real time — read before debugging a build)

**1. A phantom VS install silently sends the build to MinGW.** `SConstruct` works
around this; the workaround is not optional on this dev machine.

SCons resolves MSVC `14.3` to the *first registered* VS instance. On this machine that's
`F:\Program Files\Microsoft Visual Studio\2022\Community` — registered with the VS
installer but carrying **no C++ workload at all** (no `VC\Tools\MSVC`, no `vcvars64.bat`).
SCons rejects it and **does not fall through** to the working
`C:\...\2022\BuildTools` instance (MSVC 14.44.35207). So `msvc.exists()` returns False,
godot-cpp's `tools/windows.py:91` falls back to MinGW, and since `g++` isn't installed
every single compile fails with:

```
scons: *** [...material...o] The system cannot find the file specified
```

That message names neither the compiler nor the real problem. `scons verbose=yes` is what
exposes it — the command line reads `g++ -o ...` when it should read `cl`.

Diagnose SCons's own view with `SCONS_MSCOMMON_DEBUG=-`, which prints every instance it
considers and why it discards them.

The fix: `find_vcvars()` in `SConstruct` calls vswhere with
`-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, then *verifies
`vcvars64.bat` exists on disk* before accepting an instance, and points
`MSVC_USE_SCRIPT` at it — bypassing SCons's instance selection entirely. Override with
`scons vcvars=<path to vcvars64.bat>`.

**2. `MSVS_VERSION` is `None` at link time.** godot-cpp deliberately sets
`MSVC_VERSION = None` so SCons picks for itself, but nothing then populates `MSVS_VERSION`,
and `mslink.py` regex-matches it unguarded:

```
TypeError: expected string or bytes-like object, got 'NoneType'
```

`SConstruct` restores it from the detected VS product major (17 → `14.3`).

**3. godot-cpp `master` requires an explicit `api_version=`.** Without it:
`scons: *** 'api_version' must be provided`. Defaulted to `4.6` in `SConstruct`.

## Verification

Two harnesses, both headless:

```bash
# Does the extension load and register? (run in both 4.6 and 4.7)
<godot> --headless --path demo -s test_load.gd

# Soak: live station, real-time drain, starvation counter. Default 45s.
<godot> --headless --path demo -s test_stream.gd ++ --seconds=330
```

`test_stream.gd` drains the FIFO at exactly the stream's own sample rate to simulate a
real-time consumer, so starvation is measurable **without an audio device** — the same
failure an `AudioStreamGenerator` underrun would be. That makes the core property CI-able
rather than something only a human with speakers can check.

## Success criteria

| # | Criterion | Status |
|---|-----------|--------|
| 1 | 0 starvations over a 5+ minute run | ✅ PASS (330s) |
| 2 | Extension loads in **both** 4.6 and 4.7 from one build | ✅ PASS (both) |
| 3 | No trims after the buffer settles | ✅ PASS |
| 4 | Consumption drift < 1% vs real-time | ✅ PASS (0.00%) |
| 5 | No audible dropouts, confirmed by a human listening | ⚑ **not done — human check** |
| 6 | Worker CPU cost fits alongside a game's frame budget | ⚑ not measured |

Criteria 5 and 6 are deliberately not self-certified. A starvation counter reading 0 says
the buffer stayed fed; it does not say the audio sounds right, and that judgement isn't
mine to make.

### Measured (330s, SomaFM Groove Salad, 128 kbps)

```
ran              330.0s              44100 Hz, 2 ch
network          128 kbps steady     (~500 kbps burst for the first ~5s)
byte accounting  5494115 recv = 5489476 audio + 836 skipped + 3803 backlog
decoded          13134 mp3 frames    consumption drift 0.00% vs real-time
fifo             4.24s - 4.47s across the whole run, no drift
buffer trims     2 total, 0 after settling
STARVATIONS      0
```

`skip` stayed at 836 bytes for all 330s — i.e. the only bytes ever discarded were at
connect. 99.92% of everything received became audio.

## Open risks

- **Godot API objects off the main thread.** The worker creates and drives a
  `StreamPeerTCP` from a `std::thread`. This should be fine (no scene-tree access, atomic
  refcounting), but it's unverified and is the most likely source of a subtle failure.
  Fallback if it misbehaves: poll the socket on the main thread, decode on the worker.
- **Reconnect behavior.** A dropped stream currently just goes to `STATUS_ERROR`. Real use
  needs backoff + resume.
- **Station format assumptions.** MP3 over plain http only. Many modern stations are
  AAC or HLS; AAC brings patent questions and is deliberately out of scope.
- **Not yet a licensing answer.** Whether a shipped game may play a given third-party
  station is a ToS question wholly separate from whether the bytes decode.
