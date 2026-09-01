#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tracer_decode.py -- offline symbol resolution for tracer fault dumps.

Turns a tracer dump (serial log) + the ELF into a readable call chain:

    python tracer_decode.py <elf> <dump.log>
    python tracer_decode.py <elf> -          # read dump from stdin
    python tracer_decode.py <elf> <pc> ...   # just symbolize address(es)

It parses the "text [..]" banner, the register block, "Call stack:" and the
"Raw stack" hex dump emitted by libutils/tracer, then maps every PC (and any
word inside .text found in the raw stack) to `function+0xoffset` using the
ELF symbol table (pyelftools).  This is the "offline decode" backtrace path:
works for ANY toolchain (GCC / IAR / ARMCC5) and is exact when the project
keeps symbols, without needing .ARM.exidx at runtime.

Requires: pip install pyelftools
"""

import argparse
import bisect
import os
import re
import sys

try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
except ImportError:  # pragma: no cover
    sys.stderr.write("error: pyelftools not installed (pip install pyelftools)\n")
    sys.exit(2)


# --------------------------------------------------------------------------
# ELF symbol table -> sorted function ranges
# --------------------------------------------------------------------------
class SymbolMap:
    def __init__(self, elf_path):
        self.path = elf_path
        self.funcs = []  # list of (addr, end, name)
        with open(elf_path, "rb") as f:
            self._load(ELFFile(f))
        self._addrs = [a for (a, _, _) in self.funcs]

    def _load(self, elf):
        for sec in elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for sym in sec.iter_symbols():
                if sym["st_info"]["type"] != "STT_FUNC":
                    continue
                addr = sym["st_value"]
                size = sym["st_size"]
                if addr == 0 or not sym.name:
                    continue
                self.funcs.append([addr, addr + (size if size else 0), sym.name])
        self.funcs.sort(key=lambda r: r[0])
        # close open-ended ranges using the next symbol's start address
        for i, r in enumerate(self.funcs[:-1]):
            if r[2 - 1] == r[0]:  # size was 0
                r[1] = self.funcs[i + 1][0]

    def resolve(self, addr):
        """Return (name, offset) for an address, or None."""
        i = bisect.bisect_right(self._addrs, addr) - 1
        if i < 0:
            return None
        start, end, name = self.funcs[i]
        if addr >= end:
            return None
        return name, addr - start


# --------------------------------------------------------------------------
# Dump log parsing
# --------------------------------------------------------------------------
def parse_log(text):
    """Extract text range, regs, call-stack PCs and raw-stack words."""
    info = {"text": (0, 0), "regs": {}, "callstack": [], "raw": []}
    m = re.search(r"text\s+\[([0-9A-Fa-f]+) - ([0-9A-Fa-f]+)\]", text)
    if m:
        info["text"] = (int(m.group(1), 16), int(m.group(2), 16))

    m = re.search(r"R12=([0-9A-Fa-f]{8})\s+SP\s*=([0-9A-Fa-f]{8})\s+"
                  r"LR\s*=([0-9A-Fa-f]{8})\s+PC\s*=([0-9A-Fa-f]{8})", text)
    if m:
        info["regs"] = {"r12": int(m.group(1), 16), "sp": int(m.group(2), 16),
                        "lr": int(m.group(3), 16), "pc": int(m.group(4), 16)}

    m = re.search(r"Call stack:\s*((?:[0-9A-Fa-f]{8}\s*)*)", text)
    if m:
        info["callstack"] = [int(t, 16) for t in m.group(1).split()]

    # Raw stack: line-oriented (each line is "  ADDR: b0 b1 b2 ...").
    # Re-assemble 4 bytes into little-endian 32-bit words for address checks.
    m = re.search(r"Raw stack \(0x([0-9A-Fa-f]+), (\d+) bytes\):", text)
    if m:
        for line in text[m.end():].splitlines():
            line = line.strip()
            if not line:
                continue
            if "====" in line:
                break
            if ":" not in line:
                break
            try:
                base = int(line.split(":", 1)[0], 16)
            except ValueError:
                break
            bytes_ = [int(t, 16) for t in line.split(":", 1)[1].split()]
            for k in range(0, len(bytes_) - 3, 4):
                w = (bytes_[k] | (bytes_[k + 1] << 8) |
                     (bytes_[k + 2] << 16) | (bytes_[k + 3] << 24))
                info["raw"].append((base + k, w))
    return info


def raw_return_candidates(info, sym):
    """Words in raw stack that look like Thumb return addresses inside text."""
    ts, te = info["text"]
    out = []
    for addr, val in info["raw"]:
        if (val & 1) and (ts <= (val & ~1) < te):
            out.append((addr, val & ~1))
    return out


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", help="path to the ELF (e.g. an505-qemu.elf)")
    ap.add_argument("dump", nargs="*", default=[],
                    help="dump log file, '-' for stdin, or bare addresses")
    args = ap.parse_args()

    sym = SymbolMap(args.elf)

    def show(addr):
        return sym.resolve(addr)

    # Distinguish "log file / stdin" from "bare address list":
    # a single argument that is '-' or an existing file is a dump log;
    # otherwise every argument is treated as a bare address.
    if args.dump and not (len(args.dump) == 1 and
                          (args.dump[0] == "-" or os.path.isfile(args.dump[0]))):
        for a in args.dump:
            addr = int(a, 0)
            hit = sym.resolve(addr)
            print("%08X  %s" % (addr, _fmt(hit)))
        return

    if args.dump and args.dump[0] == "-":
        log = sys.stdin.read()
    elif args.dump:
        with open(args.dump[0], "r", encoding="utf-8", errors="replace") as f:
            log = f.read()
    else:
        sys.stderr.write("usage: tracer_decode.py <elf> <dump.log|-> [pc ...]\n")
        sys.exit(2)

    info = parse_log(log)
    ts, te = info["text"]

    print("=== tracer fault dump decode ===")
    print("ELF: %s" % args.elf)
    print("text [%08X - %08X]" % (ts, te))
    print()

    regs = info["regs"]
    if regs:
        print("PC  =%08X  %s" % (regs["pc"], _fmt(show(regs["pc"]))))
        print("LR  =%08X  %s" % (regs["lr"], _fmt(show(regs["lr"]))))
        print("SP  =%08X" % regs["sp"])

    if info["callstack"]:
        print("\nCall stack (from tracer):")
        for a in info["callstack"]:
            print("  %08X  %s" % (a, _fmt(show(a))))

    cands = raw_return_candidates(info, sym)
    if cands:
        print("\nRaw stack return-address candidates:")
        for addr, pc in cands:
            print("  [%08X] %08X  %s" % (addr, pc, _fmt(show(pc))))

    if not (regs or info["callstack"] or cands):
        sys.stderr.write("warning: no tracer dump structure recognized in input\n")
        sys.exit(1)


def _fmt(hit):
    if not hit:
        return "???"
    name, off = hit
    return "%s+0x%X" % (name, off) if off else name


if __name__ == "__main__":
    main()
