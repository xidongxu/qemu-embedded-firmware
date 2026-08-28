#!/usr/bin/env python3
"""Grab the last chunk of QEMU guest serial (tcp:2345) output.

Usage: grab_serial.py [seconds] [filter]
Connects to 127.0.0.1:2345, reads for `seconds` (default 3), prints only
lines containing `filter` (substring) if given, otherwise the tail.
"""
import socket
import sys
import time

host, port = "127.0.0.1", 2345
seconds = 3
sub = None
if len(sys.argv) > 1:
    seconds = int(sys.argv[1])
if len(sys.argv) > 2:
    sub = sys.argv[2]

s = socket.create_connection((host, port), 3)
s.settimeout(0.5)
end = time.time() + seconds
buf = b""
while time.time() < end:
    try:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    except socket.timeout:
        pass
s.close()

text = buf.decode("utf-8", "replace")
lines = text.splitlines()
if sub:
    lines = [ln for ln in lines if sub in ln]
    print("\n".join(lines[-60:]))
else:
    print("\n".join(lines[-60:]))
