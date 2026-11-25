"""Debug probe for the output DSP on a real arply.

Plays a synthetic groove (kick, E1-B1 bass line, chords, hi-hat) through the
device's own DSP and volume, captures what goes into the output processing
and what reaches the DAC (POST/GET /api/debug/capture), and prints per
configuration: output peak, limiter use, clipping, bass gain swing and the
gain per band (output vs input, volume removed).

Usage: python3 scripts/dsp_probe.py http://dut.local     (needs numpy)
Uses preview only (the saved profile is not touched) and restores the volume.
"""
import io, json, sys, time, urllib.request, wave
import numpy as np

H = sys.argv[1] if len(sys.argv) > 1 else 'http://arply.local'
FS = 44100


def groove(seconds=9.0):
    """Bass-heavy test music, like the calibration page's built-in groove."""
    t = np.arange(int(seconds * FS)) / FS
    x = np.zeros_like(t)
    beat = 0.6
    for b in np.arange(0, seconds, beat):           # kick: 110 -> 45 Hz drop
        m = (t >= b) & (t < b + 0.4); tt = t[m] - b
        x[m] += 0.9 * np.exp(-tt / 0.12) * np.sin(2 * np.pi * (45 * tt + 1.95 * (1 - np.exp(-tt / 0.03))))
    notes = [28, 28, 31, 28, 33, 28, 31, 35]           # E1..B1, 41-62 Hz
    for k, b in enumerate(np.arange(0, seconds, beat / 2)):
        f = 440 * 2 ** ((notes[k % 8] - 69) / 12); m = (t >= b) & (t < b + 0.27); tt = t[m] - b
        e = np.minimum(1, tt / 0.005) * np.exp(-tt / 0.25)
        x[m] += 0.55 * e * (np.sin(2 * np.pi * f * tt) + 0.35 * np.sin(4 * np.pi * f * tt))
    for m_ in (52, 55, 59):                             # E minor pad
        x += 0.06 * np.sin(2 * np.pi * 440 * 2 ** ((m_ - 69) / 12) * t)
    rng = np.random.default_rng(7)
    for b in np.arange(0, seconds, beat / 2):           # hi-hat
        m = (t >= b) & (t < b + 0.05); x[m] += 0.12 * np.exp(-(t[m] - b) / 0.012) * np.diff(rng.uniform(-1, 1, m.sum() + 1))
    x *= 0.5 / np.abs(x).max()
    return np.stack([x, x], axis=1)

def req(path, body=None, raw=None, ctype='application/json', timeout=60):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    r = urllib.request.Request(H + path, data=data, method='POST' if data is not None or body == {} else 'GET',
                               headers={'Content-Type': ctype})
    with urllib.request.urlopen(r, timeout=timeout) as f:
        return f.read()

pcm = (np.clip(groove(), -1, 1) * 32767).astype('<i2').tobytes()
vol0 = json.loads(req('/api/audio/volume'))['db']

def capture(cfg, vol_db):
    req('/api/audio/volume', {'db': vol_db})
    req('/api/dsp/preview', cfg)
    time.sleep(0.3)
    req('/api/debug/capture?seconds=10', {})
    req('/api/dsp/listen?channels=2', raw=pcm, ctype='application/octet-stream', timeout=120)
    time.sleep(10.5)
    w = wave.open(io.BytesIO(req('/api/debug/capture', timeout=120)))
    x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 4).astype(float) / 32768
    req('/api/dsp/stop', {})
    return x[:, :2], x[:, 2:]

def band_db(x, lo, hi):
    X = np.abs(np.fft.rfft(x[:, 0] * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    return 10 * np.log10(X[(f >= lo) & (f < hi)].sum() + 1e-20)

def bandpass_env(x, lo, hi, win=2205):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / FS); X[(f < lo) | (f >= hi)] = 0
    y = np.fft.irfft(X, len(x)) ** 2
    return np.sqrt(np.convolve(y, np.ones(win) / win, 'valid')[::win])

def analyse(name, cfg, vol_db):
    inp, out = capture(cfg, vol_db)
    # the volume curve: q15 = ((db+30)/30)^2 -> linear gain
    lin = ((vol_db + 30) / 30) ** 2
    n = min(len(inp), len(out)); inp, out = inp[1000:n], out[1000:n]
    peak = 20 * np.log10(np.abs(out).max() + 1e-12)
    lim = 100 * np.mean(np.abs(out) > 0.891)
    clip = 100 * np.mean(np.abs(out) >= 0.9999)
    bands = [(20, 45), (45, 60), (60, 90), (90, 120), (120, 180), (180, 300), (300, 1000), (1000, 5000), (5000, 16000)]
    gains = [band_db(out, a, b) - band_db(inp, a, b) - 20 * np.log10(lin) for a, b in bands]
    ei, eo = bandpass_env(inp[:, 0], 40, 120), bandpass_env(out[:, 0], 40, 120)
    ok = ei > ei.max() * 0.05
    ratio = 20 * np.log10(eo[ok] / ei[ok])
    pump = ratio.max() - ratio.min() if ok.any() else 0
    print(f'\n== {name} (volume {vol_db:+.1f} dB, gain {20*np.log10(lin):+.1f} dB)')
    print(f'   out peak {peak:+.1f} dBFS, in limiter {lim:.2f} %, clipped {clip:.3f} %, bass gain swing {pump:.1f} dB')
    print('   gain per band (out vs in, volume removed):')
    print('   ' + '  '.join(f'{a}-{b}:{g:+.1f}' for (a, b), g in zip(bands, gains)))
    return inp, out

base = {'mode': 'stereo', 'preamp_db': 0, 'channels': {'L': [], 'R': [], 'M': []}}
def bass(on, f, boost, harm): return {**base, 'bass': {'on': on, 'f_low': f, 'boost': boost, 'harmonics': harm}}
try:
    for vol in (vol0, -3.0):
        analyse('reference: DSP flat', bass(False, 120, 0.5, 0.0), vol)
        analyse('bass: limit 120, boost 50 %, harmonics 0', bass(True, 120, 0.5, 0.0), vol)
        analyse('bass: limit 120, boost 50 %, harmonics 50 %', bass(True, 120, 0.5, 0.5), vol)
finally:
    req('/api/dsp/revert', {})
    req('/api/audio/volume', {'db': vol0})
    print(f'\nrestored: saved profile, volume {vol0:+.1f} dB')
