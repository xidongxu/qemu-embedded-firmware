#!/usr/bin/env python3
"""Line + branch coverage across host and QEMU runs (gcov -b).

Re-runs gcov with -b over BOTH existing data sets -- host unit-test runs
(build/cov/run/*) and QEMU coverage runs (build/cov_qemu/<board>/<case>/*)
-- using the .gcno/.gcda already on disk, then reports per source file BOTH:
    * line coverage   : executable lines executed by any run
    * branch coverage : branch edges (if/else/switch/ternary/&&|| exits)
                        taken by any run
for the three scopes host / qemu / host|qemu.  Each scope uses its OWN
set of executable lines/edges as the denominator, so the numbers line up
with the standalone reports (coverage_report.py / qemu_coverage.py).

tracer_crash_store.c is reported on the HOST baseline only: its host suite
already covers every line, and QEMU firmware never initializes the crash
media (QEMU numbers would be pure noise) -- same rule as merge_coverage.py.

Host .gcno/.gcda come from the host gcc and QEMU ones from the ARM
toolchain: gcov_for() picks the matching gcov (override with HOST_GCOV /
GCOV).

gcov -b line annotation format (annotations belong to the most recent
source line):
    branch  0 taken 100% (fallthrough)
    branch  1 taken 0%

Usage: python scripts/branch_coverage.py
Output: stdout summary + build/cov_branch/branch_report.txt
"""
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_RUN = os.path.join(ROOT, "build", "cov", "run")      # *.gcno per test
QEMU_DIRS = os.path.join(ROOT, "build", "cov_qemu")        # <board>/<case>/
OUT = os.path.join(ROOT, "build", "cov_branch")
GCOVOUT = os.path.join(OUT, "gcov")
# source basename -> coverage scope ("union": merge host+qemu executable
# sets; "host": host set authoritative -- crash_store is 100% under host
# tests and QEMU firmware never initializes the crash media, so its QEMU
# numbers would only add noise)
REPORT = {"tracer.c": "union", "tracer_crash_store.c": "host"}

LINE_EXEC = re.compile(r"^\s*(\d+):\s*(\d+):")
LINE_HASH = re.compile(r"^\s*#####:\s*(\d+):")   # executable, not executed
LINE_DASH = re.compile(r"^\s*-:\s*(\d+):")        # non-executable line
BRANCH_RE = re.compile(r"^\s*branch\s+(\d+)\s+taken\s+(\d+)%")
BRANCH_NEVER = re.compile(r"^\s*branch\s+(\d+)\s+never executed")


def gcov_for(env):
    """Host .gcno/.gcda come from the host gcc, QEMU ones from the ARM
    toolchain -- each needs its matching gcov to read the notes format."""
    if env == "host":
        envv = os.environ.get("HOST_GCOV")
        if envv:
            return envv
        mingw = r"C:\Users\xidon\program\MSYS64\mingw64\bin\gcov.exe"
        if os.path.exists(mingw):
            return mingw
        return "gcov"   # Linux CI: gcov matching the system host gcc
    return os.environ.get("GCOV", "arm-none-eabi-gcov")


