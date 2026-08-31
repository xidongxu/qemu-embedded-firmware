#!/usr/bin/env python3
"""Connect to QEMU HMP, stop, dump PSP/MSP + the exception frame memory."""
import socket, time

HOST, PORT = '127.0.0.1', 4444
s = socket.create_connection((HOST, PORT), timeout=3)
s.setblocking(False)
buf = b''
def drain(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            buf += d
        except BlockingIOError:
            time.sleep(0.03)

drain(1.0)
s.sendall(b'stop\n'); time.sleep(0.3); drain(0.3)
s.sendall(b'info registers\n'); time.sleep(0.5); drain(0.5)
s.sendall(b'cont\n'); time.sleep(0.2); drain(0.2)
s.close()
txt = buf.decode(errors='replace')
# print register lines containing PSP/MSP/SP and R13
for ln in txt.splitlines():
    t = ln.strip()
    if any(k in t for k in ('MSP','PSP','SP','R13','R15','PC','XPSR','xPSR')):
        print(t)
