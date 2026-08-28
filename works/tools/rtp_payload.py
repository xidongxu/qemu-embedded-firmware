#!/usr/bin/env python3
"""Compare RTP payloads between guest->FS and FS->guest directions in a pcap.
If FS echoes, FS->guest payload should contain the same audio content as
guest->FS (non-silence with 1kHz tone). If FS is NOT echoing (generating its
own silence), FS->guest payload will be constant mu-law silence (0xff).
Usage: rtp_payload.py <file.pcap>
"""
import socket
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "echo_dump.pcap"

with open(path, "rb") as f:
    data = f.read()

endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
linktype = struct.unpack(endian + "I", data[20:24])[0]
off = 24
dirs = {"g2f": [], "f2g": []}
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
    udp = ip[ihl:]
    rtp = udp[8:]
    if len(rtp) < 12:
        continue
    pt = rtp[0] & 0x7F
    if pt != 0:  # only PCMU audio RTP
        continue
    if src == "10.0.2.15" and sport == 4000:
        dirs["g2f"].append(rtp[12:])
    elif dst == "10.0.2.15" and dport == 4000:
        dirs["f2g"].append(rtp[12:])

def stats(payloads, name):
    if not payloads:
        print("%s: NO packets" % name)
        return
    print("%s: %d packets" % (name, len(payloads)))
    # sample up to 40 payloads
    uniq_counts = []
    rms_samples = []
    for p in payloads[:40]:
        b = p
        if not b:
            continue
        uniq_counts.append(len(set(b)))
        # mu-law: decode to pcm roughly, compute variance
        vals = []
        for x in b:
            # mu-law decode approx
            x = x & 0xFF
            sign = 1 if (x & 0x80) else -1
            mag = ((~x) & 0x7F) if False else ((x & 0x7F))
            # simple: use raw byte spread
            vals.append(x)
        mean = sum(vals) / len(vals)
        var = sum((v - mean) ** 2 for v in vals) / len(vals)
        rms_samples.append(var ** 0.5)
    print("  unique-bytes/pkt: min=%d max=%d avg=%.1f" % (
        min(uniq_counts), max(uniq_counts), sum(uniq_counts)/len(uniq_counts)))
    print("  byte-RMS: min=%.1f max=%.1f avg=%.1f" % (
        min(rms_samples), max(rms_samples), sum(rms_samples)/len(rms_samples)))
    # constant payload?
    if all(u == 1 for u in uniq_counts):
        print("  => CONSTANT payload (silence / not echoing)")
    else:
        print("  => VARIED payload (has real audio content)")

stats(dirs["g2f"], "guest->FS")
stats(dirs["f2g"], "FS->guest")

# Show first 16 bytes of a few FS->guest payloads to see if constant
print("\nFS->guest first payloads (first 16 bytes):")
for i, p in enumerate(dirs["f2g"][:6]):
    print("  %d: %s" % (i, p[:16].hex()))
print("\nguest->FS first payloads (first 16 bytes):")
for i, p in enumerate(dirs["g2f"][:6]):
    print("  %d: %s" % (i, p[:16].hex()))
