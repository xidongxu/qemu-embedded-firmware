#!/usr/bin/env python3
"""Analyze the QEMU tap0 pcap: find guest(172.16.23.50)->FS(172.16.23.1:5061)
TLS records and any RST/FIN to understand why the INVITE never reaches Sofia."""
import struct
import sys
from collections import defaultdict

pcap = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\xidon\code\github\qemu-embedded-firmware\works\logs\tap.pcap"

data = open(pcap, "rb").read()
if data[:4] != b"\xd4\xc3\xb2\xa1" and data[:4] != b"\xa1\xb2\xc3\xd4":
    print("not a classic pcap")
    sys.exit(1)
endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
off = 24
pkts = []
while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from(endian + "IIII", data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    pkts.append((ts_sec, ts_usec, pkt))

# Ethernet: 14 bytes, ethertype 0x0800
# IPv4: version/IHL, ...
print(f"total packets: {len(pkts)}")
conn = defaultdict(lambda: {"a2b": 0, "b2a": 0, "flags": [], "bytes_a2b": 0, "bytes_b2a": 0})

def parse(pkt):
    if len(pkt) < 34:
        return None
    eth_type = struct.unpack("!H", pkt[12:14])[0]
    if eth_type != 0x0800:
        return None
    ihl = (pkt[14] & 0x0F) * 4
    if ihl < 20 or len(pkt) < 14 + ihl + 20:
        return None
    proto = pkt[14 + 9]
    if proto != 6:  # TCP only
        return None
    src = ".".join(str(b) for b in pkt[14 + 12:14 + 16])
    dst = ".".join(str(b) for b in pkt[14 + 16:14 + 20])
    sport, dport = struct.unpack("!HH", pkt[14 + ihl:14 + ihl + 4])
    flags = pkt[14 + ihl + 13]
    tcp_len = len(pkt) - 14 - ihl
    payload = tcp_len - 20
    return src, sport, dst, dport, flags, payload

# list interesting flows
for ts, _, pkt in pkts:
    r = parse(pkt)
    if not r:
        continue
    src, sport, dst, dport, flags, payload = r
    if (src == "172.16.23.50" and dport == 5061) or (dst == "172.16.23.50" and sport == 5061):
        key = (sport if src == "172.16.23.50" else dport)
        side = "guest->FS" if src == "172.16.23.50" else "FS->guest"
        c = conn[key]
        c["flags"].append(flags)
        if side == "guest->FS":
            c["a2b"] += 1
            c["bytes_a2b"] += payload
        else:
            c["b2a"] += 1
            c["bytes_b2a"] += payload

print("\n=== 5061 flows (guest source port) ===")
for k, c in sorted(conn.items()):
    fin = any(f & 0x01 for f in c["flags"])
    rst = any(f & 0x04 for f in c["flags"])
    print(f"srcport={k} guest->FS pkts={c['a2b']} bytes={c['bytes_a2b']} | "
          f"FS->guest pkts={c['b2a']} bytes={c['bytes_b2a']} | FIN={fin} RST={rst}")

# dump TLS record sizes guest->5061 for the main INVITE connection
print("\n=== guest->5061 TLS record sizes (per conn, first 20) ===")
for k, c in sorted(conn.items()):
    sizes = []
    for ts, _, pkt in pkts:
        r = parse(pkt)
        if not r:
            continue
        src, sport, dst, dport, flags, payload = r
        if src == "172.16.23.50" and dport == 5061 and sport == k and payload > 0:
            sizes.append((ts, payload))
    if sizes:
        big = [s for s in sizes if s[1] > 900]
        print(f"conn {k}: total TLS segs={len(sizes)} big(>900B)={len(big)}")
        for s in big[:10]:
            print(f"   t={s[0]} payload={s[1]}")
