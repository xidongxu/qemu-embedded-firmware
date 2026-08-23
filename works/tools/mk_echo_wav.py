#!/usr/bin/env python3
"""Generate a synthetic acoustic-echo WAV for the EC (echo cancellation) test.

The echo simulates the guest's own speaker output leaking back into its own
microphone: it is a delayed, attenuated copy of the tone the guest plays.
The guest's EC should cancel this signal before it is sent back over RTP.

Usage:
  python mk_echo_wav.py <in.wav> <out.wav> [--delay-ms 100] [--gain 0.5]
"""
import argparse
import wave
import numpy as np


def read_wav(path):
    with wave.open(path, 'rb') as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        rate = w.getframerate()
        n = w.getnframes()
        raw = np.frombuffer(w.readframes(n), dtype=np.int16)
    if sw != 2:
        raise SystemExit(f"unsupported sample width {sw} (need 16-bit)")
    data = raw.astype(np.float32) / 32768.0
    if nch > 1:
        data = data.reshape(-1, nch)[:, 0]
    return data, rate, nch


def write_wav(path, data, rate, nch=1):
    pcm = (np.clip(data, -1.0, 1.0) * 32767.0).astype(np.int16)
    with wave.open(path, 'wb') as w:
        w.setnchannels(nch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('in_wav')
    ap.add_argument('out_wav')
    ap.add_argument('--delay-ms', type=float, default=100.0,
                    help='acoustic round-trip delay (default 100 ms)')
    ap.add_argument('--gain', type=float, default=0.5,
                    help='acoustic attenuation (default 0.5 = -6 dB)')
    args = ap.parse_args()

    sig, rate, _ = read_wav(args.in_wav)
    delay_s = args.delay_ms / 1000.0
    delay_n = int(round(delay_s * rate))

    # Zero-pad with silence (the echo arrives late) then attenuate.
    echo = np.concatenate([np.zeros(delay_n, dtype=np.float32), sig]) * args.gain

    write_wav(args.out_wav, echo, rate)
    print(f"echo.wav: {len(echo)/rate:.1f}s @ {rate} Hz, "
          f"delay={args.delay_ms:.0f} ms, gain={args.gain} (-{20*np.log10(max(args.gain,1e-9)):.1f} dB)")


if __name__ == '__main__':
    main()
