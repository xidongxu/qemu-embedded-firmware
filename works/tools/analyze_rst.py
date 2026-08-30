#!/usr/bin/env python3
"""Show RST/FIN timing on the 5061 flows relative to the INVITE (big segments)."""
import struct
import sys

pcap = sys.argv[1]
data = open(pcap, "rb").read()
endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
off = 24
events = []
t0 = None
while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, _ = struct.unpack_from(endian + "IIII", data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if len(pkt) < 34:
        continue
    if struct.unpack("!H", pkt[12:14])[0] != 0x0800:
        continue
    ihl = (pkt[14] & 0x0F) * 4
    if len(pkt) < 14 + ihl + 20:
        continue
    if pkt[14 + 9] != 6:
        continue
    src = ".".join(str(b) for b in pkt[14 + 12:14 + 16])
    dst = ".".join(str(b) for b in pkt[14 + 16:14 + 20])
    sport, dport = struct.unpack("!HH", pkt[14 + ihl:14 + ihl + 4])
    flags = pkt[14 + ihl + 13]
    seq = struct.unpack("!I", pkt[14 + ihl + 4:14 + ihl + 8])[0]
    tcp_len = len(pkt) - 14 - ihl
    payload = tcp_len - 20
    if not ((src == "172.16.23.50" and dport == 5061) or (dst == "172.16.23.50" and sport == 5061)):
        continue
    ts = ts_sec + ts_usec / 1e6
    if t0 is None:
        t0 = ts
    rel = ts - t0
    d = "G->F" if src == "172.16.23.50" else "F->G"
    port = sport if src == "172.16.23.50" else dport
    fstr = ""
    if flags & 0x02: fstr += "S"
    if flags & 0x10: fstr += "A"
    if flags & 0x08: fstr += "P"
    if flags & 0x01: fstr += "F"
    if flags & 0x04: fstr += "R"
    if payload > 0 or "R" in fstr or "F" in fstr or "S" in fstr:
        events.append(f"t={rel:8.3f} {d} port={port} flags={fstr:4s} payload={payload}")

for e in events:
    print(e)
