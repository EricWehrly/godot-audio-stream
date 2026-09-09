#!/usr/bin/env python3
"""A minimal Icecast-shaped MP3 server for offline, deterministic testing.

Serves the committed fixture (fixtures/tone.mp3, see generate_fixture.py and
docs/design/decisions.md D6) on a loop, at a controlled byte rate with a
configurable burst-on-connect -- reproducing the real behaviour documented in
docs/design/architecture.md ("Icecast bursts on connect") without depending on
a live station's uptime or jitter.

Deliberately NOT real Icecast (see docs/features/fake-icecast-server.md for
why): Icecast is a relay, not a source, so "real Icecast in Docker" is three
moving parts and gives us no fault-injection hooks. This is one dependency-free
script instead, with every fault as a flag.

Usage:
    python server.py [--port 8000] [--bitrate-kbps 128]
                      [--burst-kbps 500] [--burst-seconds 5]
                      [--drop-after SECONDS] [--stall-at SECONDS]
                      [--truncate] [--garbage-at SECONDS]
                      [--http-error CODE] [--redirect URL]
                      [--slow-headers] [--icy-metadata]

Each fault flag is independent and optional; combine as needed per test case.
"""
import argparse
import http.server
import random
import sys
import time
from pathlib import Path

FIXTURE_PATH = Path(__file__).parent / "fixtures" / "tone.mp3"
ICY_META_INTERVAL = 16000  # bytes between metadata blocks, when enabled


def load_fixture() -> bytes:
    if not FIXTURE_PATH.exists():
        print(
            "Fixture missing: %s\nRun generate_fixture.py first." % FIXTURE_PATH,
            file=sys.stderr,
        )
        raise SystemExit(1)
    return FIXTURE_PATH.read_bytes()


def looped_bytes(data: bytes, count: int, offset: int = 0) -> bytes:
    """`count` bytes from `data`, wrapping around -- an infinite stream from a
    finite fixture, needed since real radio never ends."""
    if count <= 0:
        return b""
    n = len(data)
    start = offset % n
    out = bytearray()
    while len(out) < count:
        take = min(count - len(out), n - start)
        out += data[start:start + take]
        start = (start + take) % n
    return bytes(out)


