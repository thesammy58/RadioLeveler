# SPDX-License-Identifier: MIT
# Copyright (c) 2026 thesammy58
# Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
"""Builds package/RadioLeveler.package: one resource overriding the game's "stereo" voice template
(VOCE 0x029E333B : 0x001407EC : 0x3C18C0DE65EC32C9) with Compressor1 (Co10) inserted
after the time-stretch stage and before the filters/sends."""
import os, struct, sys

TEMPLATE = (b"1\r\n"
            b"SnP1 Rch0 Rsp0 Pau0 TSt0 Co10 LI20 HI20 Sen0[reverb] Sen0\r\n"
            b"lowpass=LI20:0,96000\r\n"
            b"hipass=HI20:0,0\r\n"
            b"wetlevel=Sen0[reverb]:0,0\r\n"
            b"mixpitch=Rsp0:0\r\n")
KEY = (0x029E333B, 0x001407EC, 0x3C18C0DE65EC32C9)

def build(resources):
    body = bytearray(b"\0" * 96)
    entries = []
    for (t, g, i), data in resources:
        off = len(body); body += data
        entries.append((t, g, i, off, len(data)))
    idxpos = len(body)
    idx = struct.pack("<I", 0)  # no constant fields
    for t, g, i, off, size in entries:
        idx += struct.pack("<IIIIIIIHH", t, g, i >> 32, i & 0xFFFFFFFF, off, size | 0x80000000, size, 0, 1)
    body += idx
    hdr = bytearray(96)
    hdr[0:4] = b"DBPF"
    struct.pack_into("<II", hdr, 4, 2, 0)           # version 2.0
    struct.pack_into("<I", hdr, 36, len(entries))   # index entry count
    struct.pack_into("<I", hdr, 44, len(idx))       # index size
    struct.pack_into("<I", hdr, 60, 3)              # index minor version
    struct.pack_into("<I", hdr, 64, idxpos)         # index position
    body[0:96] = hdr
    return bytes(body)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "package", "RadioLeveler.package")
open(out, "wb").write(build([(KEY, TEMPLATE)]))
print("wrote", out)
