#!/usr/bin/env python3
"""Extract SIP messages from a pcap and show the call flow (INVITE/OK/BYE),
with direction, to determine who hangs up.
Usage: sip_flow.py <file.pcap>
"""
import socket
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "bye_dump.pcap"

with open(path, "rb") as f:
    data = f.read()

endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
linktype = struct.unpack(endian + "I", data[20:24])[0]
off = 24
msgs = []
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
    udp = ip[ihl:]
    payload = udp[8:]
    if not (sport == 5060 or dport == 5060 or sport == 15062 or dport == 15062):
        continue
    if len(payload) < 4:
        continue
    text = payload.decode("utf-8", "replace")
    first = text.splitlines()[0] if text else ""
    if first.startswith(("INVITE", "ACK", "BYE", "CANCEL", "REGISTER", "OPTIONS", "200", "180", "183", "100", "401", "403", "486", "487", "603", "SIP/2.0")):
        t = ts_sec + ts_usec / 1e6
        msgs.append((t, src, sport, dst, dport, first[:60]))

msgs.sort(key=lambda x: x[0])
if not msgs:
    print("no SIP messages found")
    sys.exit(0)

t0 = msgs[0][0]
for t, src, sport, dst, dport, first in msgs:
    rel = t - t0
    who = "guest->FS" if src == "10.0.2.15" else "FS->guest"
    print("%8.3f  %-9s  %s" % (rel, who, first))

# summarize BYE / final
print("\n-- call-ending messages --")
for t, src, sport, dst, dport, first in msgs:
    if first.startswith(("BYE", "487", "603", "200")):
        who = "guest->FS" if src == "10.0.2.15" else "FS->guest"
        print("%8.3f  %-9s  %s" % (t - t0, who, first))
