#!/usr/bin/env python3
"""Dash sound generator — synthesises the on-device sound bank.

Output: 8 kHz, 1 channel, 8-bit unsigned PCM `.raw` files under data/sounds/.
Run from project root:  python3 tools/sounds/generate.py

Each sound is a small Python function that returns a numpy float array in
[-1.0, 1.0]. The driver writes them out as u8 PCM. No external sound assets
are pulled in — everything is procedurally generated so the build is
deterministic and re-runs after any pitch / duration tweak.
"""

from __future__ import annotations

import math
import os
import struct
from pathlib import Path
from typing import Callable, Dict

import numpy as np

SAMPLE_RATE = 8000
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "data" / "sounds"


# ---- DSP primitives ----------------------------------------------------------

def silence(seconds: float) -> np.ndarray:
    return np.zeros(int(seconds * SAMPLE_RATE), dtype=np.float32)


def sine(freq_hz: float, seconds: float, amp: float = 0.7) -> np.ndarray:
    n = int(seconds * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    return amp * np.sin(2 * math.pi * freq_hz * t).astype(np.float32)


def square(freq_hz: float, seconds: float, amp: float = 0.4) -> np.ndarray:
    return amp * np.sign(sine(freq_hz, seconds, 1.0)).astype(np.float32)


def triangle(freq_hz: float, seconds: float, amp: float = 0.6) -> np.ndarray:
    n = int(seconds * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    return (amp * (2 * np.abs(2 * (t * freq_hz - np.floor(t * freq_hz + 0.5))) - 1)
            ).astype(np.float32)


def noise(seconds: float, amp: float = 0.3) -> np.ndarray:
    n = int(seconds * SAMPLE_RATE)
    return (amp * (np.random.rand(n) * 2 - 1)).astype(np.float32)


def envelope(buf: np.ndarray, attack_s: float = 0.005, release_s: float = 0.05) -> np.ndarray:
    """Apply a simple linear attack/release envelope to avoid clicks."""
    n = len(buf)
    a = int(attack_s * SAMPLE_RATE)
    r = int(release_s * SAMPLE_RATE)
    env = np.ones(n, dtype=np.float32)
    if a > 0 and a < n:
        env[:a] = np.linspace(0.0, 1.0, a, dtype=np.float32)
    if r > 0 and r < n:
        env[-r:] = np.linspace(1.0, 0.0, r, dtype=np.float32)
    return buf * env


def pitch_sweep(f0: float, f1: float, seconds: float, amp: float = 0.6,
                wave: str = "sine") -> np.ndarray:
    n = int(seconds * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    freq = np.linspace(f0, f1, n, dtype=np.float32)
    phase = 2 * math.pi * np.cumsum(freq) / SAMPLE_RATE
    if wave == "sine":
        return amp * np.sin(phase).astype(np.float32)
    if wave == "square":
        return amp * np.sign(np.sin(phase)).astype(np.float32)
    if wave == "triangle":
        return (amp * (2 * np.abs(2 * (phase / (2 * math.pi)
                                      - np.floor(phase / (2 * math.pi) + 0.5))) - 1)
                ).astype(np.float32)
    raise ValueError(f"unknown wave {wave}")


def concat(*chunks: np.ndarray) -> np.ndarray:
    return np.concatenate(chunks).astype(np.float32)


# ---- Sound definitions -------------------------------------------------------

def boot() -> np.ndarray:
    """Three-note rising arpeggio, ~600 ms total."""
    notes = [(523, 0.12), (659, 0.12), (784, 0.18)]   # C5, E5, G5
    parts = []
    for f, d in notes:
        parts.append(envelope(triangle(f, d), 0.005, 0.04))
    return concat(*parts)


def wake() -> np.ndarray:
    """Two quick chirps, sounds like a yawn ending."""
    return concat(
        envelope(pitch_sweep(420, 660, 0.18, wave="sine"), 0.01, 0.06),
        silence(0.06),
        envelope(pitch_sweep(660, 540, 0.12, wave="sine"), 0.01, 0.04),
    )


def sleep() -> np.ndarray:
    """Downward sweep, 'going to sleep' feel."""
    return envelope(pitch_sweep(700, 200, 0.6, wave="triangle", amp=0.5), 0.02, 0.15)


def session_start() -> np.ndarray:
    """Confident chime to mark session start."""
    return concat(
        envelope(triangle(523, 0.10), 0.005, 0.03),
        envelope(triangle(659, 0.10), 0.005, 0.03),
        envelope(triangle(880, 0.20), 0.005, 0.08),
    )


def session_end() -> np.ndarray:
    """Reverse of session_start: softer, falling."""
    return concat(
        envelope(triangle(880, 0.10), 0.005, 0.03),
        envelope(triangle(659, 0.10), 0.005, 0.03),
        envelope(triangle(523, 0.20), 0.005, 0.08),
    )


def session_complete() -> np.ndarray:
    """Triumph fanfare for finishing a session."""
    return concat(
        envelope(triangle(523, 0.14), 0.005, 0.02),
        envelope(triangle(659, 0.14), 0.005, 0.02),
        envelope(triangle(784, 0.14), 0.005, 0.02),
        envelope(triangle(1047, 0.30), 0.005, 0.12),
    )


def whoa() -> np.ndarray:
    """Cute 'whoa!' / 'wheee!' for when the cube gets shaken or bumped.
    Quick rising-then-falling pitch sweep with a wobble — sounds like a
    little robot losing its balance."""
    return concat(
        envelope(pitch_sweep(520, 880, 0.18, wave="triangle", amp=0.55), 0.005, 0.04),
        envelope(pitch_sweep(880, 660, 0.18, wave="triangle", amp=0.5), 0.005, 0.06),
    )


def tilt() -> np.ndarray:
    """A questioning 'huh?' for tilts and orientation changes — short
    rising sweep so it sounds like a head-tilt sound effect."""
    return envelope(pitch_sweep(420, 720, 0.14, wave="sine", amp=0.5), 0.005, 0.05)


def boop() -> np.ndarray:
    """Soft 'boop' for gentle bumps — friendlier than tap_ack."""
    return envelope(sine(620, 0.06, amp=0.45), 0.003, 0.03)


def test_tone() -> np.ndarray:
    """A clearly audible 1.2-second tone used by the speaker-test step in
    the diagnostic walkthrough. Two-note alternating pattern so it's
    obviously synthetic and doesn't get confused with any ambient sound."""
    a = envelope(sine(660, 0.30, amp=0.55), 0.005, 0.05)
    b = envelope(sine(880, 0.30, amp=0.55), 0.005, 0.05)
    return concat(a, silence(0.06), b, silence(0.06), a, silence(0.06), b)


def double_tap_ack() -> np.ndarray:
    """Distinct two-note chime fired when a double-tap is classified.
    Pitches higher than tap_ack so the user can tell them apart."""
    return concat(
        envelope(sine(900, 0.05, amp=0.5), 0.002, 0.02),
        envelope(sine(1200, 0.09, amp=0.5), 0.002, 0.04),
    )


def triple_tap_ack() -> np.ndarray:
    """Three-note descending chime for triple-tap (deep-sleep gesture).
    Distinct from the long sleep chime — this is the *acknowledgement*
    that triple-tap was detected; sleep.raw plays after for the actual
    going-to-sleep cue."""
    return concat(
        envelope(sine(1100, 0.06, amp=0.5), 0.002, 0.02),
        envelope(sine(880, 0.06, amp=0.5), 0.002, 0.02),
        envelope(sine(660, 0.12, amp=0.5), 0.002, 0.05),
    )


def tap_ack() -> np.ndarray:
    """Tiny click for tap acknowledgement."""
    return envelope(sine(1200, 0.04, amp=0.4), 0.002, 0.02)


def tap_ack_2() -> np.ndarray:
    """Slightly higher click — variant 2."""
    return envelope(sine(1450, 0.04, amp=0.4), 0.002, 0.02)


def tap_ack_3() -> np.ndarray:
    """Slightly lower click — variant 3."""
    return envelope(sine(980, 0.04, amp=0.4), 0.002, 0.02)


def good_morning() -> np.ndarray:
    """Two-note 'hello' for first-session-of-the-day."""
    return concat(
        envelope(triangle(523, 0.10), 0.005, 0.03),
        envelope(triangle(784, 0.18), 0.005, 0.08),
    )


def milestone() -> np.ndarray:
    """Extra-long fanfare for round-number session milestones."""
    return concat(
        envelope(triangle(523, 0.10), 0.005, 0.02),
        envelope(triangle(659, 0.10), 0.005, 0.02),
        envelope(triangle(784, 0.10), 0.005, 0.02),
        envelope(triangle(1047, 0.10), 0.005, 0.02),
        envelope(triangle(1318, 0.40), 0.005, 0.15),
    )


def menu_blip() -> np.ndarray:
    return envelope(square(800, 0.04, amp=0.25), 0.001, 0.02)


def menu_confirm() -> np.ndarray:
    return concat(
        envelope(triangle(523, 0.05), 0.002, 0.02),
        envelope(triangle(784, 0.08), 0.002, 0.03),
    )


def menu_back() -> np.ndarray:
    return concat(
        envelope(triangle(784, 0.05), 0.002, 0.02),
        envelope(triangle(523, 0.08), 0.002, 0.03),
    )


def distraction() -> np.ndarray:
    """Disapproving 'uh-uh' for distraction-detected events."""
    return concat(
        envelope(square(330, 0.12, amp=0.4), 0.005, 0.04),
        silence(0.04),
        envelope(square(280, 0.18, amp=0.4), 0.005, 0.06),
    )


def encouragement() -> np.ndarray:
    """Cheerful little jingle for milestones."""
    return concat(
        envelope(triangle(659, 0.10), 0.005, 0.03),
        envelope(triangle(784, 0.10), 0.005, 0.03),
        envelope(triangle(659, 0.10), 0.005, 0.03),
        envelope(triangle(880, 0.20), 0.005, 0.08),
    )


def yawn() -> np.ndarray:
    """Drowsy slow sweep."""
    return envelope(pitch_sweep(440, 220, 0.5, wave="sine", amp=0.4), 0.02, 0.15)


def giggle() -> np.ndarray:
    """A few quick rising-falling beeps."""
    parts = []
    for f in [880, 988, 1175]:
        parts.append(envelope(triangle(f, 0.05, amp=0.4), 0.002, 0.02))
        parts.append(silence(0.03))
    return concat(*parts)


def heartbeat() -> np.ndarray:
    """Two thumps."""
    return concat(
        envelope(sine(110, 0.10, amp=0.6), 0.002, 0.04),
        silence(0.10),
        envelope(sine(110, 0.10, amp=0.6), 0.002, 0.04),
    )


def game_correct() -> np.ndarray:
    return concat(
        envelope(triangle(659, 0.06), 0.002, 0.02),
        envelope(triangle(988, 0.10), 0.002, 0.04),
    )


def game_wrong() -> np.ndarray:
    return concat(
        envelope(square(330, 0.10, amp=0.4), 0.002, 0.04),
        envelope(square(247, 0.18, amp=0.4), 0.002, 0.08),
    )


def game_start() -> np.ndarray:
    return concat(
        envelope(triangle(440, 0.08), 0.002, 0.02),
        envelope(triangle(659, 0.08), 0.002, 0.02),
        envelope(triangle(880, 0.18), 0.002, 0.08),
    )


# ---- Driver ------------------------------------------------------------------

SOUNDS: Dict[str, Callable[[], np.ndarray]] = {
    "boot": boot,
    "wake": wake,
    "sleep": sleep,
    "session_start": session_start,
    "session_end": session_end,
    "session_complete": session_complete,
    "tap_ack": tap_ack,
    "tap_ack_2": tap_ack_2,
    "tap_ack_3": tap_ack_3,
    "double_tap_ack": double_tap_ack,
    "triple_tap_ack": triple_tap_ack,
    "test_tone": test_tone,
    "whoa": whoa,
    "tilt": tilt,
    "boop": boop,
    "good_morning": good_morning,
    "milestone": milestone,
    "menu_blip": menu_blip,
    "menu_confirm": menu_confirm,
    "menu_back": menu_back,
    "distraction": distraction,
    "encouragement": encouragement,
    "yawn": yawn,
    "giggle": giggle,
    "heartbeat": heartbeat,
    "game_correct": game_correct,
    "game_wrong": game_wrong,
    "game_start": game_start,
}


def lowpass(buf: np.ndarray, alpha: float = 0.35) -> np.ndarray:
    """One-pole IIR lowpass — attenuates the high frequencies that make
    8 kHz u8 PCM sound harsh. Smaller alpha = more rolloff. Used on the
    reverb wet signal so the tail feels like a small room, not a series
    of sharp echoes."""
    out = np.empty_like(buf)
    prev = 0.0
    for i, x in enumerate(buf):
        prev = alpha * x + (1.0 - alpha) * prev
        out[i] = prev
    return out


def reverb(buf: np.ndarray, dry: float = 0.6, wet: float = 0.55) -> np.ndarray:
    """Algorithmic plate-style reverb tuned for Dash's 8 kHz speaker.

    Six exponentially-decaying taps with prime-ish delays (so they don't
    line up rhythmically), each lowpass-filtered to take the bite out of
    the wet signal. Result: a noticeable, soft tail without sounding
    washy. Peak-normalised to 0.95 to leave headroom for the I2S amp.
    """
    delays_ms = [23, 47, 79, 113, 167, 211]
    amps      = [0.55, 0.42, 0.32, 0.24, 0.18, 0.13]
    n = len(buf)
    tail = int(0.35 * SAMPLE_RATE)  # 350 ms reverb tail beyond input
    out = np.zeros(n + tail, dtype=np.float32)
    out[:n] = buf * dry
    # Build the wet bus separately so we can lowpass-filter it once.
    wet_bus = np.zeros(n + tail, dtype=np.float32)
    for d_ms, amp in zip(delays_ms, amps):
        d = int(d_ms * SAMPLE_RATE / 1000)
        if d < n + tail - 1:
            wet_bus[d:d + n] += buf * amp
    wet_bus = lowpass(wet_bus, alpha=0.32)
    out += wet_bus * wet
    peak = float(np.max(np.abs(out)) or 1.0)
    if peak > 0.95:
        out *= 0.95 / peak
    return out.astype(np.float32)


def to_u8_pcm(buf: np.ndarray) -> bytes:
    """Convert float32 [-1, 1] to u8 (offset-binary, 128 = silence)."""
    clipped = np.clip(buf, -1.0, 1.0)
    u8 = ((clipped * 127.0) + 128.0).astype(np.uint8)
    return u8.tobytes()


# Sounds that shouldn't get reverb — short clicks where a tail would smear
# rapid repeats (e.g. tap_ack played on every cube tap, menu_blip etc).
DRY_SOUNDS = {"tap_ack", "tap_ack_2", "tap_ack_3",
              "double_tap_ack", "triple_tap_ack", "menu_blip"}


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"writing {len(SOUNDS)} sounds to {OUT}")
    total_bytes = 0
    for name, gen in sorted(SOUNDS.items()):
        np.random.seed(hash(name) & 0xFFFFFFFF)
        buf = gen()
        if name not in DRY_SOUNDS:
            buf = reverb(buf)
        data = to_u8_pcm(buf)
        out_path = OUT / f"{name}.raw"
        out_path.write_bytes(data)
        ms = 1000.0 * len(buf) / SAMPLE_RATE
        total_bytes += len(data)
        tag = " " if name in DRY_SOUNDS else "~"
        print(f"  {name:20s}{tag} {ms:6.0f} ms  {len(data):6d} bytes -> {out_path.name}")
    print(f"\ntotal: {total_bytes} bytes "
          f"({total_bytes/1024:.1f} KiB at {SAMPLE_RATE} Hz mono u8)")
    print("(~ = with reverb tail)")


if __name__ == "__main__":
    main()
