# Roadmap

Internet radio streaming for Godot 4. What exists, what's planned, and why in this order.

Work-item scheme and live index: [`docs/features/README.md`](features/README.md).
Recorded decisions: [`docs/design/decisions.md`](design/decisions.md).

## Where this stands

The POC works and is measured (330s live soak, 0 starvations, 0.00% drift — see the
[README](../README.md)). What it is **not** yet is trustworthy: there are no offline tests,
a dropped connection is terminal, and nothing but plain `http://` MP3 resolves.

**Scope (D1):** surfer-first. Public on GitHub, but priorities follow what surfer needs and
the API is free to churn. No compatibility promises to third parties yet.

## Sequencing rationale

The order below is not arbitrary, and one dependency drives most of it:

> **`fake-icecast-server` is the keystone.** It unblocks *two* phases, not one. Deterministic
> tests need it (today every test requires the open internet and a third party's uptime), and
> so does honest perf measurement — network jitter pollutes every number you'd otherwise
> collect. Measuring an unreliable thing against a variable input wastes the measurement.

So: make it testable, then make it reliable, then measure it, then broaden what it accepts,
then ship it into surfer.

## Epics

| Epic | Goal | Status |
|------|------|--------|
| `phase-poc` | Prove Godot can play a continuous MP3 stream at all | ✅ done |
| `phase-trust` | Deterministic offline tests; survives a dropped stream | planned |
| `phase-measure` | Know what it costs, in CPU and memory | planned |
| `phase-stations` | Accept the URLs real stations actually publish | planned |
| `phase-ship` | Land it in surfer, on the platforms surfer targets | planned |
| `phase-reach` | Extras: Opus, web fallback, architecture contingency | planned |

---

### `phase-poc` — ✅ done

Delivered: `RadioStream` (HTTP handshake, worker thread, mutex-guarded PCM FIFO), a GDScript
demo driving `AudioStreamGenerator`, headless load + soak harnesses, and a `SConstruct` that
survives this machine's Visual Studio installs. Verified in both Godot 4.6 and 4.7 from one
`api_version=4.6` build.

The findings that cost real time — minimp3's lookahead requirement, Icecast's burst-on-connect,
and the phantom VS install — are written up in the [README](../README.md) so they aren't
rediscovered.

### `phase-trust` — make it testable and reliable

Everything today depends on a live third-party stream. That's fine for a POC and unacceptable
as a foundation.

| Feature | What it buys |
|---------|--------------|
| [`fake-icecast-server`](features/fake-icecast-server.md) | Offline, deterministic, fault-injecting stream source. Unblocks real tests *and* clean perf numbers. |
| [`unit-test-suite`](features/unit-test-suite.md) | URL parsing, header parsing, FIFO/trim logic — all pure, all currently untested. |
| [`reconnect-resilience`](features/reconnect-resilience.md) | A dropped stream currently ends the session. Backoff, resume, and status signals instead of polling. |

### `phase-measure` — answer the cost question

Depends on `fake-icecast-server` for a repeatable input.

| Feature | What it buys |
|---------|--------------|
| [`perf-harness`](features/perf-harness.md) | Worker CPU per stream-minute, main-thread `pop_frames` cost, memory over hours, scaling with N streams. |

Which of those need surfer and which don't is worked out in
[`design/performance.md`](design/performance.md) — short version: **most of it is measurable
here, and better here**; only frame-time percentiles under real contention need surfer.

### `phase-stations` — accept real-world URLs

Today only a direct `http://` MP3 endpoint works. That is a minority of what stations publish.

| Feature | What it buys |
|---------|--------------|
| [`tls-streams`](features/tls-streams.md) | `https://` via `StreamPeerTLS`. Increasingly mandatory. |
| [`url-resolution`](features/url-resolution.md) | HTTP redirects and `.m3u`/`.pls` playlists — how station URLs are actually distributed. |
| [`icy-metadata`](features/icy-metadata.md) | Now-playing artist/title. Deliberately skipped in the POC; genuinely wanted for a music UI. |

### `phase-ship` — land it in surfer

| Feature | What it buys |
|---------|--------------|
| [`surfer-integration`](features/surfer-integration.md) | How it's consumed, and the in-game perf verdict `phase-measure` can't give. |
| [`platform-matrix`](features/platform-matrix.md) | Linux/macOS builds; CI so binaries aren't hand-built. |
| [`stream-gain-normalization`](features/stream-gain-normalization.md) | Radio loudness vs. local files — a real mixing problem for a music game. |

### `phase-reach` — later

| Feature | What it buys |
|---------|--------------|
| [`opus-codec`](features/opus-codec.md) | Ogg/Opus stations. Permissive licensing, unlike AAC. |
| [`web-audio-fallback`](features/web-audio-fallback.md) | Web is blocked twice over (D3). A `JavaScriptBridge` `<audio>` path is the only way. |
| [`audiostreamplayback-migration`](features/audiostreamplayback-migration.md) | Contingency (D2): documented trigger conditions, not planned work. |

## Explicit non-goals

- **AAC / HLS stations.** Patent encumbrance plus real complexity. Opus covers the permissive case.
- **Recording streams to disk.** Different feature, different legal posture.
- **Station discovery / directory browsing.** Belongs in the consuming game, not here.
- **Being a general-purpose audio library.** This decodes network audio streams. Local file
  playback is already solved by Godot.
