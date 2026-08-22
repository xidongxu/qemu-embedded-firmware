# rtp_probe.py - send minimal G.711 (PCMU) RTP packets from the host to a
# QEMU hostfwd'd port, to prove host->guest RTP reachability independent of
# pjsua.  Usage: python rtp_probe.py [port=4000] [duration_s=3]
import socket, struct, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
dur  = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq = 1000
ts  = 0
ssrc = 0xAAAA1234
payload = b'\x80' * 160          # 160 bytes silence (u-law 0x80)
end = time.time() + dur
n = 0
while time.time() < end:
    hdr = struct.pack('>BBHII', 0x80, 0, seq & 0xFFFF,
                      ts & 0xFFFFFFFF, ssrc)
    s.sendto(hdr + payload, ('127.0.0.1', port))
    seq += 1
    ts += 160
    n += 1
    time.sleep(0.01)              # 100 pkt/s = 10ms cadence
print(f"rtp_probe: sent {n} RTP pkts to 127.0.0.1:{port}")
