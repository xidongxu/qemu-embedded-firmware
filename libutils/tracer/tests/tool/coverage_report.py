#!/usr/bin/env python3
"""Host line-coverage report for the tracer library.

 * Compiles every host unit test under tests/host/ with gcov instrumentation
(gcc --coverage), runs each one, runs gcov per translation unit, then
aggregates line coverage of tracer.c / tracer_crash_store.c across all
tests (a line is "covered" if ANY test executed it).

Only C-logic paths can be covered this way; hardware/fault-context paths
(real exception entry, FPU lazy frame, SecureFault, PSP vs MSP, NMI, ...)
are exercised by the QEMU boards instead -- see the repo README matrix.

Environment:
    CC    host C compiler with gcov (default: mingw gcc if found, else gcc)
    GCOV  gcov binary (default: beside CC, else 'gcov')

Output:
    stdout report + build/cov/uncovered.txt (per-line uncovered list) +
    build/cov/report.txt (full text)
"""
import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build", "cov")
RUN = os.path.join(BUILD, "run")
GCOVDIR = os.path.join(BUILD, "gcov")

# (exe-stem, test-source, extra -D defines, extra object sources to link)
TESTS = [
    ("test_tracer_host", "test_tracer.c", [], []),
    # Directly drives tracer_fault_handler() with a RAM MMIO backend (see the
    # test header) -- covers the fault/assert decode + dump pipeline that a
    # real fault reaches on the target.
    ("test_tracer_fault", "test_fault_handler.c", [], []),
    ("test_tracer_miniprint", "test_miniprint.c", [], []),
    ("test_tracer_crashlog", "test_crashlog.c", [], []),
    ("test_tracer_log", "test_tracer_log.c", [], []),
    ("test_tracer_log_sink", "test_tracer_log_sink.c",
     ["TRACER_USE_LOG=1", "TRACER_STACK_DUMP_BYTES=0",
      "TRACER_STACK_BASE=0x800", "TRACER_STACK_TOP=0x1000",
      "TRACER_TEXT_START=0x08000000", "TRACER_TEXT_END=0x08020000"],
     ["tracer.c"]),
    ("test_tracer_crash_store", "test_crash_store.c",
     ["TRACER_USE_CRASH=1"], ["tracer_crash_store.c"]),
    # Weak-media defaults: each mode links only part of the media layer so
    # the remaining weak get_media/erase/write/read stubs execute.
    ("test_cs_stub_nomedia", "test_crash_store_stubs.c",
     ["TRACER_USE_CRASH=1", "TCS_STUB_MODE=0"], ["tracer_crash_store.c"]),
    ("test_cs_stub_noerase", "test_crash_store_stubs.c",
     ["TRACER_USE_CRASH=1", "TCS_STUB_MODE=1"], ["tracer_crash_store.c"]),
    ("test_cs_stub_nowrite", "test_crash_store_stubs.c",
     ["TRACER_USE_CRASH=1", "TCS_STUB_MODE=2"], ["tracer_crash_store.c"]),
]


def tool(name, default=None):
    env = os.environ.get(name.upper())
    if env:
        return env
    if name == "cc":
        cand = r"C:\Users\xidon\program\MSYS64\mingw64\bin\gcc.exe"
        if os.path.exists(cand):
            return cand
        return "gcc"
    # gcov sits beside gcc in the same bin dir
    cc = tool("cc")
    cand = os.path.join(os.path.dirname(cc), "gcov.exe") if cc != "gcc" else ""
    if cand and os.path.exists(cand):
        return cand
    return "gcov"


def parse_gcov(path):
    """Return {line: hits} for executable lines and a set of executable lines."""
    hits = {}
    with open(path, "r", errors="replace") as f:
        for raw in f:
            parts = raw.split(":", 2)
            if len(parts) != 3:
                continue
            cnt, ln, _ = parts[0].strip(), parts[1].strip(), parts[2]
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


def run(cmd, cwd=None):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, cwd=cwd)
    return p


