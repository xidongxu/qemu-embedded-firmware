#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tracer_parser.py -- offline symbol resolution + .ARM.exidx unwind for tracer
fault dumps.

This file is the library-shipped copy of works/tools/tracer_decode.py
(keep both in sync when changing one).

Turns a tracer dump (serial log) + the ELF into a readable call chain:

    python tracer_parser.py <elf> <dump.log>
    python tracer_parser.py <elf> -          # read dump from stdin
    python tracer_parser.py <elf> <pc> ...   # just symbolize address(es)

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
import struct
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
    """Extract text range, regs, call-stack PCs, raw-stack words and the
    dynamic function trace (-finstrument-functions)."""
    info = {"text": (0, 0), "regs": {}, "callstack": [], "raw": [], "trace": []}
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

    # Function trace block: lines "  -> ADDR" / "  <- ADDR".
    m = re.search(r"Function trace \(last \d+\):", text)
    if m:
        for line in text[m.end():].splitlines():
            line = line.strip()
            if not line:
                continue
            if "====" in line:
                break
            mm = re.match(r"(->|<-)\s+([0-9A-Fa-f]{8})", line)
            if not mm:
                break
            info["trace"].append((mm.group(1), int(mm.group(2), 16)))
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
# Offline .ARM.exidx unwind
# --------------------------------------------------------------------------
class ExidxUnwind:
    """Decode the ELF .ARM.exidx/.ARM.extab (ARM EHABI) on the host and walk
    a call chain from (pc, sp) using the raw stack words captured in the dump.
    Opcode semantics are aligned with binutils `readelf -u` output (verified)."""

    def __init__(self, elf_path):
        self.entries = []  # (start, end, w1, entry_addr)
        self.extab = b""
        self.extab_addr = 0
        with open(elf_path, "rb") as f:
            elf = ELFFile(f)
            for sec in elf.iter_sections():
                if sec.name == ".ARM.exidx":
                    self.exidx_addr = sec["sh_addr"]
                    self._build_entries(sec.data())
                elif sec.name == ".ARM.extab":
                    self.extab = sec.data()
                    self.extab_addr = sec["sh_addr"]
        self.entries.sort(key=lambda e: e[0])
        for i in range(len(self.entries) - 1):
            self.entries[i] = (self.entries[i][0], self.entries[i + 1][0],
                               self.entries[i][2], self.entries[i][3])
        if self.entries:
            last = self.entries[-1]
            self.entries[-1] = (last[0], last[0] + 4, last[2], last[3])

    def _build_entries(self, data):
        for i in range(0, len(data) - 7, 8):
            w0, w1 = struct.unpack_from("<II", data, i)
            start = self._prel31(w0) + self.exidx_addr + i  # word0 location
            self.entries.append((start, 0, w1, self.exidx_addr + i))

    @staticmethod
    def _prel31(v):
        """Signed 31-bit relative offset (ARM EHABI prel31)."""
        v &= 0x7FFFFFFF
        if v & 0x40000000:  # bit30 is the sign bit
            v -= 0x80000000
        return v

    def find(self, pc):
        """Return the exidx entry (start,end,w1,entry_addr) covering pc."""
        lo, hi = 0, len(self.entries)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.entries[mid][0] <= pc:
                lo = mid + 1
            else:
                hi = mid
        idx = lo - 1
        if idx < 0:
            return None
        start, end, w1, eaddr = self.entries[idx]
        if pc >= end:
            return None
        return (start, end, w1, eaddr)

    def _instructions(self, w1, entry_addr):
        """Return the byte stream of unwind opcodes for an exidx word.

        Verified against binutils readelf -u on this toolchain:
          bit31=1 -> three inline opcodes in the low 24 bits
          bit31=0 -> signed prel31 offset to the .ARM.extab entry,
                     whose first byte is the compact model (skipped)."""
        if w1 & 0x80000000:
            return bytes([(w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF])
        off = self._prel31(w1) + entry_addr + 4
        rel = off - self.extab_addr
        if rel < 0 or rel >= len(self.extab):
            return None
        out = bytearray()
        j = rel + 1  # skip the compact-model byte
        while j < len(self.extab) and len(out) < 64:
            x = self.extab[j]
            out.append(x)
            j += 1
            if x in (0xB0, 0xB1, 0xB2):  # finish
                break
        return bytes(out)

    @staticmethod
    def _execute(instrs, sp, read32, lr=None):
        """Run EHABI opcodes; return (return_address, next_sp) or (None,None).
        read32(addr) must return an int or None if the word is unavailable.
        lr is the live LR register value: a frameless function (finish with
        no pop r14) keeps LR untouched, so it is the caller's return address."""
        vsp = sp
        restored = {}
        i, n = 0, len(instrs)
        while i < n:
            b = instrs[i]
            i += 1
            if b <= 0x3F:                       # 00xxxxxx: vsp += (b+1)*4
                vsp += (b + 1) * 4
            elif b == 0x80:                     # pop r15 (PC)
                v = read32(vsp)
                if v is None:
                    return None, None
                restored[15] = v
                vsp += 4
            elif b == 0x81:                     # pop r14 (LR)
                v = read32(vsp)
                if v is None:
                    return None, None
                restored[14] = v
                vsp += 4
            elif 0x82 <= b <= 0x8D:             # pop r14 [+ mask r4-r11]
                if i >= n:
                    return None, None
                mask = instrs[i]
                i += 1
                for k in range(8):
                    if mask & (1 << k):
                        v = read32(vsp)
                        if v is None:
                            return None, None
                        restored[4 + k] = v
                        vsp += 4
                v = read32(vsp)
                if v is None:
                    return None, None
                restored[14] = v
                vsp += 4
            elif 0x90 <= b <= 0x9F:             # pop n+1 regs r4-r4+n
                for k in range((b & 0x0F) + 1):
                    v = read32(vsp)
                    if v is None:
                        return None, None
                    restored[4 + k] = v
                    vsp += 4
            elif 0xA8 <= b <= 0xAF:             # pop r4..r(4+cnt-1), r14
                for k in range(b - 0xA7):
                    v = read32(vsp)
                    if v is None:
                        return None, None
                    restored[4 + k] = v
                    vsp += 4
                v = read32(vsp)
                if v is None:
                    return None, None
                restored[14] = v
                vsp += 4
            elif b in (0xB0, 0xB1, 0xB2):      # finish
                break
            elif 0xB4 <= b <= 0xB7:             # pop r4-r11
                for k in range(8):
                    v = read32(vsp)
                    if v is None:
                        return None, None
                    restored[4 + k] = v
                    vsp += 4
            elif 0xC0 <= b <= 0xC3:             # vsp += (8+(b&3))*4
                vsp += (8 + (b & 0x03)) * 4
            elif b == 0xC5:
                vsp -= 8
            elif b == 0xC6:
                vsp -= 16
            elif b == 0xC7:
                vsp += 8
            elif b == 0xC8:
                vsp += 16
            else:
                return None, None               # unknown opcode
        ra = restored.get(14)
        if ra is None:
            ra = lr                            # frameless fn keeps LR
        if ra is None:
            return None, None
        return (ra & ~1), vsp

    def unwind(self, pc, sp, stack_words, lr, max_frames=32):
        """Walk the chain from (pc, sp, lr). stack_words maps addr -> word."""
        chain = []

        def read32(addr):
            return stack_words.get(addr)

        for _ in range(max_frames):
            hit = self.find(pc)
            if hit is None:
                break
            start, _, w1, eaddr = hit
            instrs = self._instructions(w1, eaddr)
            if instrs is None:
                break
            npc, nsp = self._execute(instrs, sp, read32, lr)
            if npc is None or npc == pc:
                break
            chain.append((pc, npc))
            pc, sp = npc, nsp
            lr = npc | 1  # caller's LR = this frame's return address (Thumb)
        return chain


def exidx_unwind(elf_path, info, max_frames=32):
    """Offline .ARM.exidx unwind from a parsed dump (PC + SP + LR + raw stack)."""
    regs = info["regs"]
    if not regs or not info["raw"]:
        return []
    uw = ExidxUnwind(elf_path)
    if not uw.entries:
        return []
    stack_words = dict(info["raw"])  # (addr, word) pairs -> dict
    pc = regs["pc"]
    sp = regs["sp"] + 32  # skip the hardware exception frame to the pre-fault SP
    lr = regs["lr"]
    return uw.unwind(pc, sp, stack_words, lr, max_frames)


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

    if info["trace"]:
        print("\nFunction trace (last %d):" % len(info["trace"]))
        for d, a in info["trace"]:
            print("  %s %08X  %s" % (d, a, _fmt(sym.resolve(a))))

    cands = raw_return_candidates(info, sym)
    if cands:
        print("\nRaw stack return-address candidates:")
        for addr, pc in cands:
            print("  [%08X] %08X  %s" % (addr, pc, _fmt(show(pc))))

    chain = exidx_unwind(args.elf, info)
    if chain:
        print("\nExidx offline unwind (exact, from PC + raw stack):")
        for pc, npc in chain:
            print("  %08X  %s  ->  %08X  %s" % (pc, _fmt(sym.resolve(pc)),
                                                 npc, _fmt(sym.resolve(npc))))

    if not (regs or info["callstack"] or info["trace"] or cands or chain):
        sys.stderr.write("warning: no tracer dump structure recognized in input\n")
        sys.exit(1)


def _fmt(hit):
    if not hit:
        return "???"
    name, off = hit
    return "%s+0x%X" % (name, off) if off else name


if __name__ == "__main__":
    main()
