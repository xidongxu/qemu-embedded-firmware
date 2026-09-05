#!/usr/bin/env python3
"""Compile-level coverage across all Cortex-M cores exposed by the boards.

For every core in the board matrix this:
  1. syntax-checks tracer.c + tracer_crash_store.c with the full feature set
     (CRASH+LOG, TRACER_PUTCHAR) -- pure C, no linker symbols needed.
  2. assembles tracer_gnugcc.s for MAIN-profile cores (M3/M4/M7/M33/M55/M85).

M23 (armv8-M Baseline) is intentionally SKIPPED for step 2: tracer_gnugcc.s
saves r4..r11 with `push {r4-r11}`, which is a MAIN-profile instruction; a
runtime fault-dump entry for M23 would need a dedicated baseline port.

Environment: CC (default arm-none-eabi-gcc). Exits non-zero on failure.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARDS = os.path.join(ROOT, "qemu-tests")
TRACER = ROOT
COMMON = os.path.join(BOARDS, "application")

MAIN_PROFILE = ("cortex-m3", "cortex-m4", "cortex-m7",
                "cortex-m33", "cortex-m55", "cortex-m85")
BASELINE_ONLY = ("cortex-m23",)


def cc():
    return os.environ.get("CC", "arm-none-eabi-gcc")


def collect_cores():
    cores = []
    for d in sorted(os.listdir(BOARDS)):
        if d.startswith("_"):
            continue
        cfg = os.path.join(BOARDS, d, "config.json")
        if os.path.exists(cfg):
            with open(cfg, encoding="utf-8") as f:
                core = json.load(f).get("core")
            if core and core not in cores:
                cores.append(core)
    return cores


def flags(core):
    return ["-mcpu=%s" % core, "-mthumb", "-mfloat-abi=soft", "-std=c99",
            "-Wall", "-I", TRACER, "-I", COMMON,
            "-include", os.path.join(COMMON, "uart.h"),
            "-DTRACER_PUTCHAR=board_putc", "-DBOARD_UART0=0x40004000",
            "-DTRACER_USE_CRASH=1", "-DTRACER_USE_LOG=1"]


def main():
    cores = collect_cores()
    # M23 (no QEMU board, baseline profile) is kept in the compile matrix for
    # the C layer even though the fault-entry asm is MAIN-only.
    if "cortex-m23" not in cores:
        cores.append("cortex-m23")
    cores.sort()
    failed = []
    for core in cores:
        # 1) C syntax check
        cmd = ([cc()] + flags(core) + ["-fsyntax-only",
               os.path.join(TRACER, "tracer.c"),
               os.path.join(TRACER, "tracer_crash_store.c")])
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
        ok = r.returncode == 0
        print("%s  %-12s fsyntax tracer.c + crash_store" %
              ("PASS" if ok else "FAIL", core))
        if not ok:
            failed.append(core)
            sys.stderr.write(r.stdout)
            continue

        # 2) assemble the fault entry (main profile only)
        if core in BASELINE_ONLY:
            print("SKIP  %-12s asm entry (M23 baseline: needs dedicated port)"
                  % core)
            continue
        cmd = ([cc()] + flags(core) + ["-c",
               os.path.join(TRACER, "tracer_gnugcc.s"), "-o",
               os.devnull])
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
        ok = r.returncode == 0
        print("%s  %-12s assemble tracer_gnugcc.s" %
              ("PASS" if ok else "FAIL", core))
        if not ok:
            failed.append(core)
            sys.stderr.write(r.stdout)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
