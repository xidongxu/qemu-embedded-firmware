"""
phone_udp.py - drive the guest phone over the slirp UDP command channel.

Sends commands to 127.0.0.1:15000 (hostfwd -> guest phone_net_task) and
prints the UDP response.

Usage:
  python phone_udp.py --cmd status
  python phone_udp.py --cmd "dial 1007" --poll 3 --polls 10   # dial + watch
  python phone_udp.py --cmd hangup
"""
import socket, time, argparse

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cmd', action='append', default=['status'])
    ap.add_argument('--poll', type=float, default=0, help='re-poll status every N sec')
    ap.add_argument('--polls', type=int, default=0, help='how many status polls')
    ap.add_argument('--port', type=int, default=15000)
    ap.add_argument('--timeout', type=float, default=3)
    args = ap.parse_args()

    def send(cmd):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(args.timeout)
        try:
            s.sendto(cmd.encode(), ('127.0.0.1', args.port))
            data, _ = s.recvfrom(4096)
            print(f'[{cmd}] -> {data.decode("utf-8", "replace")}', flush=True)
        except socket.timeout:
            print(f'[{cmd}] -> TIMEOUT', flush=True)
        finally:
            s.close()

    for c in args.cmd:
        send(c)
    for _ in range(args.polls):
        time.sleep(args.poll)
        send('stat')

if __name__ == '__main__':
    main()
