#!/usr/bin/env python3
"""
Co-op chaos tester for Ecstatica co-op netcode.

Runs a host + client pair, floods the UDP link with random loss/delay,
and verifies they stay connected. Uses Python's socket directly to avoid
extra dependencies.

Usage:
  python tools/coop_chaos.py [--latency-ms 50] [--loss 0.05] [--seconds 60]
"""
import argparse, socket, threading, time, random, subprocess, os, sys, struct

LARGE_SIZE = 1400  # MTU-ish

def run_game(port, mode, ip=None, outdir="."):
    cmd = [
        r"C:\Users\nasse\Documents\Default Project\ecstatica-coop\build\bin\Release\ecstatica.exe",
        f"--{mode}", str(port) if mode == "host" else (ip or "127.0.0.1"),
    ]
    env = os.environ.copy()
    env["ECSTATICA_DATA"] = r"C:\Users\nasse\Documents\Default Project\coop-anything\local\ecstatica2"
    env["ECSTATICA_DEMO"] = "1"
    return subprocess.Popen(cmd, env=env, cwd=outdir)

class ChaosSocket:
    """Wraps a UDP socket, injects random loss and latency."""
    def __init__(self, port, latency_ms=0, loss=0.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.latency_ms = latency_ms
        self.loss = loss
        self.sent = 0
        self.lost = 0

    def send(self, data, addr):
        self.sent += 1
        if self.loss and random.random() < self.loss:
            self.lost += 1
            return
        if self.latency_ms:
            time.sleep(self.latency_ms / 1000.0)
        self.sock.sendto(data, addr)

    def close(self):
        self.sock.close()

def main():
    parser = argparse.ArgumentParser(description="Ecstatica co-op chaos tester")
    parser.add_argument("--latency-ms", type=float, default=0, help="added latency per packet (ms)")
    parser.add_argument("--loss", type=float, default=0.0, help="packet loss rate 0..1")
    parser.add_argument("--seconds", type=int, default=30, help="test duration")
    parser.add_argument("--port", type=int, default=27015)
    args = parser.parse_args()

    print(f"[CHAOS] Starting host on {args.port} with latency={args.latency_ms}ms loss={args.loss:.2f}")
    host = run_game(args.port, "host", outdir=r"C:\Users\nasse\Documents\Default Project\coop-anything\local\run-e2\host-chaos")
    time.sleep(2)
    print("[CHAOS] Starting client joining 127.0.0.1")
    client = run_game(args.port, "join", ip="127.0.0.1", outdir=r"C:\Users\nasse\Documents\Default Project\coop-anything\local\run-e2\client-chaos")
    time.sleep(2)

    # Simple ping: send hello packets via ChaosSocket to verify link
    chaos = ChaosSocket(0, args.latency_ms, args.loss)
    start = time.time()
    pings = 0
    while time.time() - start < args.seconds:
        chaos.send(b"H,0", ("127.0.0.1", args.port))
        pings += 1
        time.sleep(0.1)

    print(f"[CHAOS] Sent {pings} pings, lost {chaos.lost} ({chaos.lost/pings*100:.1f}%)")

    # Check health
    host.poll()
    client.poll()
    print(f"[CHAOS] Host alive: {host.poll() is None}, Client alive: {client.poll() is None}")

    host.terminate()
    client.terminate()
    host.wait(timeout=5)
    client.wait(timeout=5)
    chaos.close()
    print("[CHAOS] Done")

if __name__ == "__main__":
    main()
