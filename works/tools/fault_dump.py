#!/usr/bin/env python3
"""Connect to QEMU HMP monitor, pause, dump Cortex-M33 fault registers."""
import socket
import time

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
            if not d:
                break
            buf += d
        except BlockingIOError:
            time.sleep(0.03)
        except socket.timeout:
            break

drain(1.0)
s.sendall(b'stop\n')
time.sleep(0.3)
drain(0.3)
s.sendall(b'xp /4 0xE000ED28\n')   # CFSR, HFSR
time.sleep(0.4)
drain(0.5)
s.sendall(b'xp /4 0xE000ED34\n')   # MMFAR, BFAR
time.sleep(0.4)
drain(0.5)
s.sendall(b'cont\n')
time.sleep(0.2)
drain(0.2)
s.close()
print(buf.decode(errors='replace'))
