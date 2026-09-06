#!/usr/bin/env python3
"""Build one board test firmware and run it under QEMU, asserting markers.

Usage:
    python tests/tool/board_test.py <board> <case> [--build-only] [--keep]

  <board>  board directory name under tests/qemu/ (must contain config.json)
  <case>   test case id from config.json "cases" (e.g. 0,1,2)

Environment:
    CC      arm-none-eabi C compiler (default: arm-none-eabi-gcc)
    QEMU    qemu-system-arm (default: qemu-system-arm)

Exits 0 on PASS (all expected markers seen), non-zero otherwise.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOARDS = os.path.join(ROOT, "tests", "qemu")
COMMON = os.path.join(BOARDS, "application")
TRACER = ROOT
BUILD_ROOT = os.path.join(ROOT, "build")

# Shared test scenarios (a board config may override per-case expectations).
# Extra per-case keys:  "macros": [...]  extra -D defines for this case
# (e.g. TRACER_AUTO_RESET_MS), and  "fpu": true  -> build the case with the
# board's FPU ISA (config "fpu") under the softfp ABI.  A case that needs an
# FPU on a board without one (config "fpu" missing) is SKIPPED.
DEFAULT_CASES = {
    "0": {"desc": "smoke/callstack",
          "expect": ["Tracer: Cortex-M fault dump ready",
                     "PASS callstack", "PASS smoke"]},
    "1": {"desc": "usage fault",
          "expect": ["trigger: unaligned access", "Fault Dump", "UsageFault"]},
    "2": {"desc": "bus fault",
          "expect": ["trigger: write", "Fault Dump", "BusFault"]},
    "3": {"desc": "assert",
          "expect": ["Assert Failed", "End of assert"]},
    "4": {"desc": "PSP (thread-mode) fault",
          "expect": ["trigger: write", "Thread mode, PSP",
                     "BusFault", "Fault Dump"]},
    "5": {"desc": "re-entrancy guard",
          "expect": ["hook: pend NMI inside dump",
                     "fault while dumping, ignored"]},
    "6": {"desc": "auto-reset after dump",
          "macros": ["TRACER_AUTO_RESET_MS=1000"],
          "expect": ["End of dump: reset in 1000 ms",
                     "app: boot (TEST_CASE=6)"]},
    "7": {"desc": "FPU extended frame",
          "fpu": True,
          "expect": ["FPU enabled, touching VFP",
                     "FPU (extended frame):", "FPSCR="]},
    "8": {"desc": "PSP + FPU combined fault",
          "fpu": True,
          "expect": ["FPU enabled, touching VFP", "trigger: write",
                     "Thread mode, PSP", "FPU (extended frame):", "BusFault"]},
    "9": {"desc": "assert re-entrancy inside dump",
          "expect": ["hook: assert inside dump",
                     "assert while dumping, ignored"]},
    "10": {"desc": "assert + auto-reset",
           "macros": ["TRACER_AUTO_RESET_MS=1000"],
           "expect": ["End of assert: reset in 1000 ms",
                      "app: boot (TEST_CASE=10)"]},
}


def cc():
    return os.environ.get("CC", "arm-none-eabi-gcc")


def qemu_bin():
    return os.environ.get("QEMU", "qemu-system-arm")


def load_config(board):
    cfg_path = os.path.join(BOARDS, board, "config.json")
    with open(cfg_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    cfg.setdefault("cases", DEFAULT_CASES)
    return cfg


def board_dir(board):
    return os.path.join(BOARDS, board)


def linker_path(cfg, board):
    """Linker script: board-local linker.ld unless cfg['linker'] overrides
    (path relative to tests/qemu/)."""
    if cfg.get("linker"):
        return os.path.join(BOARDS, cfg["linker"])
    return os.path.join(board_dir(board), "linker.ld")


def case_meta(cfg, tc):
    """Per-case compile overrides derived from the shared case table:
    extra -D macros and, for "fpu" cases, the board's FPU ISA built under
    the softfp ABI.  Returns None when the case needs an FPU the board does
    not provide (config "fpu" missing) -- the caller SKIPs it."""
    case = (cfg.get("cases") or DEFAULT_CASES).get(tc, {})
    meta = {"macros": list(case.get("macros", [])),
            "abi": cfg.get("mfloat", "soft"),
            "fpu": None}
    if case.get("fpu"):
        if not cfg.get("fpu"):
            return None
        meta["abi"] = "softfp"
        meta["fpu"] = cfg["fpu"]
    return meta


def cflags(cfg, board, tc, meta):
    """Common compile flags for every C/asm unit of this board."""
    core = cfg["core"]
    abi = meta["abi"]
    uart_h = os.path.join(COMMON, "uart.h")
    fl = [
        "-mcpu=%s" % core,
        "-mthumb",
        "-mfloat-abi=%s" % abi,
        "-O2", "-g", "-Wall",
        "-ffunction-sections", "-fdata-sections",
        "-I", TRACER,
        "-I", COMMON,
        "-include", uart_h,
        "-DTRACER_PUTCHAR=board_putc",
        "-DBOARD_UART0=%s" % cfg["uart0"],
        "-DTEST_CASE=%s" % tc,
    ]
    for m in cfg.get("tracer_macros", []):
        fl.append("-D%s" % m)
    # The shared app.c prints via tracer_log (synchronous serial), which is
    # compiled in with TRACER_USE_LOG.  Keep CRASH on too so the fault dump
    # captures the recent log/event ring.
    fl.append("-DTRACER_USE_LOG=1")
    # Per-case extras: FPU ISA first so later -D flags stay consistent.
    if meta["fpu"]:
        fl += ["-mfpu=%s" % meta["fpu"]]
    if cfg.get("busfault_addr"):
        fl.append("-DAPP_BUSFAULT_ADDR=%s" % cfg["busfault_addr"])
    for m in meta["macros"]:
        fl.append("-D%s" % m)
    return fl


def run(cmd, **kw):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)
    return proc


def build(board, tc, out_dir, meta):
    cfg = load_config(board)
    board_dir = os.path.join(BOARDS, board)
    os.makedirs(out_dir, exist_ok=True)
    objs = []
    fl = cflags(cfg, board, tc, meta)

    units = [
        ("startup", os.path.join(BOARDS, cfg["startup"]), None),
        ("tracer", os.path.join(TRACER, "tracer.c"), None),
        ("tracer_cs", os.path.join(TRACER, "tracer_crash_store.c"), None),
        ("tracer_asm", os.path.join(TRACER, "tracer_gnugcc.s"), None),
        ("app", os.path.join(COMMON, "app.c"), None),
    ]
    for tag, src, extra in units:
        obj = os.path.join(out_dir, "%s_%s.o" % (tag, board))
        cmd = [cc(), "-c", src, "-o", obj] + fl
        r = run(cmd)
        if r.returncode != 0:
            sys.stderr.write("build FAILED (%s)\n%s\n" % (tag, r.stdout))
            return None
        objs.append(obj)

    elf = os.path.join(out_dir, "%s_tc%s.elf" % (board, tc))
    ld = linker_path(cfg, board)
    # core flags on the LINK command too, so gcc picks the right multilib
    # (Thumb) libgcc/libc -- without -mcpu/-mthumb the default multilib is
    # ARM, which cannot be linked against Thumb objects.
    link_flags = ["-mcpu=%s" % cfg["core"], "-mthumb",
                  "-mfloat-abi=%s" % meta["abi"]]
    if meta["fpu"]:
        link_flags += ["-mfpu=%s" % meta["fpu"]]
    cmd = ([cc(), "-o", elf] + link_flags +
           ["-T", ld, "--specs=nano.specs",
            "-Wl,--gc-sections", "-Wl,-Map=%s.map" % elf] + objs)
    r = run(cmd)
    if r.returncode != 0:
        sys.stderr.write("link FAILED\n%s\n" % r.stdout)
        return None
    return elf


def run_qemu(cfg, elf, expect, timeout=20):
    """Run firmware under QEMU; stop as soon as all expected markers appear
    (or timeout).  Output goes to a temp file which we poll."""
    log_fd, log_path = tempfile.mkstemp(suffix=".log", prefix="tracer_")
    os.close(log_fd)
    cmd = [qemu_bin(), "-machine", cfg["machine"]]
    cmd += cfg.get("qemu_args", ["-display", "none", "-serial", "stdio"])
    serial = cmd.index("-serial")
    if serial >= 0 and serial + 1 < len(cmd):
        cmd[serial + 1] = "file:%s" % log_path
    else:
        cmd += ["-serial", "file:%s" % log_path]
    cmd += ["-kernel", elf]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    out = ""
    start = time.time()
    try:
        while True:
            try:
                with open(log_path, "r", errors="replace") as f:
                    out = f.read()
            except OSError:
                out = ""
            if all(m in out for m in expect):
                break
            if time.time() - start > timeout:
                break
            time.sleep(0.05)
    finally:
        if proc.poll() is None:
            proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
    try:
        os.unlink(log_path)
    except OSError:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("case")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    cfg = load_config(args.board)
    if args.case not in cfg["cases"]:
        sys.stderr.write("unknown case '%s' for board %s\n" % (args.case, args.board))
        return 2

    meta = case_meta(cfg, args.case)
    if meta is None:
        sys.stderr.write("SKIP %s tc%s: case needs an FPU but board "
                         "has no config 'fpu'\n" % (args.board, args.case))
        return 0

    out_dir = os.path.join(BUILD_ROOT, args.board)
    t0 = time.time()
    elf = build(args.board, args.case, out_dir, meta)
    if elf is None:
        return 1
    sys.stderr.write("built %s in %.1fs\n" % (os.path.basename(elf), time.time() - t0))

    if args.build_only:
        return 0

    # Boards without a QEMU machine cannot run (e.g. m85 before a QEMU with
    # mps3-an555, m23 which has no standard QEMU board).
    if not cfg.get("machine"):
        sys.stderr.write("SKIP run %s: no QEMU machine configured\n" % args.board)
        return 0

    expect = cfg["cases"][args.case]["expect"]
    out = run_qemu(cfg, elf, expect)
    missing = [m for m in expect if m not in out]
    if missing:
        sys.stderr.write("FAIL %s tc%s: missing markers %s\n" % (args.board, args.case, missing))
        sys.stderr.write("---- captured output ----\n%s\n" % out)
        return 1
    sys.stderr.write("PASS %s tc%s (%s)\n" % (args.board, args.case, cfg["cases"][args.case]["desc"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
