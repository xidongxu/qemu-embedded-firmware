#!/usr/bin/env python3
"""encrypt_cred.py - encrypt the SIP credential password for the firmware.

Encrypts a plaintext credential with AES-128-CBC (PKCS7 padding) using the
fixed on-device key/IV and prints a C initializer for lib
boards/mps2-an505/FreeRTOS/application/pj_crypto.c.

The key/IV must match pj_crypto.c (they are baked into the firmware; this
is obfuscation-level protection, not a hardware-backed secret).

Usage:
    python works\\tools\\encrypt_cred.py            # default: "1234"
    python works\\tools\\encrypt_cred.py "mypass"
"""
import subprocess
import sys

KEY = b"qemu-phone-cred01"   # 16 bytes, must match pj_crypto.c
IV = b"0123456789abcdef"     # 16 bytes, must match pj_crypto.c
OPENSSL = r"C:\Program Files\Git\usr\bin\openssl.exe"


def hexstr(b):
    return "".join("%02x" % x for x in b)


def main():
    plain = sys.argv[1].encode() if len(sys.argv) > 1 else b"1234"
    key_hex = hexstr(KEY)
    iv_hex = hexstr(IV)

    p = subprocess.run(
        [OPENSSL, "enc", "-aes-128-cbc", "-K", key_hex, "-iv", iv_hex],
        input=plain, capture_output=True, check=True)
    ct = p.stdout

    print("// key : %s" % KEY.decode())
    print("// iv  : %s" % IV.decode())
    print("// plaintext: %r" % plain.decode())
    print("static const uint8_t s_cred_ct[] = {")
    line = []
    for i, b in enumerate(ct):
        line.append("0x%02x" % b)
        if (i + 1) % 12 == 0:
            print("    " + ", ".join(line) + ",")
            line = []
    if line:
        print("    " + ", ".join(line) + ",")
    print("};")


if __name__ == "__main__":
    main()
