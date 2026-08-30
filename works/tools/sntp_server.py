#!/usr/bin/env python3
"""Minimal SNTP v4 server for the QEMU guest boot-time sync.

The guest's lwIP SNTP client (application/sntp_sync.c) points at the host
tap0 address 172.16.23.1 and uses the non-standard port 12345 (see
SNTP_PORT in libutils/lwip/ports/lwipopts.h) - no Administrator needed and
no clash with the Windows time service on UDP :123.

Run this on the host BEFORE booting QEMU so the guest gets the real
wall-clock time on first poll:
    python works\\tools\\sntp_server.py
"""
import socket
import struct
import time

SNTP_PORT = 12345
NTP_EPOCH_OFFSET = 2208988800  # seconds between 1900-01-01 and 1970-01-01


def to_ntp_ts(t):
    sec = int(t)
    frac = int((t - sec) * (1 << 32))
    return struct.pack(">II", sec & 0xFFFFFFFF, frac & 0xFFFFFFFF)


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", SNTP_PORT))
    print("SNTP server listening on UDP :%d (Ctrl+C to stop)" % SNTP_PORT)
    while True:
        data, addr = sock.recvfrom(512)
        if len(data) < 48:
            continue
        # Client's transmit timestamp (bytes 40..47) goes back as originate.
        originate = data[40:48]
        now = time.time() + NTP_EPOCH_OFFSET
        print("SNTP request from %s:%d -> replied" % (addr[0], addr[1]),
              flush=True)

        resp = bytearray(48)
        resp[0] = 0x24      # LI=0 (no warning), VN=4, Mode=4 (server)
        resp[1] = 1         # stratum 1 (primary)
        resp[2] = 6         # poll interval 2^6 = 64 s
        resp[3] = 0xEC      # precision ~ 2^-20 s
        # root delay = 0, root dispersion = 0
        resp[12:16] = b"LOCL"
        resp[16:24] = to_ntp_ts(now)   # reference timestamp
        resp[24:32] = originate        # originate timestamp
        resp[32:40] = to_ntp_ts(now)   # receive timestamp
        resp[40:48] = to_ntp_ts(now)   # transmit timestamp
        sock.sendto(bytes(resp), addr)


if __name__ == "__main__":
    main()
