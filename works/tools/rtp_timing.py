#!/usr/bin/env python3
"""Analyze wall-clock timing of RTP packets on a flow in a classic pcap.
Usage: rtp_timing.py <file.pcap> <src:port->dst:port>
"""
import socket
import struct
import sys

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else "10.0.2.15:4000->10.0.2.2"

with open(path, "rb") as f:
    data = f.read()

endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
linktype = struct.unpack(endian + "I", data[20:24])[0]
off = 24
times = []
while off + 16 <= len(data):
    ts_sec, ts_usec, incl_len, _ = struct.unpack(endian + "IIII", data[off:off+16])
    off += 16
    pkt = data[off:off+incl_len]
    off += incl_len
    ip = pkt[14:] if linktype == 1 else pkt
    if len(ip) < 20 or (ip[0] >> 4) != 4 or ip[9] != 17:
        continue
    ihl = (ip[0] & 0x0F) * 4
    src = socket.inet_ntoa(ip[12:16])
    dst = socket.inet_ntoa(ip[16:20])
    sport, dport = struct.unpack(">HH", ip[ihl:ihl+4])
    if "%s:%d->%s" % (src, sport, dst) != want:
        continue
    udp = ip[ihl:]
    rtp = udp[8:]
    if len(rtp) < 12:
        continue
    t = ts_sec + ts_usec / 1e6
    times.append(t)

if not times:
    print("no packets")
    sys.exit(0)

t0 = times[0]
t1 = times[-1]
span = t1 - t0
n = len(times)
print("packets=%d span=%.3fs rate=%.2f pps (expected ~50 for 20ms)" % (n, span, n/span if span else 0))
gaps = [times[i+1]-times[i] for i in range(n-1)]
gaps_ms = [g*1000 for g in gaps]
print("gap ms: min=%.1f max=%.1f avg=%.2f median=%.1f" % (
    min(gaps_ms), max(gaps_ms), sum(gaps_ms)/len(gaps_ms),
    sorted(gaps_ms)[len(gaps_ms)//2]))
print("gaps > 50ms:", [round(g,1) for g in gaps_ms if g > 50][:20])
print("gaps < 5ms:", [round(g,1) for g in gaps_ms if g < 5][:20])