class Config:
    """Populated once from argparse; shared read-only across request threads."""

    def __init__(self, args: argparse.Namespace):
        self.port = args.port
        self.bitrate_bps = args.bitrate_kbps * 1000
        self.burst_bps = args.burst_kbps * 1000
        self.burst_seconds = args.burst_seconds
        self.drop_after = args.drop_after
        self.stall_at = args.stall_at
        self.truncate = args.truncate
        self.garbage_at = args.garbage_at
        self.http_error = args.http_error
        self.redirect = args.redirect
        self.slow_headers = args.slow_headers
        self.icy_metadata = args.icy_metadata
        self.playlist_format = args.playlist_format
        self.playlist_entries = args.playlist_entries.split(",") if args.playlist_entries else []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "fake-icecast/0.1"

    fixture: bytes = b""
    config: Config = None  # type: ignore[assignment]

    def log_message(self, fmt: str, *args) -> None:  # quieter test output
        pass

    def do_GET(self) -> None:
        cfg = self.config

        if cfg.http_error:
            self.send_response(cfg.http_error)
            self.end_headers()
            return

        if cfg.redirect:
            self.send_response(302)
            self.send_header("Location", cfg.redirect)
            self.end_headers()
            return

        if cfg.playlist_format:
            self._serve_playlist()
            return

        if cfg.slow_headers:
            self._send_headers_byte_by_byte()
        else:
            self._send_headers_normal()

        try:
            self._stream_body()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # client (or our own --drop-after) closed -- not a test failure

    def _serve_playlist(self) -> None:
        fmt = self.config.playlist_format
        entries = self.config.playlist_entries
        if fmt == "pls":
            content_type = "audio/x-scpls"
            lines = ["[playlist]"]
            for i, url in enumerate(entries, start=1):
                lines.append("File%d=%s" % (i, url))
            lines.append("NumberOfEntries=%d" % len(entries))
            body = ("\r\n".join(lines) + "\r\n").encode("utf-8")
        else:  # m3u / m3u8 -- same line-based shape, extension picked via URL
            content_type = "audio/x-mpegurl" if fmt == "m3u" else "application/vnd.apple.mpegurl"
            lines = ["#EXTM3U"] + list(entries)
            body = ("\r\n".join(lines) + "\r\n").encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_headers_normal(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("icy-name", "fake-icecast test stream")
        self.send_header("icy-br", str(self.config.bitrate_bps // 1000))
        self.send_header("Connection", "close")
        if self.config.icy_metadata and self.headers.get("Icy-MetaData") == "1":
            self.send_header("icy-metaint", str(ICY_META_INTERVAL))
        self.end_headers()

    def _send_headers_byte_by_byte(self) -> None:
        # Exercises RadioStream's handshake-timeout path -- a slow-loris server
        # that trickles the header block instead of sending it in one write.
        raw = (
            b"HTTP/1.0 200 OK\r\n"
            b"Content-Type: audio/mpeg\r\n"
            b"icy-name: fake-icecast test stream\r\n"
            b"Connection: close\r\n\r\n"
        )
        for b in raw:
            self.wfile.write(bytes([b]))
            self.wfile.flush()
            time.sleep(0.05)

    def _stream_body(self) -> None:
        cfg = self.config
        started = time.monotonic()
        bytes_sent = 0
        meta_countdown = ICY_META_INTERVAL
        send_metadata = cfg.icy_metadata and self.headers.get("Icy-MetaData") == "1"

        while True:
            elapsed = time.monotonic() - started

            if cfg.drop_after is not None and elapsed >= cfg.drop_after:
                return
            if cfg.stall_at is not None and elapsed >= cfg.stall_at:
                time.sleep(3600)  # holds the connection open, sends nothing
                return

            rate = cfg.burst_bps if elapsed < cfg.burst_seconds else cfg.bitrate_bps
            # One chunk's worth for a ~50ms tick at the target rate.
            chunk_size = max(1, int(rate / 8 * 0.05))

            if cfg.truncate and bytes_sent + chunk_size >= 32000:
                # Cut an MP3 frame off mid-frame instead of on a clean boundary.
                chunk_size = max(1, chunk_size - 7)
                chunk = looped_bytes(self.fixture, chunk_size, bytes_sent)
                self.wfile.write(chunk)
                return

            chunk = bytearray(looped_bytes(self.fixture, chunk_size, bytes_sent))

            if cfg.garbage_at is not None and elapsed >= cfg.garbage_at:
                # Corrupt one frame's worth of bytes with random data, then
                # resume clean -- tests the decoder's resync-on-garbage path.
                for i in range(min(64, len(chunk))):
                    chunk[i] = random.randint(0, 255)
                cfg.garbage_at = None  # once per connection, not every tick

            if send_metadata:
                chunk, meta_countdown = self._interleave_metadata(
                    bytes(chunk), meta_countdown
                )

            self.wfile.write(bytes(chunk))
            bytes_sent += chunk_size
            time.sleep(0.05)

    def _interleave_metadata(self, chunk: bytes, countdown: int) -> tuple:
        # ICY metadata: a 1-byte length/16 field then that many*16 bytes of
        # 'StreamTitle=...;' text, inserted every icy-metaint bytes of audio.
        # See docs/features/icy-metadata.md -- this is the interleaving that
        # feature's consumer must strip back out.
        out = bytearray()
        pos = 0
        while pos < len(chunk):
            take = min(countdown, len(chunk) - pos)
            out += chunk[pos:pos + take]
            pos += take
            countdown -= take
            if countdown == 0:
                title = b"StreamTitle='fake-icecast test track';"
                pad = (-len(title)) % 16
                block = title + b"\x00" * pad
                length_byte = len(block) // 16
                out.append(length_byte)
                out += block
                countdown = ICY_META_INTERVAL
        return bytes(out), countdown


def build_config() -> Config:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--bitrate-kbps", type=int, default=128)
    p.add_argument("--burst-kbps", type=int, default=500)
    p.add_argument("--burst-seconds", type=float, default=3.0)
    p.add_argument("--drop-after", type=float, default=None,
                    help="Close the connection after N seconds.")
    p.add_argument("--stall-at", type=float, default=None,
                    help="Stop sending bytes (no close) after N seconds.")
    p.add_argument("--truncate", action="store_true",
                    help="Cut the stream mid-MP3-frame near the 32KB mark.")
    p.add_argument("--garbage-at", type=float, default=None,
                    help="Corrupt one chunk of bytes at N seconds, then resume clean.")
    p.add_argument("--http-error", type=int, default=None,
                    help="Return this HTTP status instead of streaming (e.g. 404).")
    p.add_argument("--redirect", type=str, default=None,
                    help="Return a 302 to this URL instead of streaming.")
    p.add_argument("--slow-headers", action="store_true",
                    help="Trickle response headers one byte per 50ms.")
    p.add_argument("--icy-metadata", action="store_true",
                    help="Honor Icy-MetaData:1 requests with interleaved titles.")
    p.add_argument("--playlist-format", choices=["pls", "m3u", "m3u8"], default=None,
                    help="Serve a playlist body (see --playlist-entries) instead of streaming.")
    p.add_argument("--playlist-entries", type=str, default="",
                    help="Comma-separated URLs for --playlist-format's body, in order.")
    return Config(p.parse_args())


def main() -> int:
    cfg = build_config()
    Handler.fixture = load_fixture()
    Handler.config = cfg

    with http.server.ThreadingHTTPServer(("127.0.0.1", cfg.port), Handler) as httpd:
        print("fake-icecast listening on http://127.0.0.1:%d/stream" % cfg.port)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
