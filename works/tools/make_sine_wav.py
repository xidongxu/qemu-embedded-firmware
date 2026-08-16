#!/usr/bin/env python3
"""Generate a pure-tone mono S16 WAV used as the mpsx-simple-mic test source.

Used by the pjsip call tests to feed a known pitch into each QEMU instance:
  - caller source: 1 kHz  -> the callee should hear ~1001 Hz in its out.wav
  - callee source: 440 Hz -> the caller should hear ~439 Hz  in its out.wav

Usage:
  python make_sine_wav.py <out.wav> <freq_hz> [rate=8000] [amp=16000] [secs=5]
"""
import struct, sys, math, wave


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[1]
    freq = float(sys.argv[2])
    rate = int(sys.argv[3]) if len(sys.argv) > 3 else 8000
    amp = int(sys.argv[4]) if len(sys.argv) > 4 else 16000
    secs = int(sys.argv[5]) if len(sys.argv) > 5 else 5

    n = rate * secs
    w = wave.open(out, 'wb')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(n):
        frames += struct.pack('<h', int(amp * math.sin(2 * math.pi * freq * i / rate)))
    w.writeframes(bytes(frames))
    w.close()
    print(f"wrote {out}: {freq} Hz, {rate} Hz/{amp} amp/{secs}s mono S16")


if __name__ == '__main__':
    main()
