#!/usr/bin/env python3
"""QEMU test matrix for the kasan library (mps2-an505).

Builds and runs KASAN_TEST_CASE 1..12, boots each firmware under QEMU, reads
the kasan_* report markers via gdb and checks them against the expected
report.  Exit code 0 = all cases pass.

Usage:
  python run_matrix.py --build <dir> --qemu <qemu-system-arm> \\
                       --gdb <arm-none-eabi-gdb> [--cases 1-12] [--no-build]
"""
import argparse
import os
import re
import subprocess
import sys
import time

# case -> (report_type, shadow, cause) expected report
EXPECTED = {
    1:  (2, 0xFB, 1),   # heap overflow
    2:  (2, 0xFA, 2),   # use-after-free
    3:  (3, 0xFA, 2),   # double-free
    4:  (2, 0xFB, 1),   # heap underflow
    5:  (2, 0xFA, 2),   # realloc-move UAF
    6:  (2, 0xFB, 1),   # realloc shrink
    7:  (2, 0x04, 3),   # partial-granule tail
    8:  (1, 0xFB, 1),   # memcpy read overflow
    9:  (2, 0xFB, 1),   # memset write overflow
    10: (2, 0xFA, 2),   # quarantine UAF
    11: (2, 0xF8, 1),   # global overflow
    12: (2, 0xF3, 1),   # stack overflow
}

MARKERS = ("kasan_reports", "kasan_report_type", "kasan_report_shadow",
           "kasan_report_cause")


def parse_markers(gdb_out):
    """Parse `x/wx &sym` lines into {symbol_name: int}."""
    result = {}
    for line in gdb_out.splitlines():
        m = re.search(r"<(\w+)>:\s*(0x[0-9a-fA-F]+)", line)
        if m:
            result[m.group(1)] = int(m.group(2), 16)
    return result


def read_report(gdb, elf, port):
    ex = ["-batch", elf, "-ex", "target remote :%d" % port]
    for name in MARKERS:
        ex += ["-ex", "x/wx &%s" % name]
    try:
        out = subprocess.run([gdb] + ex, capture_output=True, text=True,
                             timeout=15).stdout
    except subprocess.TimeoutExpired:
        return None
    return parse_markers(out)


def run_case(case, args):
    if not args.no_build:
        subprocess.run([args.cmake, "-B", args.build,
                        "-DKASAN_TEST_CASE=%d" % case],
                       capture_output=True, text=True, check=True)
        subprocess.run([args.cmake, "--build", args.build,
                        "--target", "kasan_qemu_test"],
                       capture_output=True, text=True, check=True)

    elf = os.path.join(args.build, "libutils", "kasan", "tests", "qemu",
                       "kasan_qemu_test")
    qemu_cmd = [args.qemu, "-machine", "mps2-an505", "-cpu", "cortex-m33",
                "-m", "16M", "-kernel", elf, "-display", "none",
                "-serial", "null", "-monitor", "none", "-no-reboot",
                "-gdb", "tcp::%d" % args.port]
    q = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(args.sleep)

    markers = read_report(args.gdb, elf, args.port)

    q.terminate()
    try:
        q.wait(timeout=5)
    except subprocess.TimeoutExpired:
        q.kill()

    if markers is None:
        return "FAIL (gdb timeout)"
    reports = markers.get("kasan_reports", 0)
    rtype = markers.get("kasan_report_type", 0)
    shadow = markers.get("kasan_report_shadow", 0)
    cause = markers.get("kasan_report_cause", 0)
    exp_type, exp_shadow, exp_cause = EXPECTED[case]

    if (reports == 1 and rtype == exp_type and
            shadow == exp_shadow and cause == exp_cause):
        return "PASS"
    return ("FAIL reports=%d type=0x%x shadow=0x%x cause=0x%x "
            "(want type=0x%x shadow=0x%x cause=0x%x)"
            % (reports, rtype, shadow, cause, exp_type, exp_shadow, exp_cause))


def parse_cases(spec):
    cases = []
    for part in spec.split(","):
        if "-" in part:
            lo, hi = part.split("-", 1)
            cases.extend(range(int(lo), int(hi) + 1))
        else:
            cases.append(int(part))
    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", required=True, help="CMake build directory")
    ap.add_argument("--qemu", required=True, help="path to qemu-system-arm")
    ap.add_argument("--gdb", required=True, help="path to arm-none-eabi-gdb")
    ap.add_argument("--cmake", default="cmake", help="path to cmake")
    ap.add_argument("--cases", default="1-12",
                    help="case list, e.g. '1-12' or '1,2,5'")
    ap.add_argument("--port", type=int, default=1237,
                    help="QEMU gdb stub TCP port")
    ap.add_argument("--sleep", type=float, default=2.0,
                    help="seconds to let QEMU boot before attaching gdb")
    ap.add_argument("--no-build", action="store_true",
                    help="skip configure/build (firmware must already exist)")
    args = ap.parse_args()

    cases = parse_cases(args.cases)
    failed = 0
    for case in cases:
        result = run_case(case, args)
        print("case %2d => %s" % (case, result))
        if not result.startswith("PASS"):
            failed += 1

    print("----")
    print("%d/%d passed" % (len(cases) - failed, len(cases)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
