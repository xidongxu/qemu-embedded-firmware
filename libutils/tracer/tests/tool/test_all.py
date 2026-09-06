#!/usr/bin/env python3
"""Run every enabled (board, case) matrix entry; skip boards whose QEMU
machine is not available on this host.  Exits 0 only if all ran PASS.

Usage:
    python tests/tool/test_all.py [--build-only] [--boards m3-an385 m33-an505 ...]

Environment: CC, QEMU (see board_test.py)
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOARDS = os.path.join(ROOT, "tests", "qemu")
HERE = os.path.dirname(os.path.abspath(__file__))

# Reuse the shared per-case table (descriptions + FPU/AUTO_RESET metadata)
# so a board without an explicit "cases" list runs every scenario.
sys.path.insert(0, HERE)
from board_test import DEFAULT_CASES  # noqa: E402


def qemu_bin():
    return os.environ.get("QEMU", "qemu-system-arm")


def qemu_has_machine(name):
    try:
        p = subprocess.run([qemu_bin(), "-machine", "help"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=20)
    except Exception:
        return False
    return ("\n%s " % name) in p.stdout or name in p.stdout


def list_boards():
    out = []
    for d in sorted(os.listdir(BOARDS)):
        if d.startswith("_"):
            continue
        if os.path.exists(os.path.join(BOARDS, d, "config.json")):
            out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--boards", nargs="*", default=None)
    ap.add_argument("--skip-run", action="store_true",
                    help="compile every board/case but do not run QEMU")
    args = ap.parse_args()

    boards = args.boards or list_boards()
    results = []
    for b in boards:
        with open(os.path.join(BOARDS, b, "config.json"), encoding="utf-8") as f:
            cfg = json.load(f)
        machine_ok = bool(cfg.get("machine")) and qemu_has_machine(cfg["machine"])
        if not machine_ok:
            if not cfg.get("build_only"):
                print("SKIP  %-10s machine '%s' not available" % (b, cfg.get("machine", "-")))
                continue
            # build_only board without machine: fall through to compile-level
        for case in sorted((cfg.get("cases") or DEFAULT_CASES)):
            cmd = [sys.executable, os.path.join(HERE, "board_test.py"), b, case]
            if args.skip_run or args.build_only or not machine_ok:
                cmd.append("--build-only")
            r = subprocess.run(cmd)
            results.append((b, case, r.returncode == 0))
            if not args.build_only and not args.skip_run and r.returncode != 0:
                pass  # keep going to report the full matrix

    failed = [x for x in results if not x[2]]
    ran = len(results)
    print("\n== matrix: %d run, %d failed ==" % (ran, len(failed)))
    for b, c, ok in results:
        if not ok:
            print("  FAIL %s tc%s" % (b, c))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
