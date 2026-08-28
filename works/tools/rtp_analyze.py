#!/usr/bin/env python3
"""Parse RTP packets from a classic pcap and analyze sequence/timestamp
step on a specific flow (e.g. guest -> FS RTP).
Usage: parse_pcap.py <file.pcap> [srcip:sport->dstip:dport]
"""
import socket
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "rtp_dump.pcap"
want = sys.argv[2] if len(sys.argv) > 2 else "10.0.2.15:4000->10.0.2.2"

with open(path, "rb") as f:
    data = f.read()

magic = data[:4]
endian = "<" if magic == b"\xd4\xc3\xb2\xa1" else ">"
linktype = struct.unpack(endian + "I", data[20:24])[0]

off = 24
pkts = []
while off + 16 <= len(data):
    _, _, incl_len, _ = struct.unpack(endian + "IIII", data[off:off+16])
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
    if "%s:%d->%s" % (src, sport, dst) not in want and want not in ("*", ""):
        continue
    udp = ip[ihl:]
    rtp = udp[8:]  # skip UDP header
    if len(rtp) < 12:
        continue
    b0 = rtp[0]
    pt = b0 & 0x7F
    if pt > 100:  # not audio rtp (telephone-event could be 101)
        pass
    seq = struct.unpack(">H", rtp[2:4])[0]
    ts = struct.unpack(">I", rtp[4:8])[0]
    ssrc = struct.unpack(">I", rtp[8:12])[0]
    pkts.append((pt, seq, ts, ssrc))

if not pkts:
    print("no matching RTP packets")
    sys.exit(0)

print("total RTP pkts=%d" % len(pkts))
# Show first 12 and last 8
print("\n idx  pt   seq    timestamp     dseq   dts   ssrc")
prev = None
for i, (pt, seq, ts, ssrc) in enumerate(pkts):
    dseq = seq - prev[1] if prev else 0
    dts = ts - prev[2] if prev else 0
    if i < 12 or i >= len(pkts) - 8:
        print("%4d %3d %6d %11d %6d %6d %d" % (i, pt, seq, ts, dseq, dts, ssrc))
    prev = (pt, seq, ts, ssrc)

# stats
seqs = [p[1] for p in pkts]
tss = [p[2] for p in pkts]
dseqs = [seqs[i+1]-seqs[i] for i in range(len(seqs)-1)]
dtss = [tss[i+1]-tss[i] for i in range(len(tss)-1)]
print("\nseq gaps (non +1):", [g for g in dseqs if g != 1][:20])
print("ts deltas unique:", sorted(set(dtss))[:20])
print("ts min/max:", min(dtss), max(dtss))
