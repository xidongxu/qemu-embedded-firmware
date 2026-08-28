#!/usr/bin/env python3
"""Parse a classic pcap (filter-dump output) and summarize UDP flows,
highlighting the RTP (guest 10.0.2.15:4000) traffic direction/target.
Usage: parse_pcap.py <file.pcap>
"""
import socket
import struct
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "rtp_dump.pcap"

with open(path, "rb") as f:
    data = f.read()

# Global header
magic = data[:4]
if magic == b"\xd4\xc3\xb2\xa1":
    endian = "<"
elif magic == b"\xa1\xb2\xc3\xd4":
    endian = ">"
else:
    print("Not a classic pcap (magic=%r)" % magic)
    sys.exit(1)

snaplen = struct.unpack(endian + "I", data[16:20])[0]
linktype = struct.unpack(endian + "I", data[20:24])[0]
print("linktype=%d snaplen=%d total_bytes=%d" % (linktype, snaplen, len(data)))

off = 24
flows = Counter()
samples = {}
n = 0
while off + 16 <= len(data):
    ts_sec, ts_usec, incl_len, orig_len = struct.unpack(endian + "IIII", data[off:off+16])
    off += 16
    pkt = data[off:off+incl_len]
    off += incl_len
    n += 1
    if linktype == 1:  # Ethernet
        if len(pkt) < 14:
            continue
        eth_type = struct.unpack(">H", pkt[12:14])[0]
        ip = pkt[14:]
    else:
        ip = pkt
    if len(ip) < 20:
        continue
    if (ip[0] >> 4) != 4:
        continue
    ihl = (ip[0] & 0x0F) * 4
    proto = ip[9]
    if proto != 17:  # UDP
        continue
    src = socket.inet_ntoa(ip[12:16])
    dst = socket.inet_ntoa(ip[16:20])
    if len(ip) < ihl + 8:
        continue
    sport, dport = struct.unpack(">HH", ip[ihl:ihl+4])
    key = "%s:%d -> %s:%d" % (src, sport, dst, dport)
    flows[key] += 1
    if key not in samples:
        samples[key] = pkt

print("total packets=%d" % n)
print("\n-- UDP flows (count) --")
for k, c in flows.most_common():
    print("%6d  %s" % (c, k))

# Detail RTP-related flows
print("\n-- RTP/RTCP detail (guest side ports 4000/4001) --")
for k, c in flows.most_common():
    if "4000" in k or "4001" in k or (":4000" in k):
        print("%6d  %s" % (c, k))
