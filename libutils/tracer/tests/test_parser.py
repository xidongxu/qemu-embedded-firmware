#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host unit tests for tracer_parser.py pure helpers (no ELF required).

Run:  python3 test_parser.py

Covers the risky pure functions: dump-log parsing (regs / callstack / raw
stack / function trace / FW / FPU block), the EHABI prel31 sign decode, the
EHABI opcode executor and the raw-stack return-address filter.  ELF-dependent
paths (SymbolMap / LineResolver / ExidxUnwind.find) are exercised on-device.

Requires pyelftools only to import the module (the parser imports it at
module scope); skips with a note if it is not installed.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    import elftools  # noqa: F401  (module-level dependency of tracer_parser)
except ImportError:
    sys.stderr.write("skip: pyelftools not installed; parser tests skipped\n")
    sys.exit(0)

from tracer_parser import ExidxUnwind, parse_log, raw_return_candidates  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL: %s" % msg)


def test_parse_log():
    log = (
        "===== Tracer: UsageFault Fault Dump =====\r\n"
        "FW     : v1.2.3\r\n"
        "text   [10000000 - 101CD324]\r\n"
        " R12=00001234  SP =80215268  LR =1000196D  PC =100BFB3A\r\n"
        " Call stack: 100C250C 10001FCC\r\n"
        " FPU (extended frame):\r\n"
        "  S0 =3F800000 S1 =40000000 S2 =00000000 S3 =00000000\r\n"
        " FPSCR=00000000\r\n"
        " Raw stack (0x80215268, 16 bytes):\r\n"
        "  80215268: 6D 19 00 10 CC 25 0C 10 00 00 00 00 00 00 00 00\r\n"
        " Function trace (last 2):\r\n"
        "  -> 10001FCC\r\n"
        "  <- 1000196D\r\n"
        "===== End of dump =====\r\n"
    )
    info = parse_log(log)
    check(info["fw"] == "v1.2.3", "FW version parsed")
    check(info["text"] == (0x10000000, 0x101CD324), "text range parsed")
    check(info["regs"]["sp"] == 0x80215268, "SP parsed")
    check(info["regs"]["pc"] == 0x100BFB3A, "PC parsed")
    check(info["callstack"] == [0x100C250C, 0x10001FCC], "callstack parsed")
    check(info["fpu"][0] == 0x3F800000, "FPU S0 parsed")
    check(info["fpu"][1] == 0x40000000, "FPU S1 parsed")
    check(info["fpu"][16] == 0x00000000, "FPU FPSCR parsed")
    # little-endian 4-byte re-assembly of the first raw-stack line
    check(info["raw"][0] == (0x80215268, 0x1000196D), "raw word0 reassembled")
    check(info["trace"] == [("->", 0x10001FCC), ("<-", 0x1000196D)],
          "function trace parsed")


def test_prel31():
    check(ExidxUnwind._prel31(0x1234) == 0x1234, "prel31 positive")
    # negative: stored = 2^31 - X  ->  decoded = stored - 2^31
    check(ExidxUnwind._prel31(0x80000000 - 0x100) == -0x100, "prel31 negative")


def test_execute():
    # pop {r4, lr} then finish: [0xA8, 0xB0]
    stack = {0x100: 0x11111111, 0x104: 0x10000005}
    ra, nsp = ExidxUnwind._execute(b"\xA8\xB0", 0x100,
                                   lambda a: stack.get(a), lr=None)
    check(ra == 0x10000004, "execute pop lr clears thumb bit, got %r" % (ra,))
    check(nsp == 0x108, "execute next sp, got %r" % (nsp,))

    # vsp += N*4 opcode (0x03 -> +16) then finish, no pop -> uses live LR
    ra2, nsp2 = ExidxUnwind._execute(b"\x03\xB0", 0x100,
                                     lambda a: None, lr=0x20000009)
    check(ra2 == 0x20000008, "execute frameless uses lr, got %r" % (ra2,))
    check(nsp2 == 0x110, "execute vsp add, got %r" % (nsp2,))

    # unknown opcode -> (None, None)
    ra3, nsp3 = ExidxUnwind._execute(b"\xCC\xB0", 0x100,
                                     lambda a: None, lr=None)
    check(ra3 is None and nsp3 is None, "execute unknown opcode")


def test_raw_candidates():
    info = {"text": (0x10000000, 0x20000000), "raw": [
        (0x80215268, 0x1000196D),  # Thumb return addr inside .text -> hit
        (0x8021526C, 0xDEADBEEF),  # outside .text -> miss
        (0x80215270, 0x00000001),  # odd but below .text start -> miss
    ]}
    cands = raw_return_candidates(info, None)
    check(cands == [(0x80215268, 0x1000196C)],
          "raw candidates, got %r" % (cands,))


if __name__ == "__main__":
    test_parse_log()
    test_prel31()
    test_execute()
    test_raw_candidates()
    if FAILURES:
        print("tracer_parser test: %d FAILURE(S)" % len(FAILURES))
        sys.exit(1)
    print("tracer_parser test: all passed")
    sys.exit(0)
