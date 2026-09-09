# Architecture

What the pieces are, why the boundary sits where it does, and which invariants are
load-bearing.

## The gap this fills

Godot 4 has two audio facilities and no bridge between them:

- `AudioStreamMP3` / `AudioStreamOggVorbis` — **whole-file** decoders. Hand them a complete
  buffer; there is no "here's more data, keep going".
- `AudioStreamGenerator` + `AudioStreamGeneratorPlayback.push_buffer()` — the real-time
  streaming primitive, but it takes raw **PCM**, not compressed bytes.

Engine-side this is a known, unresolved request
([godot-proposals#11782](https://github.com/godotengine/godot-proposals/issues/11782)). The
GDScript-only workaround — download fixed chunks and chain them with `AudioStreamPlaylist` —
stutters at every chunk boundary.

## Boundary

```
┌─ C++ (RadioStream) ───────────────────────┐   ┌─ GDScript ────────────────────┐
│  worker thread:                           │   │  _process:                    │
│    StreamPeerTCP read                     │   │    playback.get_frames_avail  │
│      → incremental minimp3 decode         │──▶│    → radio.pop_frames(n)      │
│      → mutex-guarded PCM FIFO             │   │    → playback.push_buffer()   │
└───────────────────────────────────────────┘   └───────────────────────────────┘
```

C++ owns only what GDScript *cannot* do. The audio graph stays in GDScript, where it's
visible and tweakable. Rationale and the contingency for changing this: **D2**.

`RadioStream` is a `RefCounted` with three responsibilities:

1. the HTTP GET and response-header handshake
2. a worker thread: socket read → incremental decode
3. a thread-safe PCM FIFO the caller drains

It knows nothing about Godot's audio graph. That independence is deliberate: it means the
D2 migration, if ever triggered, replaces the *output surface* and leaves the guts alone.

## Load-bearing invariants

Each of these was learned by breaking it. Don't "simplify" one without reading why.

### Never decode the tail of the input buffer

minimp3 validates a frame by checking that a sync header follows it. Decoding the instant
bytes arrive leaves one complete frame with nothing after it — validation fails, and minimp3
skips the frame hunting for sync. Measured cost of getting this wrong: **43% of the stream
silently discarded**, audio output pinned at 69% of real time.

`DECODE_LOOKAHEAD` (4 KiB) is always left unread. This is why `get_input_backlog()` reads
~3.7–4 KB in a healthy stream rather than 0 — that backlog is *correct*, not a symptom.

**Why this was hard to find:** every individual health signal looked fine. Backlog 0, network
a clean 128 kbps, no decode errors, no starvation until the buffer drained. Only byte-level
accounting (`received = audio + skipped + backlog`) exposed it. That accounting is worth
keeping for exactly this reason.

### Decode outside the lock, append under it

Taking the PCM mutex once per MP3 frame let a fast consumer starve the decode thread. The
worker now decodes a whole batch into a reused buffer with no lock held, then appends under a
single lock.

### Trim the oldest audio, never the newest

Icecast opens with a burst (measured ~500 kbps for the first ~5s against a 128 kbps stream)
to fill a client buffer fast. That overfills the FIFO — and since production then matches
consumption exactly, it never drains on its own and sits pinned at the cap forever.

Discarding *newly decoded* audio there means a fresh audible gap every time the cap is
touched. Trimming the **oldest** back to `TARGET_FIFO_FRAMES` puts the single discontinuity
during startup instead. Confirmed: 2 trims total, 0 after settling, across a 330s run.

### Averages lie about bitrate

Because of that burst, an average bitrate reading is meaningless — it read 174 kbps against a
128 kbps stream. Always measure per-interval.

## Threading model

One worker thread per stream, spawned in `open()`, joined in `close()` (and the destructor).
It creates and drives its own `StreamPeerTCP`. Nothing touches the scene tree.

Driving Godot API objects from a `std::thread` was flagged as the biggest unknown in the POC.
It is now **empirically retired**: sustained across a 330s run with no scene-tree access and
atomic refcounting holding up. Not proof of correctness, but no longer the top risk.

Shared state is a `std::vector<Vector2>` FIFO plus a read cursor, guarded by one mutex; every
statistic is a `std::atomic`. A lock-free ring buffer is possible but unjustified until
`perf-harness` says the mutex costs anything.

## Known rough edges

- **A dropped stream is terminal** — `STATUS_ERROR` and done. → `reconnect-resilience`
- **Polling, not signals** — callers must poll `get_status()`. → `reconnect-resilience`
- **No redirects, no playlist files** — `tls-streams` is done; `url-resolution` isn't. →
  `phase-stations`
- **The 5ms poll sleep is unexamined** — ~200 wakeups/second regardless of bitrate, a fixed
  cost independent of load. → `perf-harness`, hypothesis 2
- **Windows x86_64 only.** → `platform-matrix`
- **Web is impossible from this architecture** (D3), not merely unimplemented.
