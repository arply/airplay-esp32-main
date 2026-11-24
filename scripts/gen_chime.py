"""Render the arply boot chime ("F5 sparkle", 2.0 s, -18 dBFS peak).

Usage: python3 scripts/gen_chime.py [sample_rate] [out_dir]   (numpy, scipy)
Writes <out_dir>/arply_chime_<rate>.pcm (s16le, mono) and a .wav to listen
to. The firmware embeds the .pcm matching CONFIG_OUTPUT_SAMPLE_RATE_HZ.
"""
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt

SR = int(sys.argv[1]) if len(sys.argv) > 1 else 44100
OUT = sys.argv[2] if len(sys.argv) > 2 else "."
PEAK_DBFS = -18.0


def hz(n):  # MIDI note -> Hz
    return 440.0 * 2 ** ((n - 69) / 12)


def tone(freq, start, dur, total, attack=0.08, decay=1.2, bright=0.35,
         detune_cents=4.0, n_partials=8):
    t = np.arange(int(total * SR)) / SR
    out = np.zeros_like(t)
    x = t - start
    m = (x >= 0) & (x < dur)
    x = x[m]
    # bloom envelope: smooth rise, exponential tail
    env = (1 - np.exp(-x / attack)) * np.exp(-x / decay)
    sig = np.zeros_like(x)
    for d in (-detune_cents, 0.0, detune_cents):
        f = freq * 2 ** (d / 1200)
        for k in range(1, n_partials + 1):
            amp = bright ** (k - 1) / k ** 0.5
            # upper partials fade faster -> sound mellows as it rings
            sig += amp * np.exp(-x * (k - 1) * 0.9) * np.sin(2 * np.pi * f * k * x)
    out[m] = env * sig / 3
    return out


def lowpass(sig, fc):
    return sosfilt(butter(2, fc, fs=SR, output="sos"), sig)


def finish(sig, name, reverb=True):
    if reverb:  # tiny diffuse tail: a few decaying delays
        tail = np.zeros_like(sig)
        for ms, g in ((37, .30), (53, .25), (71, .20), (97, .15), (131, .10)):
            d = int(ms * SR / 1000)
            tail[d:] += g * sig[:-d]
        sig = sig + lowpass(tail, 3000)
    fade = int(0.25 * SR)
    sig[-fade:] *= np.linspace(1, 0, fade) ** 2
    sig = sig / np.max(np.abs(sig)) * 10 ** (PEAK_DBFS / 20)
    wavfile.write(name, SR, (sig * 32767).astype(np.int16))
    print(name, f"{len(sig)/SR:.2f}s")




C = [48, 55, 60, 64, 67, 72]  # C3 G3 C4 E4 G4 C5, with C6 on top as shimmer
T, decay, step, sparkle_at, sparkle_decay = 2.0, 0.7, 0.06, 0.42, 0.6

s = sum(tone(hz(n), i * step, T - i * step, T, attack=0.03, decay=decay, bright=0.33)
        * (1.0 - i * 0.08) for i, n in enumerate(C))
s = s + tone(hz(84), sparkle_at, T - sparkle_at, T, attack=0.02, decay=sparkle_decay,
             bright=0.15, detune_cents=7) * 0.35
base = os.path.join(OUT, f"arply_chime_{SR}")
finish(lowpass(s, 5000), base + ".wav")

sr, d = wavfile.read(base + ".wav")
d.astype("<i2").tofile(base + ".pcm")
print(base + ".pcm", len(d) * 2, "bytes")
