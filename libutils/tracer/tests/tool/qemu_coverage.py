#!/usr/bin/env python3
"""QEMU-layer line coverage for the library (Zephyr-style gcov export).

The board firmware is built with gcov instrumentation plus GCC's
-fprofile-info-section; a small guest-side exporter (tests/qemu/application/
gcov_dump.c, linked only here) streams every instrumented TU's .gcda over the
UART just before trapping.  This script:
  1. builds one coverage firmware per (board, test case) with RELATIVE source
     paths so the embedded .gcda filename resolves from the repo root,
  2. runs it under QEMU, captures the serial byte stream,
  3. splits the [0xA5'G''C'][len][name]<.gcda> frames into per-TU .gcda files,
  4. runs the ARM gcov on each (board, case, TU), keeping each .gcov apart,
  5. aggregates: a line is covered if ANY (board, case) executed it.

Auto-reset cases (TEST_CASE 6/10) are skipped: a system reset would wipe the
counters.  FPU cases 7/8 still run on FPU boards only (like the main matrix).

Usage:
    python tests/tool/qemu_coverage.py [--boards m3-an385 ...]
Environment: CC (arm-none-eabi), QEMU, GCOV (default: arm-none-eabi-gcov).
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOARDS = os.path.join(ROOT, "tests", "qemu")
COMMON = os.path.join(BOARDS, "application")
TRACER = ROOT
COV = os.path.join(ROOT, "build", "cov_qemu")
GCOVOUT = os.path.join(COV, "gcov")

# Instrumented library TUs we build and report on (unit stem -> source
# basename).  gcov_dump names each frame with the exported .gcda path, so we
# match on stem.  app/startup/gcov_dump/nosys are built plain (no frames).
LIB_UNITS = {"tracer": "tracer.c",
             "tracer_crash_store": "tracer_crash_store.c"}
CASES = ["0", "1", "2", "3", "4", "5", "7", "8", "9"]  # skip 6/10 (auto-reset)

# import shared helpers without side effects
sys.path.insert(0, os.path.join(ROOT, "tests", "tool"))
import board_test as bt  # noqa: E402


def cc():
    return os.environ.get("CC", "arm-none-eabi-gcc")


def qemu():
    return os.environ.get("QEMU", "qemu-system-arm")


def gcov():
    return os.environ.get("GCOV", "arm-none-eabi-gcov")


def boards():
    out = []
    for d in sorted(os.listdir(BOARDS)):
        if os.path.exists(os.path.join(BOARDS, d, "config.json")):
            out.append(d)
    return out


def machine_ok(cfg):
    try:
        p = subprocess.run([qemu(), "-machine", "help"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=20)
    except Exception:
        return False
    return cfg.get("machine") and cfg["machine"] in p.stdout


def build(board, case):
    cfg = bt.load_config(board)
    meta = bt.case_meta(cfg, case)
    if meta is None:
        return None, "no-fpu"
    out = os.path.join(COV, board, case)
    os.makedirs(out, exist_ok=True)
    core_flags = ["-mcpu=%s" % cfg["core"], "-mthumb",
                  "-mfloat-abi=%s" % meta["abi"]]
    if meta["fpu"]:
        core_flags += ["-mfpu=%s" % meta["fpu"]]
    base = bt.cflags(cfg, board, case, meta)  # includes -DTEST_CASE etc.
    cov = ["-fprofile-arcs", "-ftest-coverage", "-fprofile-info-section"]

    objs = []
    def comp(name, src, extra, cwd):
        obj = os.path.join(out, name + ".o")
        cmd = [cc(), "-c", src, "-o", obj] + base + extra
        if subprocess.call(cmd, cwd=cwd) != 0:
            raise SystemExit("compile failed: %s" % src)
        objs.append(obj)

    # Instrumented library sources use REPO-ROOT-RELATIVE paths (compiled from
    # ROOT) so the .gcda filename resolves when gcov runs from ROOT.
    comp("tracer", "tracer.c", cov, ROOT)
    comp("tracer_crash_store", "tracer_crash_store.c", cov, ROOT)
    # app/asm are built plain (no instrumentation -> no extra frames).  The
    # smoke case needs the export hook symbol so pass -DTRACER_GCOV.
    comp("app", os.path.join("tests", "qemu", "application", "app.c"),
         ["-DTRACER_GCOV"], ROOT)
    # asm trampolines: plain.
    comp("tracer_gnugcc", os.path.join(TRACER, "tracer_gnugcc.s"), [], ROOT)
    # gcov_dump provides the strong tracer_halt() that streams .gcda at trap
    # time; nosys supplies the newlib syscall stubs for bare-metal linking.
    comp("gcov_dump", os.path.join("tests", "qemu", "application", "gcov_dump.c"),
         [], ROOT)
    comp("nosys", os.path.join("tests", "qemu", "application", "nosys.c"),
         [], ROOT)
    comp("startup", os.path.join(BOARDS, cfg["startup"]), [], None)

    ld = bt.linker_path(cfg, board)
    elf = os.path.join(out, "cov.elf")
    link = ([cc(), "-o", elf] + core_flags +
            ["-fprofile-arcs", "-nostartfiles", "-T", ld,
             "--specs=nano.specs", "-Wl,--gc-sections"] + objs)
    if subprocess.call(link) != 0:
        raise SystemExit("link failed: %s" % board)
    return out, None


def run_qemu(cfg, elf):
    log = elf[:-4] + ".raw"
    cmd = [qemu(), "-machine", cfg["machine"]]
    cmd += cfg.get("qemu_args", ["-display", "none", "-serial", "stdio"])
    try:
        s = cmd.index("-serial")
    except ValueError:
        s = -1
    if s >= 0 and s + 1 < len(cmd):
        cmd[s + 1] = "file:%s" % log
    else:
        cmd += ["-serial", "file:%s" % log]
    cmd += ["-kernel", elf]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(1.5)          # let the fault dump + .gcda frames flush
    if p.poll() is None:
        p.kill()
    try:
        p.wait(timeout=5)
    except Exception:
        pass
    with open(log, "rb") as f:
        return f.read()


def split_frames(raw):
    """Yield (name, gcda_bytes) for each [0xA5 'G' 'C'][u16][name] frame."""
    frames = []
    i = 0
    n = len(raw)
    magic = bytes([0xA5, ord('G'), ord('C')])
    while True:
        j = raw.find(magic, i)
        if j < 0:
            break
        if j + 5 > n:
            break
        ln = raw[j + 3] | (raw[j + 4] << 8)
        name = raw[j + 5:j + 5 + ln].decode("latin-1", "replace")
        # payload starts after the header; ends at next magic or EOF
        k = raw.find(magic, j + 5 + ln)
        end = k if k >= 0 else n
        frames.append((name, raw[j + 5 + ln:end]))
        if k < 0:
            break
        i = j + 5 + ln
    return frames


def store_and_gcov(board, case):
    """Split the raw capture, save each TU's .gcda beside its .gcno, run gcov,
    and park the .gcov output in GCOVOUT/<board>_<case>/. Returns the list of
    produced (source basename, .gcov path)."""
    raw_path = os.path.join(COV, board, case, "cov.raw")
    if not os.path.exists(raw_path):
        return []
    raw = open(raw_path, "rb").read()
    out = os.path.join(COV, board, case)
    res = []
    for name, gcda in split_frames(raw):
        stem = os.path.basename(name)
        if stem.endswith(".gcda"):
            stem = stem[:-5]
        if stem not in LIB_UNITS:
            continue
        src_base = LIB_UNITS[stem]
        gcda_path = os.path.join(out, stem + ".gcda")
        open(gcda_path, "wb").write(gcda)
        dst_dir = os.path.join(GCOVOUT, "%s_%s" % (board, case))
        os.makedirs(dst_dir, exist_ok=True)
        # gcov from ROOT so the relative source path resolves; it emits
        # <source>.gcov into the cwd (ROOT) -- move it aside afterwards.
        # Drop any stale output first so a failed gcov isn't mistaken for a
        # fresh report.
        src_gcov = os.path.join(ROOT, src_base + ".gcov")
        if os.path.exists(src_gcov):
            os.unlink(src_gcov)
        p = subprocess.run([gcov(), "-o", out, gcda_path],
                           cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if os.path.exists(src_gcov):
            shutil.move(src_gcov, os.path.join(dst_dir, src_base + ".gcov"))
            res.append((src_base, os.path.join(dst_dir, src_base + ".gcov")))
        else:
            sys.stderr.write("gcov produced nothing for %s/%s/%s\n%s\n" %
                             (board, case, stem, p.stdout))
    # gcov also emits .gcov for headers pulled in via -include (uart.h) and
    # for other sources; only our per-TU reports are moved away above, so
    # sweep the leftovers out of the repo root.
    for g in glob.glob(os.path.join(ROOT, "*.gcov")):
        try:
            os.unlink(g)
        except OSError:
            pass
    return res


def parse_gcov(path):
    """Return {line: hits} for executable lines."""
    hits = {}
    with open(path, "r", errors="replace") as f:
        for raw in f:
            parts = raw.split(":", 2)
            if len(parts) != 3:
                continue
            cnt, ln = parts[0].strip(), parts[1].strip()
            if not ln.isdigit() or int(ln) <= 0:
                continue
            line = int(ln)
            if cnt == "-":
                continue
            if cnt == "#####":
                hits.setdefault(line, 0)
            else:
                try:
                    hits[line] = hits.get(line, 0) + int(cnt)
                except ValueError:
                    pass
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boards", nargs="*", default=None)
    ap.add_argument("--cases", nargs="*", default=None,
                    help="only these test cases (default: all non-reset)")
    ap.add_argument("--no-build", action="store_true",
                    help="skip build/run; aggregate existing cov.raw files")
    args = ap.parse_args()
    boards_sel = args.boards or boards()
    cases_sel = args.cases or CASES

    ran = []
    if args.no_build:
        for b in boards_sel:
            for case in cases_sel:
                if os.path.exists(os.path.join(COV, b, case, "cov.raw")):
                    ran.append((b, case))
    else:
        for b in boards_sel:
            cfg = bt.load_config(b)
            if not machine_ok(cfg):
                print("SKIP  %-10s machine '%s' not available"
                      % (b, cfg.get("machine", "-")))
                continue
            for case in cases_sel:
                out, why = build(b, case)
                if out is None:
                    print("SKIP  %-10s tc%s (%s)" % (b, case, why))
                    continue
                elf = os.path.join(out, "cov.elf")
                raw = run_qemu(cfg, elf)
                open(os.path.join(out, "cov.raw"), "wb").write(raw)
                ran.append((b, case))
                print("RUN   %-10s tc%s (cov.raw %d B)"
                      % (b, case, len(raw)))

    # aggregate per source
    per_file = {}
    for b, case in ran:
        for tu, gcpath in store_and_gcov(b, case):
            hits = parse_gcov(gcpath)
            bucket = per_file.setdefault(tu, {})
            for ln, h in hits.items():
                bucket.setdefault(ln, {"exec": False, "hits": 0})
                bucket[ln]["exec"] = True
                if h > 0:
                    bucket[ln]["hits"] += h

    print("\n== QEMU-layer line coverage (gcov) ==")
    for src in sorted(per_file):
        lines = per_file[src]
        total = len(lines)
        covered = sum(1 for v in lines.values() if v["hits"] > 0)
        pct = 100.0 * covered / total if total else 0.0
        print("%-24s  %5.2f%%  (%d/%d)" % (src, pct, covered, total))
        un = sorted(ln for ln, v in lines.items() if v["hits"] == 0)
        if un:
            print("  uncovered (%d): %s" % (len(un), un))
    print("\nraw/gcov data under %s" % COV)
    return 0


if __name__ == "__main__":
    sys.exit(main())