def run_gcov(gcno_dir, stem, env):
    """Run gcov -b from ROOT for one .gcno (stem) in gcno_dir; returns the
    .gcov basenames produced in ROOT (they are moved out by the caller)."""
    rel = os.path.relpath(gcno_dir, ROOT).replace(os.sep, "/")
    p = subprocess.run([gcov_for(env), "-b", "-o", rel, stem],
                       cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def sweep_gcovs(dst_tag):
    """Move any *.gcov left in ROOT into GCOVOUT/<dst_tag>/ and return the
    paths of the report sources among them."""
    dst = os.path.join(GCOVOUT, dst_tag)
    os.makedirs(dst, exist_ok=True)
    moved = []
    for g in glob.glob(os.path.join(ROOT, "*.gcov")):
        base = os.path.basename(g)
        src = base[:-len(".gcov")]          # "tracer.c.gcov" -> "tracer.c"
        shutil.move(g, os.path.join(dst, base))
        if src in REPORT:
            moved.append(os.path.join(dst, base))
    return moved


def parse_gcov(path):
    """Return (lines: {line: executed_bool}, branches: {edge: taken_bool}).
    Lines keep every EXECUTABLE line (run or #####); '-' (non-code) lines are
    dropped so the union denominator matches merge_coverage.py.  Branch/call
    annotations belong to the most recent source-line record."""
    lines = {}
    branches = {}
    cur = None
    with open(path, "r", errors="replace") as f:
        for raw in f:
            m = LINE_EXEC.match(raw)
            if m:
                cur = int(m.group(2))
                lines[cur] = True
                continue
            m = LINE_HASH.match(raw)
            if m:
                cur = int(m.group(1))
                lines[cur] = False
                continue
            m = LINE_DASH.match(raw)
            if m:
                cur = int(m.group(1))
                continue
            m = BRANCH_RE.match(raw)
            if m:
                branches[(cur, int(m.group(1)))] = int(m.group(2)) > 0
                continue
            m = BRANCH_NEVER.match(raw)
            if m:
                branches[(cur, int(m.group(1)))] = False
    return lines, branches


def collect_roots():
    """All (gcno_dir, stem) work units: host runs + qemu (board, case)."""
    units = []
    for d in sorted(glob.glob(os.path.join(HOST_RUN, "*"))):
        for gcno in sorted(glob.glob(os.path.join(d, "*.gcno"))):
            stem = os.path.basename(gcno)[:-len(".gcno")]
            units.append((d, stem, "host", os.path.basename(d)))
    for b in sorted(os.listdir(QEMU_DIRS)):
        bd = os.path.join(QEMU_DIRS, b)
        if not os.path.isdir(bd):
            continue
        for case in sorted(os.listdir(bd)):
            cd = os.path.join(bd, case)
            if not os.path.isdir(cd):
                continue
            for gcno in sorted(glob.glob(os.path.join(cd, "*.gcno"))):
                stem = os.path.basename(gcno)[:-len(".gcno")]
                units.append((cd, stem, "qemu", "%s_%s" % (b, case)))
    return units


def main():
    os.makedirs(GCOVOUT, exist_ok=True)
    # agg[env][src] = {"lines": {line: executed}, "branches": {edge: taken}}
    agg = {"host": {}, "qemu": {}}
    units = collect_roots()
    sys.stderr.write("gcov -b over %d run dirs...\n" % len(units))
    for gcno_dir, stem, env, tag in units:
        rc, out = run_gcov(gcno_dir, stem, env)
        if rc != 0:
            sys.stderr.write("gcov failed (%s/%s): %s\n" % (tag, stem, out))
            continue
        for gpath in sweep_gcovs("%s_%s" % (tag, stem)):
            src = os.path.basename(gpath)[:-len(".gcov")]
            lines, branches = parse_gcov(gpath)
            bucket = agg[env].setdefault(
                src, {"lines": {}, "branches": {}})
            for ln, ex in lines.items():
                if ln not in bucket["lines"]:
                    bucket["lines"][ln] = ex
                elif ex and not bucket["lines"][ln]:
                    bucket["lines"][ln] = True
            for edge, taken in branches.items():
                if edge not in bucket["branches"]:
                    bucket["branches"][edge] = taken
                elif taken and not bucket["branches"][edge]:
                    bucket["branches"][edge] = True
    # clean any leftovers
    for g in glob.glob(os.path.join(ROOT, "*.gcov")):
        try:
            os.unlink(g)
        except OSError:
            pass

    host_srcs = set(agg["host"])
    qemu_srcs = set(agg["qemu"])
    report = []
    print("== line + branch coverage (gcov -b over host + QEMU runs) ==")

    def pct(taken, total):
        return 100.0 * taken / total if total else 0.0

    for src in sorted(host_srcs | qemu_srcs):
        mode = REPORT.get(src, "union")
        h = agg["host"].get(src, {"lines": {}, "branches": {}})
        q = agg["qemu"].get(src, {"lines": {}, "branches": {}})
        h_lines, h_br = h["lines"], h["branches"]
        q_lines, q_br = q["lines"], q["branches"]

        # Each scope uses its OWN executable set as the denominator so the
        # numbers line up with the standalone reports (host ~92%, qemu ~81%,
        # union ~97% for tracer.c lines); "union" merges both sets.
        if mode == "host":
            # host baseline only (crash_store): qemu numbers are meaningless
            hlc = sum(1 for ln in h_lines if h_lines[ln])
            hbc = sum(1 for e in h_br if h_br[e])
            header = "%-22s  (host baseline: %d lines, %d branch edges)" % (
                src, len(h_lines), len(h_br))
            print(header)
            report.append(header)
            r = ("  line      host %6.2f%% (%d/%d)   "
                 "branch host %5.2f%% (%d/%d)"
                 % (pct(hlc, len(h_lines)), hlc, len(h_lines),
                    pct(hbc, len(h_br)), hbc, len(h_br)))
            print(r)
            report.append(r)
        else:
            u_lines = sorted(set(h_lines) | set(q_lines))
            u_br = sorted(set(h_br) | set(q_br))
            if not u_lines:
                continue
            hlc = sum(1 for ln in h_lines if h_lines[ln])
            qlc = sum(1 for ln in q_lines if q_lines[ln])
            ulc = sum(1 for ln in u_lines
                      if h_lines.get(ln) or q_lines.get(ln))
            hbc = sum(1 for e in h_br if h_br[e])
            qbc = sum(1 for e in q_br if q_br[e])
            ubc = sum(1 for e in u_br if h_br.get(e) or q_br.get(e))
            header = "%-22s  lines %d/%d/%d   branch edges %d/%d/%d " % (
                src, len(h_lines), len(q_lines), len(u_lines),
                len(h_br), len(q_br), len(u_br))
            header += "(host/qemu/union)"
            print(header)
            report.append(header)
            r = ("  line      host %6.2f%% (%d/%d)   qemu %6.2f%% (%d/%d)   "
                 "host|qemu %6.2f%% (%d/%d)"
                 % (pct(hlc, len(h_lines)), hlc, len(h_lines),
                    pct(qlc, len(q_lines)), qlc, len(q_lines),
                    pct(ulc, len(u_lines)), ulc, len(u_lines)))
            print(r)
            report.append(r)
            r = ("  branch    host %6.2f%% (%d/%d)   qemu %6.2f%% (%d/%d)   "
                 "host|qemu %6.2f%% (%d/%d)"
                 % (pct(hbc, len(h_br)), hbc, len(h_br),
                    pct(qbc, len(q_br)), qbc, len(q_br),
                    pct(ubc, len(u_br)), ubc, len(u_br)))
            print(r)
            report.append(r)
            # one-sided taken edges + still-untaken (union scope)
            h_t = set(e for e in h_br if h_br[e])
            q_t = set(e for e in q_br if q_br[e])
            only_qemu = sorted(q_t - h_t)
            only_host = sorted(h_t - q_t)
            if only_qemu:
                x = ("  QEMU-only taken branch edges (%d): %s"
                     % (len(only_qemu), only_qemu))
                print(x)
                report.append(x)
            if only_host:
                x = ("  host-only taken branch edges (%d): %s"
                     % (len(only_host), only_host))
                print(x)
                report.append(x)
            miss = sorted(set(u_br) - (h_t | q_t))
            if miss:
                m = ("  STILL untaken branch edges (%d): %s"
                     % (len(miss), miss))
                print(m)
                report.append(m)
            miss_lines = sorted(set(u_lines)
                                - set(ln for ln in u_lines
                                      if h_lines.get(ln) or q_lines.get(ln)))
            if miss_lines:
                m = ("  STILL uncovered lines (%d): %s"
                     % (len(miss_lines), miss_lines))
                print(m)
                report.append(m)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "branch_report.txt"), "w",
              encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\nfull report: %s" % os.path.join(OUT, "branch_report.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
