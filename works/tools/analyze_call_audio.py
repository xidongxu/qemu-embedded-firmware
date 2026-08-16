#!/usr/bin/env python3
"""Analyze a call/loopback output WAV: per-100ms dominant pitch + loudness.

For the dual-QEMU pjsip calls we feed a 1 kHz (caller) and a 440 Hz (callee)
sine source, so each side's out.wav should show the OTHER side's frequency:
  - caller out.wav -> ~439 Hz  (hears the callee's 440 Hz)
  - callee out.wav -> ~1001 Hz (hears the caller's 1 kHz)

(QEMU's wav audiodev re-samples everything to 44100 Hz, hence 439/1001.)

Usage:
  python analyze_call_audio.py <out.wav> [<out2.wav> ...]
"""
import struct, sys
import numpy as np


def read_wav(path):
    raw = open(path, 'rb').read()
    pos = 12
    nch, rate, data_off = 1, 0, None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        csize = struct.unpack('<I', raw[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            nch = struct.unpack('<H', raw[pos + 10:pos + 12])[0]
            rate = struct.unpack('<I', raw[pos + 12:pos + 16])[0]
        elif cid == b'data':
            data_off = pos + 8
            break
        pos += 8 + csize + (csize & 1)
    s = np.frombuffer(raw[data_off:], dtype='<i2').astype(np.float64)
    if nch == 2:
        s = (s[0::2] + s[1::2]) / 2.0
    return rate, s


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    for path in sys.argv[1:]:
        rate, s = read_wav(path)
        win = int(rate * 0.1)          # 100 ms window
        nfft = 16384
        freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
        mask = (freqs >= 40) & (freqs <= 3000)
        rows = []
        for k in range(len(s) // win):
            chunk = s[k * win:(k + 1) * win]
            rms = float(np.sqrt(np.mean(chunk ** 2)))
            if rms < 500:              # quiet window -> no tone
                rows.append(0.0)
                continue
            spec = np.abs(np.fft.rfft(chunk * np.hanning(len(chunk)), nfft))
            rows.append(float(freqs[mask][np.argmax(spec[mask])]))
        from collections import Counter
        hist = Counter()
        for f in rows:
            if f > 0:
                hist[int(round(f))] += 1
        loud = sum(1 for f in rows if f > 0)
        print(f"{path}: rate={rate} loud={loud}/{len(rows)} top={hist.most_common(5)}")


if __name__ == '__main__':
    main()
