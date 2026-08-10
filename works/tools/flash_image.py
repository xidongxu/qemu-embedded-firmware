#!/usr/bin/env python3
"""Tools to create / pre-fill / inspect the mps2-an505 SPI NOR flash image.

The flash is a raw image backing QEMU's `-drive if=mtd,format=raw,file=flash.bin`
for the Winbond w25q02jvm (256 MiB) on the mps2-an505.  NOR flash erased state
is 0xFF, so a freshly created image is filled with 0xFF (matching the QEMU
m25p80 default when no drive is attached).

The default pattern (i*3+1 & 0xFF) matches `spi_flash_selftest()` in
Core/Src/spi_flash.c so host-side data can be verified against the device.

Usage examples:
    # create a fresh 256 MiB image (all 0xFF)
    python flash_image.py create flash.bin

    # write the i*3+1 pattern at offset 0x100000 for 1 MiB
    python flash_image.py pattern flash.bin 0x100000 1M

    # hex dump a region
    python flash_image.py dump flash.bin 0x100000 256

    # verify a region holds the i*3+1 pattern
    python flash_image.py verify flash.bin 0x100000 1M --pattern

    # verify a region is all 0xFF (erased)
    python flash_image.py verify flash.bin 0x0 4096 --erased
"""
import argparse
import os
import sys

ERASED = 0xFF
PATTERN_BLOCK = 1 << 20  # 1 MiB write chunk


def parse_size(s):
    """Parse '256M', '64K', '1234' (bytes) into an int."""
    s = s.strip().lower()
    mult = 1
    if s.endswith('g'):
        mult = 1 << 30
        s = s[:-1]
    elif s.endswith('m'):
        mult = 1 << 20
        s = s[:-1]
    elif s.endswith('k'):
        mult = 1 << 10
        s = s[:-1]
    try:
        return int(s, 0) * mult
    except ValueError:
        sys.exit(f"error: bad size '{s}'")


def pattern_byte(i):
    return (i * 3 + 1) & 0xFF


def write_erased(path, size):
    """Create an all-0xFF raw image (sparse-friendly block write)."""
    with open(path, 'wb') as f:
        chunk = bytes([ERASED]) * PATTERN_BLOCK
        remaining = size
        while remaining > 0:
            n = min(PATTERN_BLOCK, remaining)
            f.write(chunk[:n])
            remaining -= n
    print(f"created {path}: {size} bytes (0x{size:X}), all 0xFF")


def write_pattern(path, offset, length):
    with open(path, 'r+b') as f:
        f.seek(offset)
        # generate pattern in blocks to keep memory bounded
        for base in range(0, length, PATTERN_BLOCK):
            n = min(PATTERN_BLOCK, length - base)
            data = bytes(pattern_byte(base + i) for i in range(n))
            f.write(data)
    print(f"pattern written: {path} @0x{offset:X} len=0x{length:X}")


def write_zeros(path, offset, length):
    with open(path, 'r+b') as f:
        f.seek(offset)
        chunk = bytes([0x00]) * PATTERN_BLOCK
        remaining = length
        while remaining > 0:
            n = min(PATTERN_BLOCK, remaining)
            f.write(chunk[:n])
            remaining -= n
    print(f"zeros written: {path} @0x{offset:X} len=0x{length:X}")


def dump(path, offset, length):
    with open(path, 'rb') as f:
        f.seek(offset)
        data = f.read(length)
    print(f"dump @0x{offset:X} len=0x{length:X}:")
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        hexs = ' '.join(f'{b:02X}' for b in row)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print(f"  {offset + i:08X}  {hexs:<47}  {asc}")


def verify(path, offset, length, mode):
    failures = 0
    with open(path, 'rb') as f:
        f.seek(offset)
        for base in range(0, length, PATTERN_BLOCK):
            n = min(PATTERN_BLOCK, length - base)
            data = f.read(n)
            for i, b in enumerate(data):
                if mode == 'pattern':
                    exp = pattern_byte(base + i)
                else:  # erased
                    exp = ERASED
                if b != exp:
                    if failures < 16:
                        print(f"  mismatch @0x{offset + base + i:X}: got 0x{b:02X} "
                              f"expected 0x{exp:02X}")
                    failures += 1
    if failures == 0:
        print(f"verify OK: {mode} @0x{offset:X} len=0x{length:X} ({length} bytes)")
    else:
        print(f"verify FAILED: {failures} mismatches @0x{offset:X} len=0x{length:X}")
        return 1
    return 0


def info(path):
    size = os.path.getsize(path)
    print(f"{path}: {size} bytes = {size / (1 << 20):.1f} MiB (0x{size:X})")
    with open(path, 'rb') as f:
        first = f.read(16)
    print(f"  first bytes: {' '.join(f'{b:02X}' for b in first)}")


def main():
    ap = argparse.ArgumentParser(description='mps2-an505 SPI NOR flash image tool')
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('create', help='create an all-0xFF raw image')
    p.add_argument('file')
    p.add_argument('--size', default='256M', help='image size, e.g. 256M / 64K / 1234')

    for name, help_ in [('pattern', 'write i*3+1 pattern'),
                        ('zeros', 'write zeros'),
                        ('dump', 'hex dump a region'),
                        ('verify', 'verify a region')]:
        p = sub.add_parser(name, help=help_)
        p.add_argument('file')
        p.add_argument('offset', help='byte offset, e.g. 0x100000')
        p.add_argument('len', help='length, e.g. 1M / 256')
        if name == 'verify':
            verify_p = p

    verify_p.add_argument('--pattern', action='store_true',
                          help='expect i*3+1 pattern (default: 0xFF)')
    verify_p.add_argument('--erased', action='store_true', help='expect 0xFF')

    sub.add_parser('info', help='print file size').add_argument('file')

    a = ap.parse_args()

    if a.cmd == 'create':
        write_erased(a.file, parse_size(a.size))
    elif a.cmd == 'pattern':
        write_pattern(a.file, parse_size(a.offset), parse_size(a.len))
    elif a.cmd == 'zeros':
        write_zeros(a.file, parse_size(a.offset), parse_size(a.len))
    elif a.cmd == 'dump':
        dump(a.file, parse_size(a.offset), parse_size(a.len))
    elif a.cmd == 'verify':
        mode = 'pattern' if a.pattern else 'erased'
        sys.exit(verify(a.file, parse_size(a.offset), parse_size(a.len), mode))
    elif a.cmd == 'info':
        info(a.file)


if __name__ == '__main__':
    main()
