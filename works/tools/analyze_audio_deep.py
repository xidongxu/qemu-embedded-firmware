#!/usr/bin/env python3
"""Deep audio quality analysis for dual-QEMU call recordings.

Per-20 ms frame analysis of RMS energy, dominant pitch and spectral centroid,
then quantifies what the 55-59 % jitter-buffer EMPTY frames (concealed by
G.711 PLC = repeat-last-frame, or zero-filled once the PLC limit is hit) do to
the audible output:

  - silence ratio & run-length: PLC-limit exceeded -> zero fill -> gaps
  - tone-frame ratio & dominant-pitch stability: PLC repeat keeps the pitch
  - spectral centroid: a repeated frame gains harmonics (mechanical feel)

Usage:
  python analyze_audio_deep.py <out.wav> <target_freq_hz> [<out2.wav> <freq2>...]
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
        n_frames = len(s) // 2          # interleaved -> real audio frames
        s = (s[0::2] + s[1::2]) / 2.0   # mix to mono (same length as n_frames)
    else:
        n_frames = len(s)
    return rate, s, n_frames


def analyze(path, target):
    rate, s, n_frames = read_wav(path)
    win = int(rate * 0.02)                       # 20 ms frame
    nf = len(s) // win
    nfft = 4096
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    mask = (freqs >= 40) & (freqs <= 3000)
    SIL = 300.0                                  # RMS silence threshold
    rms_a = np.empty(nf); peak_a = np.zeros(nf); cent_a = np.zeros(nf)
    for k in range(nf):
        c = s[k * win:(k + 1) * win]
        rms_a[k] = float(np.sqrt(np.mean(c ** 2)))
        if rms_a[k] < SIL:
            continue
        spec = np.abs(np.fft.rfft(c * np.hanning(len(c)), nfft))
        sm = spec[mask]
        peak_a[k] = float(freqs[mask][np.argmax(sm)])
        cent_a[k] = float(np.sum(freqs[mask] * sm) / np.sum(sm))

    sil = rms_a < SIL
    n_sil = int(sil.sum()); n_tone = nf - n_sil

    # RMS tiers (20ms frame): silence < 300, weak=PLC-fade 300-3000, normal >3000
    weak = (rms_a >= SIL) & (rms_a < 3000.0)
    norm_f = rms_a >= 3000.0
    n_weak = int(weak.sum()); n_norm = int(norm_f.sum())

    # silence runs (PLC-limit exceeded -> zero fill -> audible gap)
    runs = []
    in_run = False
    for i, is_s in enumerate(sil):
        if is_s and not in_run:
            start = i; in_run = True
        elif not is_s and in_run:
            runs.append((start, i - start)); in_run = False
    if in_run:
        runs.append((start, nf - start))
    n_gaps = len(runs)
    max_gap = max((r[1] for r in runs), default=0)

    # tone frames: pitch / centroid stats
    tone = ~sil
    peaks = peak_a[tone]; cents = cent_a[tone]
    near = (peaks > target * 0.9) & (peaks < target * 1.1)
    near_rate = float(near.mean() * 100) if len(near) else 0.0
    p_med = float(np.median(peaks)) if len(peaks) else 0.0
    c_med = float(np.median(cents)) if len(cents) else 0.0

    # pitch-run continuity: longest consecutive tone frames
    best = cur = 0
    for is_t in tone:
        cur = cur + 1 if is_t else 0
        best = max(best, cur)

    # RMS distribution of tone frames
    tone_rms = rms_a[tone]
    rms_med = float(np.median(tone_rms)) if len(tone_rms) else 0.0

    print(f"[{path}] target={target}Hz  dur={n_frames/rate:.2f}s frames={nf} "
          f"({win/rate*1000:.0f}ms)")
    print(f"  silence: {n_sil}/{nf} ({n_sil*100/nf:.1f}%)  gaps={n_gaps} "
          f"max_gap={max_gap*0.02:.2f}s")
    print(f"  tiers  : normal={n_norm}({n_norm*100/nf:.1f}%) "
          f"weak/PLC-fade={n_weak}({n_weak*100/nf:.1f}%) "
          f"silence={n_sil}({n_sil*100/nf:.1f}%)")
    print(f"  tone   : {n_tone}/{nf} ({n_tone*100/nf:.1f}%)  "
          f"peak_med={p_med:.0f}Hz target_hit={near_rate:.1f}%  "
          f"centroid_med={c_med:.0f}Hz  rms_med={rms_med:.0f}")
    print(f"  continuity: longest_tone_run={best*0.02:.2f}s  "
          f"silence_ratio={n_sil/max(nf,1)*100:.1f}%  "
          f"pitch_only({(len(near)/max(nf,1))*100:.1f}%)")
    return n_sil, n_tone, n_gaps, max_gap, near_rate


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    for i in range(1, len(sys.argv), 2):
        if i + 1 >= len(sys.argv):
            break
        analyze(sys.argv[i], float(sys.argv[i + 1]))


if __name__ == '__main__':
    main()
