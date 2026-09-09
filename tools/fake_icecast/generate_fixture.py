#!/usr/bin/env python3
"""Renders the committed test fixture (docs/design/decisions.md D6).

Generates a short, deliberately-audible chord arpeggio -- not silence, since
silence decodes fine while hiding exactly the corruption fake_icecast_server's
consumers need to catch (see RadioStream's DECODE_LOOKAHEAD invariant). Content
is entirely ours: a hardcoded sine-wave sequence, no external audio anywhere in
the chain. Requires ffmpeg (libmp3lame) to encode; the WAV step is pure stdlib.

Run this only when the fixture needs regenerating -- the committed
fixtures/tone.mp3 is what tests actually use, so nothing at test time depends
on ffmpeg being installed.
"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

SAMPLE_RATE = 44100
DURATION_SECONDS = 6.0
AMPLITUDE = 0.28  # headroom so the chord's sum of tones never clips

# A gentle major-ish arpeggio, low enough to not be piercing, high enough to
# be obviously "on" when someone is listening for corruption/dropouts.
# (root, third, fifth, octave) in Hz, stepped every NOTE_SECONDS.
NOTE_SEQUENCE_HZ = [220.00, 277.18, 329.63, 440.00, 329.63, 277.18]
NOTE_SECONDS = DURATION_SECONDS / len(NOTE_SEQUENCE_HZ)

OUT_DIR = Path(__file__).parent / "fixtures"
WAV_PATH = OUT_DIR / "_tone_tmp.wav"
MP3_PATH = OUT_DIR / "tone.mp3"


def _envelope(t: float, note_t: float) -> float:
    """Quick fade in/out per note so notes don't click at their boundaries."""
    fade = 0.015
    if note_t < fade:
        return note_t / fade
    if note_t > NOTE_SECONDS - fade:
        return (NOTE_SECONDS - note_t) / fade
    return 1.0


def generate_wav(path: Path) -> None:
    total_samples = int(SAMPLE_RATE * DURATION_SECONDS)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for i in range(total_samples):
            t = i / SAMPLE_RATE
            note_index = min(int(t / NOTE_SECONDS), len(NOTE_SEQUENCE_HZ) - 1)
            note_t = t - note_index * NOTE_SECONDS
            freq = NOTE_SEQUENCE_HZ[note_index]
            # Root + a soft fifth overtone reads as a chord, not a bare beep.
            sample = math.sin(2 * math.pi * freq * t)
            sample += 0.35 * math.sin(2 * math.pi * freq * 1.5 * t)
            sample *= AMPLITUDE * _envelope(t, note_t)
            frames += struct.pack("<h", int(sample * 32767))
        w.writeframes(bytes(frames))


def encode_mp3(wav_path: Path, mp3_path: Path) -> None:
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-i", str(wav_path),
            "-codec:a", "libmp3lame", "-b:a", "128k",
            str(mp3_path),
        ],
        check=True,
    )


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    generate_wav(WAV_PATH)
    try:
        encode_mp3(WAV_PATH, MP3_PATH)
    except FileNotFoundError:
        print("ffmpeg not found on PATH -- install it to regenerate the fixture.", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        print("ffmpeg failed: %s" % exc, file=sys.stderr)
        return 1
    finally:
        WAV_PATH.unlink(missing_ok=True)
    print("wrote %s (%d bytes)" % (MP3_PATH, MP3_PATH.stat().st_size))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
