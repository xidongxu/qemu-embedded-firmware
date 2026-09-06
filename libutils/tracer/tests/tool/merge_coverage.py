#!/usr/bin/env python3
"""Combine host + QEMU gcov data into one line-coverage picture.

Host coverage  : build/cov/gcov/<tu>/*.gcov        (coverage_report.py)
QEMU coverage  : build/cov_qemu/gcov/<board>_<case>/*.gcov (qemu_coverage.py)

A source line is "covered" if it was executed in ANY host test OR ANY
(board, case) QEMU run.  Executable-line sets are unioned across both so
lines only present in one environment still count.

Output:
    build/cov/combined.txt   full report
    stdout                  short summary
"""
import glob
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST_GCOV = os.path.join(ROOT, "build", "cov", "gcov")
QEMU_GCOV = os.path.join(ROOT, "build", "cov_qemu", "gcov")
OUT = os.path.join(ROOT, "build", "cov", "combined.txt")

# source files we report on (basename used by gcov output)
#   "union": host AND QEMU executable sets are merged (tracer.c -- each
#            environment reaches lines the other cannot).
#   "host" : host executable set is authoritative (tracer_crash_store.c is
#            already 100% under the host unit tests; the QEMU builds compile
#            it with different flags so its executable set differs and would
#            only add noise).
REPORT = {"tracer.c": "union", "tracer_crash_store.c": "host"}


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


def collect(root):
    """root -> {src_base: {line: hits}} across every *.gcov under root."""
    agg = {}
    for p in glob.glob(os.path.join(root, "**", "*.gcov"), recursive=True):
        src = os.path.basename(p)[:-len(".gcov")]
        if src not in REPORT:
            continue
        hits = parse_gcov(p)
        bucket = agg.setdefault(src, {})
        for ln, h in hits.items():
            cur = bucket.setdefault(ln, 0)
            bucket[ln] = cur + h if h > 0 else cur
    return agg


def main():
    host = collect(HOST_GCOV)
    qemu = collect(QEMU_GCOV)
    report = []
    missing = {}
    for src, mode in REPORT.items():
        host_lines = set(host.get(src, {}))
        qemu_lines = set(qemu.get(src, {}))
        if mode == "host":
            all_lines = sorted(host_lines)
        else:
            all_lines = sorted(host_lines | qemu_lines)
        if not all_lines:
            continue
        covered = []
        host_only = []     # lines only host executes
        qemu_only = []     # lines only QEMU executes (host cannot reach)
        both = []
        qemu_extra = []    # executable only under QEMU flags (host ignores)
        for ln in all_lines:
            hh = host.get(src, {}).get(ln, 0)
            qq = qemu.get(src, {}).get(ln, 0)
            if mode == "host":
                if ln not in host_lines:
                    qemu_extra.append(ln)   # not part of the host baseline
                    continue
            if hh > 0 or qq > 0:
                covered.append(ln)
                if hh > 0 and qq > 0:
                    both.append(ln)
                elif hh > 0:
                    host_only.append(ln)
                else:
                    qemu_only.append(ln)
            else:
                missing.setdefault(src, []).append(ln)
        base_total = len(all_lines)
        pct = 100.0 * len(covered) / base_total
        line = ("%-22s  combined coverage %6.2f%%  (%d/%d executable lines)"
                % (src, pct, len(covered), base_total))
        print(line)
        report.append(line)
        if qemu_only:
            q = ("  QEMU-only covered (%d): %s" % (len(qemu_only), qemu_only))
            print(q)
            report.append(q)
        if host_only:
            h = ("  host-only covered (%d): %s" % (len(host_only), host_only))
            print(h)
            report.append(h)
        if both:
            b = ("  covered by BOTH (%d lines)" % len(both))
            print(b)
            report.append(b)
        if mode == "host" and qemu_extra:
            x = ("  QEMU-only executable, not in host baseline (%d lines): %s"
                 % (len(qemu_extra), qemu_extra))
            print(x)
            report.append(x)
        miss = missing.get(src, [])
        if miss:
            m = ("  STILL uncovered (%d): %s" % (len(miss), miss))
            print(m)
            report.append(m)
        else:
            print("  STILL uncovered: none -- host+QEMU reach 100%")
            report.append("  STILL uncovered: none")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\ncombined report: %s" % OUT)


if __name__ == "__main__":
    main()
