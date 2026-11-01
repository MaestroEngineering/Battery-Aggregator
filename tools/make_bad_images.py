#!/usr/bin/env python3
"""Make deliberately broken copies of a firmware image to test OTA rejection.

Usage: tools/make_bad_images.py release/bms-agg-0.1.0.bin [out_dir]

Writes into out_dir (default: release/bad/):
  wrong-chip.bin     header chip id patched to ESP32-S3   -> rejected after 288 bytes
  wrong-project.bin  app descriptor project name changed  -> rejected after 288 bytes
  bad-magic.bin      first byte not 0xE9                  -> rejected after 288 bytes
  corrupt.bin        one byte flipped in the middle       -> rejected by SHA-256 check at the end
  truncated.bin      last 64 KB cut off                   -> rejected by image verification at the end
  random.bin         random bytes                         -> rejected after 288 bytes
"""
import os
import sys

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(src) or ".", "bad")
os.makedirs(out, exist_ok=True)
img = bytearray(open(src, "rb").read())
assert img[0] == 0xE9, "input is not an ESP image"


def write(name, data):
    p = os.path.join(out, name)
    with open(p, "wb") as f:
        f.write(data)
    print(f"{p}: {len(data)} bytes")


b = bytearray(img); b[12:14] = (9).to_bytes(2, "little"); write("wrong-chip.bin", b)
b = bytearray(img); b[80:112] = b"not-bms-agg".ljust(32, b"\0"); write("wrong-project.bin", b)
b = bytearray(img); b[0] = 0x00; write("bad-magic.bin", b)
b = bytearray(img); b[len(b) // 2] ^= 0xFF; write("corrupt.bin", b)
write("truncated.bin", img[: len(img) - 65536])
write("random.bin", os.urandom(len(img)))
