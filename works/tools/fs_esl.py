#!/usr/bin/env python3
"""Query FreeSWITCH over ESL (mod_event_socket, default 8021/ClueCon).

Usage: fs_esl.py "api show channels as json"
       fs_esl.py "api uuid dump <uuid>"
"""
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 8021
PASS = "ClueCon"

cmd = sys.argv[1] if len(sys.argv) > 1 else "api status"
if not cmd.endswith("\n"):
    cmd += "\n"

s = socket.create_connection((HOST, PORT), 3)
s.settimeout(8)
s.recv(4096)  # greeting

def send(x):
    s.sendall(x.encode())
    out = b""
    while True:
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        if not d:
            break
        out += d
        if b"Content-Length:" in out:
            head_end = out.find(b"\n\n")
            if head_end != -1:
                hdr = out[:head_end].decode("utf-8", "replace")
                cl = 0
                for ln in hdr.splitlines():
                    if ln.lower().startswith("content-length:"):
                        cl = int(ln.split(":", 1)[1].strip())
                if len(out) - head_end - 2 >= cl:
                    return out
    return out

send("auth %s\n\n" % PASS)
time.sleep(0.3)
r = send(cmd)
sys.stdout.write(r.decode("utf-8", "replace"))

