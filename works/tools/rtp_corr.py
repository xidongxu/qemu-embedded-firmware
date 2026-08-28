#!/usr/bin/env python3
"""Cross-correlate guest->FS vs FS->guest RTP payloads to determine whether
FreeSWITCH actually echoes the guest's audio (1kHz tone) back, or whether
the FS->guest stream is just FreeSWITCH's own comfort noise.
Usage: rtp_corr.py <file.pcap>
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
g2f, f2g = [], []
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
    if len(rtp) < 12 or (rtp[0] & 0x7F) != 0:
        continue
    if src == "10.0.2.15" and sport == 4000:
        g2f.append(rtp[12:])
    elif dst == "10.0.2.15" and dport == 4000:
        f2g.append(rtp[12:])

def ulaw_decode(b):
    out = []
    for x in b:
        x = (~x) & 0xFF
        sign = 1 if (x & 0x80) else -1
        exp = (x >> 4) & 0x07
        mant = x & 0x0F
        sample = ((mant << 3) + 0x84) << exp
        out.append(sign * (sample - 0x84))
    return out

def autocorr_lag8(pcm, name):
    """Check periodicity at lag 8 (1kHz @ 8kHz)."""
    if len(pcm) < 32:
        return
    vals = pcm[:160]
    mean = sum(vals)/len(vals)
    num = sum((vals[i]-mean)*(vals[i+8]-mean) for i in range(len(vals)-8))
    den = sum((v-mean)**2 for v in vals)
    lag8 = num/den if den else 0
    print("%s: lag-8 autocorr = %.3f (%s)" % (
        name, lag8, "~1kHz periodic" if lag8 > 0.5 else "not 1kHz"))

def cross_corr(g, f, name):
    """Cross correlate G and F over a few time offsets, report max corr."""
    ga = g[:160]
    fa = f[:160]
    gmean = sum(ga)/len(ga)
    fmean = sum(fa)/len(fa)
    gd = [x-gmean for x in ga]
    fd = [x-fmean for x in fa]
    gnorm = sum(x*x for x in gd) ** 0.5
    fnorm = sum(x*x for x in fd) ** 0.5
    if gnorm == 0 or fnorm == 0:
        print("%s: zero energy" % name)
        return
    # compare same-index (echo at same packet) and shifted by up to 10 packets
    best = 0
    for shift in range(-10, 11):
        num = sum(gd[i]*fd[i+shift] for i in range(max(0,-shift), min(len(gd), len(fd)-shift)))
        den = gnorm*fnorm
        c = num/den if den else 0
        if abs(c) > abs(best):
            best = c
    print("%s: max cross-corr = %.3f (%s)" % (
        name, best, "ECHO (strongly related)" if abs(best) > 0.5 else "not echo (independent)"))
    print("    lag-8 autocorr: guest->FS=%.3f  FS->guest=%.3f" % (
        autocorr_val(g), autocorr_val(f)))

def autocorr_val(pcm):
    vals = pcm[:160]
    mean = sum(vals)/len(vals)
    num = sum((vals[i]-mean)*(vals[i+8]-mean) for i in range(len(vals)-8))
    den = sum((v-mean)**2 for v in vals)
    return num/den if den else 0

print("guest->FS pkts=%d, FS->guest pkts=%d" % (len(g2f), len(f2g)))
if g2f and f2g:
    # find a non-silent pair by index
    for idx in range(min(len(g2f), len(f2g))):
        g = ulaw_decode(g2f[idx])
        f = ulaw_decode(f2g[idx])
        if max(abs(v) for v in g) > 500 and max(abs(v) for v in f) > 500:
            cross_corr(g, f, "packet #%d" % idx)
            break
    # also a later packet
    for idx in range(min(len(g2f), len(f2g)) - 1, max(0, min(len(g2f), len(f2g)) - 50), -1):
        g = ulaw_decode(g2f[idx])
        f = ulaw_decode(f2g[idx])
        if max(abs(v) for v in g) > 500 and max(abs(v) for v in f) > 500:
            cross_corr(g, f, "packet #%d" % idx)
            break
else:
    print("insufficient data")
