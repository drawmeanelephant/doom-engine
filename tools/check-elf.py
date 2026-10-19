#!/usr/bin/env python3
"""tools/check-elf.py — DOOM.ELF loader-contract check, stated engine-side.

The VirelaiOS loader accepts a statically linked AArch64 ELF64
little-endian image with at most three PT_LOAD headers ordered R+X /
optional R / RW-last, file ranges disjoint and forward-ordered,
page-aligned vaddrs strictly below 0x1000_0000, e_entry inside segment
0's initialized file bytes, sum(filesz) <= 32 MiB and sum(memsz) <=
64 MiB. This checker restates those interface facts so the smoke leg
runs without a VirelaiOS checkout; the canonical host checker is
tools/lib/elf_rules.py there, and both read the same rules.
"""

import struct
import sys

MAX_IMAGE = 32 * 1024 * 1024
MAP_MAX = 64 * 1024 * 1024
GAP_BASE_MAX = 0x1000_0000
MAX_SEGMENTS = 3
EM_AARCH64 = 183


def check(path):
    fails = []
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        return ["not an ELF"]
    if data[4] != 2 or data[5] != 1:
        fails.append("not a 64-bit little-endian ELF")
    if struct.unpack_from("<H", data, 0x12)[0] != EM_AARCH64:
        fails.append("not AArch64")
    if len(data) > MAX_IMAGE:
        fails.append("file %d > 32 MiB" % len(data))

    entry = struct.unpack_from("<Q", data, 0x18)[0]
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phes = struct.unpack_from("<H", data, 0x36)[0]
    pnum = struct.unpack_from("<H", data, 0x38)[0]

    segs = []
    for i in range(pnum):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phes)
        if t == 1:
            segs.append((fl, va, off, fsz, msz))
    segs.sort(key=lambda s: s[1])

    if not segs:
        fails.append("no PT_LOAD segments")
        return fails
    if len(segs) > MAX_SEGMENTS:
        fails.append("%d PT_LOAD > %d" % (len(segs), MAX_SEGMENTS))
    if sum(s[3] for s in segs) > MAX_IMAGE:
        fails.append("sum filesz > 32 MiB")
    if sum(s[4] for s in segs) > MAP_MAX:
        fails.append("sum memsz > 64 MiB")

    prev_end = 0
    for fl, va, off, fsz, msz in segs:
        if va & 4095:
            fails.append("segment at %#x unaligned" % va)
        if va + msz > GAP_BASE_MAX:
            fails.append("segment at %#x crosses gap_base_max" % va)
        if off < prev_end:
            fails.append("file ranges out of order/overlapping")
        prev_end = off + fsz
        if msz < fsz:
            fails.append("memsz < filesz at %#x" % va)

    if segs[0][0] & 2:
        fails.append("segment 0 writable")
    if not (segs[-1][0] & 2):
        fails.append("last segment not writable")
    for fl, va, off, fsz, msz in segs[1:-1]:
        if fl & 2:
            fails.append("middle segment writable")
    if not (segs[0][1] <= entry < segs[0][1] + segs[0][3]):
        fails.append("entry %#x outside segment 0 initialized bytes" % entry)
    return fails


if __name__ == "__main__":
    path = sys.argv[1]
    fails = check(path)
    data = open(path, "rb").read()
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phes = struct.unpack_from("<H", data, 0x36)[0]
    pnum = struct.unpack_from("<H", data, 0x38)[0]
    n_load = sum(1 for i in range(pnum)
                 if struct.unpack_from("<I", data, phoff + i * phes)[0] == 1)
    print("%s: %d bytes, %d PT_LOAD segments" % (path, len(data), n_load))
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1 if fails else 0)
