#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministic differential fuzz for the CHD driver's hunk-slicing math.

This does not test libchdr codecs; it stress-tests the exact offset/length ->
hunk/slice/cross-hunk algorithm used by chd-block.c against a raw byte stream.
"""
from __future__ import annotations
import random

SECTOR = 2048
HUNKS = (2048, 4096, 8192, 65536, 262144, 1048576)
READS_PER_HUNK = 100_000
LOGICAL_SIZE = 5 * 1024 * 1024  # divisible by 2048

# Cheap deterministic non-periodic-enough test data.
raw = bytes((((i * 1315423911) ^ (i >> 7) ^ (i * 17)) & 0xFF)
            for i in range(LOGICAL_SIZE))


def read_hunked(offset: int, length: int, hunkbytes: int) -> bytes:
    pos = offset
    remaining = length
    out = bytearray()
    while remaining:
        hunk = pos // hunkbytes
        hunk_offset = pos % hunkbytes
        take = min(remaining, hunkbytes - hunk_offset)
        start = hunk * hunkbytes
        decoded = raw[start:min(start + hunkbytes, LOGICAL_SIZE)]
        out += decoded[hunk_offset:hunk_offset + take]
        pos += take
        remaining -= take
    return bytes(out)


def main() -> None:
    rng = random.Random(0x58454D55434844)  # "XEMUCHD"
    checks = 0
    for hunk in HUNKS:
        for _ in range(READS_PER_HUNK):
            # Bias some starts toward hunk boundaries while retaining arbitrary
            # byte offsets/lengths and EOF coverage.
            if rng.randrange(4) == 0:
                boundary = rng.randrange(0, LOGICAL_SIZE // hunk + 1) * hunk
                offset = min(LOGICAL_SIZE, max(0, boundary + rng.randrange(-32, 33)))
            else:
                offset = rng.randrange(LOGICAL_SIZE + 1)
            max_len = min(LOGICAL_SIZE - offset, 128 * 1024)
            length = rng.randrange(max_len + 1) if max_len else 0
            got = read_hunked(offset, length, hunk)
            expected = raw[offset:offset + length]
            if got != expected:
                raise SystemExit(
                    f"FAIL hunk={hunk} offset={offset} length={length}")
            checks += 1
    print(f"PASS: {checks:,} differential hunk-slice reads")


if __name__ == "__main__":
    main()