def main():
    cc, gcov = tool("cc"), tool("gcov")
    shutil.rmtree(BUILD, ignore_errors=True)   # clean previous run
    os.makedirs(RUN, exist_ok=True)
    os.makedirs(GCOVDIR, exist_ok=True)

    coverage_files = {}   # gcov dir -> list of .gcov names collected
    failures = []

    for stem, src, defs, extra_objs in TESTS:
        d = os.path.join(RUN, stem)
        os.makedirs(d, exist_ok=True)
        out = os.path.join(d, stem + ".exe")
        cmd = [cc, "--coverage", "-std=c99",
               "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
               "-I", ROOT] + ["-D%s" % x for x in defs]
        cmd.append(os.path.join("tests", "host", src))
        for obj in extra_objs:
            cmd += ["-I", ROOT, os.path.join(ROOT, obj)]
        cmd += ["-o", out]
        p = run(cmd, cwd=ROOT)
        if p.returncode != 0:
            failures.append((stem, "compile", p.stdout))
            print("FAIL compile %s\n%s" % (stem, p.stdout))
            continue
        p = run([os.path.abspath(out)], cwd=d)
        if p.returncode != 0:
            failures.append((stem, "run", p.stdout))
            print("FAIL run %s rc=%d\n%s" % (stem, p.returncode, p.stdout))

    # gcov per translation unit (each *.gcno in every run dir)
    for stem, *_ in TESTS:
        d = os.path.join(RUN, stem)
        for gcno in sorted(glob.glob(os.path.join(d, "*.gcno"))):
            base = os.path.basename(gcno)[:-len(".gcno")]
            # run gcov from ROOT so relative source paths resolve; gcov is an
            # MSYS tool that chokes on backslashes -> pass a forward-slash path
            rel = os.path.relpath(d, ROOT).replace(os.sep, "/")
            p = run([gcov, "-o", rel, base], cwd=ROOT)
            if p.returncode != 0:
                failures.append((base, "gcov", p.stdout))
                continue
            # move .gcov out of the repo root
            gd = os.path.join(GCOVDIR, base)
            os.makedirs(gd, exist_ok=True)
            for g in glob.glob(os.path.join(ROOT, "*.gcov")):
                shutil.move(g, os.path.join(gd, os.path.basename(g)))
            coverage_files[base] = gd

    # ---- aggregate per source file across all TUs ----
    per_file = {}
    for tu, gd in coverage_files.items():
        for g in glob.glob(os.path.join(gd, "*.gcov")):
            src = os.path.basename(g)[:-len(".gcov")]
            if src.endswith(".c"):
                src = src[:-2]
            if src not in ("tracer", "tracer_crash_store"):
                continue
            hits = parse_gcov(g)
            bucket = per_file.setdefault(src, {})
            for ln, h in hits.items():
                bucket.setdefault(ln, {"exec": False, "hits": 0})
                bucket[ln]["exec"] = True
                if h > 0:
                    bucket[ln]["hits"] += h

    lines_out = []
    report = []
    for src in sorted(per_file):
        lines = per_file[src]
        total = len(lines)
        covered = sum(1 for v in lines.values() if v["hits"] > 0)
        pct = 100.0 * covered / total if total else 0.0
        head = "%-20s  line coverage %5.2f%%  (%d/%d executable lines)" % (
            src, pct, covered, total)
        print(head)
        report.append(head)
        un = sorted(ln for ln, v in lines.items() if v["hits"] == 0)
        if un:
            print("  uncovered lines (%d): %s" % (len(un), un))
            report.append("  uncovered: %s" % un)
            lines_out.append("%s: %s" % (src, un))
        else:
            print("  all executable lines covered")
            report.append("  all executable lines covered")

    with open(os.path.join(BUILD, "report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    with open(os.path.join(BUILD, "uncovered.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines_out) + "\n")
    print("\nfull report : %s" % os.path.join(BUILD, "report.txt"))
    if failures:
        print("\n%d steps failed (see above)" % len(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
