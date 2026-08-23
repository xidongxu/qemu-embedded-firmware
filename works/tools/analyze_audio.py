#!/usr/bin/env python3
"""Analyze how much 1 kHz energy is present in a WAV.

Used by the EC test: the guest plays a 1 kHz tone (reference) and a delayed
copy leaks into its mic (the echo).  After echo cancellation, the RTP sent by
the guest should contain almost no 1 kHz.

Usage:
  python analyze_audio.py <file.wav> [--freq 1000] [--bw 40]
"""
import argparse
import struct
import numpy as np


def read_wav(path):
    """Tolerant WAV reader.  pjsua's wav_writer leaves the RIFF/data size
    fields at 0 when it is force-killed, which the stdlib wave module rejects
    ('not a WAVE file').  We parse the chunks manually instead."""
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'RIFF':
        raise SystemExit('not a RIFF file')
    fmti = raw.find(b'fmt ')
    di = raw.find(b'data')
    if fmti < 0 or di < 0:
        raise SystemExit('fmt/data chunk missing')
    # fmt: audio_fmt(H) ch(H) rate(I) byte_rate(I) align(H) bits(H)
    _, nch, rate, _, _, bits = struct.unpack('<HHIIHH', raw[fmti + 8:fmti + 24])
    if bits != 16:
        raise SystemExit(f"unsupported sample width {bits} (need 16-bit)")
    pcm = raw[di + 8:]
    data = np.frombuffer(pcm, dtype='<i2').astype(np.float32) / 32768.0
    if nch > 1:
        data = data.reshape(-1, nch)[:, 0]
    return data, rate


def band_power(x, rate, f_center, f_bw):
    """Power in [f_center-bw/2, f_center+bw/2] relative to total power."""
    n = len(x)
    if n < 2:
        return 0.0, 0.0
    # Hann window -> ~ -40 dB sidelobes so a pure tone doesn't bleed much.
    w = np.hanning(n)
    X = np.fft.rfft(x * w)
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    lo, hi = f_center - f_bw / 2, f_center + f_bw / 2
    mask = (freqs >= lo) & (freqs <= hi)
    p_band = float(np.sum(np.abs(X[mask]) ** 2))
    p_tot = float(np.sum(np.abs(X) ** 2))
    return p_band, p_tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--freq', type=float, default=1000.0)
    ap.add_argument('--bw', type=float, default=40.0)
    args = ap.parse_args()

    x, rate = read_wav(args.wav)
    if len(x) == 0:
        raise SystemExit("empty wav")
    dur = len(x) / rate

    p_band, p_tot = band_power(x, rate, args.freq, args.bw)
    ratio_db = 10 * np.log10(p_band / p_tot) if p_tot > 0 else -np.inf
    # Absolute band power in dBFS (1.0 full-scale sine = -3.01 dBFS).
    band_dbfs = 10 * np.log10(p_band / (len(x) / 2.0)) if p_band > 0 else -np.inf
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    rms = float(np.sqrt(np.mean(x ** 2))) if len(x) else 0.0

    print(f"{args.wav}: {dur:.1f}s {rate} Hz, "
          f"peak={peak:.4f}, rms={rms:.5f}")
    print(f"  1 kHz band: ratio={ratio_db:7.1f} dB  abs_power={band_dbfs:7.1f} dBFS")


if __name__ == '__main__':
    main()
