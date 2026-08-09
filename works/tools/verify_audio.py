#!/usr/bin/env python3
"""Verify the mpsx-simple-audio test melody with FFT-based pitch detection.

Expected (per 3520 ms loop, 8 kHz U8 mono, sine):
  0-300ms   C4  261.63 | 1080-1380 C5 523.25 | 2160-2460 C6 1046.50
  300-360   gap       | 1380-1440 gap        | 2460-2520 gap
  360-660   E4  329.63 | 1440-1740 E5 659.25 | 2520-3520 C4 (sustained)
  660-720   gap       | 1740-1800 gap
  720-1020  G4  392.00 | 1800-2100 G5 783.99
  1020-1080 gap       | 2100-2160 gap
then loop repeats.
"""
import struct, sys, math
import numpy as np

NOTES = [(261.63, 'C4'), (329.63, 'E4'), (392.00, 'G4'),
         (523.25, 'C5'), (659.25, 'E5'), (783.99, 'G5'), (1046.50, 'C6')]

def read_wav(path):
    raw = open(path, 'rb').read()
    pos = 12
    nch, rate, pcm = 1, 0, b''
    while pos + 8 <= len(raw):
        cid = raw[pos:pos+4]
        csize = struct.unpack('<I', raw[pos+4:pos+8])[0]
        body = raw[pos+8:pos+8+csize]
        if cid == b'fmt ':
            nch = struct.unpack('<H', body[2:4])[0]
            rate = struct.unpack('<I', body[4:8])[0]
        elif cid == b'data':
            pcm = body if csize > 0 else raw[pos+8:]
            break
        pos += 8 + csize + (csize & 1)
    s = np.frombuffer(pcm, dtype='<i2').astype(np.float64)
    if nch == 2:
        s = (s[0::2] + s[1::2]) / 2.0
    return rate, s

def nearest(f):
    best, bd = None, 1e9
    for nf, name in NOTES:
        d = abs(f - nf)
        if d < bd:
            bd, best = d, name
    return best

def analyze(path, max_sec):
    rate, s = read_wav(path)
    print(f"wav: rate={rate}Hz frames={len(s)} dur={len(s)/rate:.2f}s  (analyzing first {max_sec}s)")
    win = int(rate * 0.1)            # 100 ms window, non-overlapping
    nfft = 8192                      # zero-padded FFT -> ~5.4 Hz resolution
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    mask = (freqs >= 40) & (freqs <= 2000)
    nmax = int(max_sec * rate / win)
    rows = []
    for k in range(min(nmax, len(s) // win)):
        chunk = s[k*win:(k+1)*win]
        rms = float(np.sqrt(np.mean(chunk**2)))
        if rms < 500:
            rows.append((k*win/rate, 0.0, 0.0))
            continue
        spec = np.abs(np.fft.rfft(chunk * np.hanning(len(chunk)), nfft))
        idx = int(np.argmax(spec[mask]))
        f = float(freqs[mask][idx])
        rows.append((k*win/rate, rms, f))
    # per-100ms timeline
    tl = ''.join('%-3s ' % ('gap' if f == 0 else nearest(f)) for _, _, f in rows)
    print("\nPer-100ms timeline (t=0 -> %.1fs):" % (len(rows) * 0.1))
    for i in range(0, len(tl), 96):
        print(tl[i:i+96])
    # segments
    print("\n%-5s %-10s %-8s %-12s %s" % ("note", "start(s)", "dur(ms)", "freq", "vs expected"))
    exp = {0:'C4',360:'E4',720:'G4',1080:'C5',1440:'E5',1800:'G5',2160:'C6',2520:'C4'}
    segs = []
    for t, rms, f in rows:
        kind = 'gap' if f == 0 else nearest(f)
        if segs and segs[-1][0] == kind:
            s_ = segs[-1]
            s_[2] = t + win/rate
            s_[4].append(f)
        else:
            segs.append([kind, t, t + win/rate, f, [] if f else None])
    for kind, t0, t1, f0, flist in segs:
        if kind == 'gap':
            print("%-5s %-10.2f %-8d %-12s %s" % (kind, t0, (t1-t0)*1000, "-", "-"))
        else:
            med = float(np.median(flist))
            center_ms = (t0 + t1) / 2.0 * 1000.0
            pos = center_ms % 3520.0
            base = min(exp, key=lambda b: abs(pos - b))
            expk = exp[base]
            ok = "OK" if kind == expk else "MISMATCH"
            print("%-5s %-10.2f %-8d %-12.0f %s (expected %s @%dms)" % (kind, t0, (t1-t0)*1000, med, ok, expk, base))

if __name__ == '__main__':
    analyze(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 10.6)
