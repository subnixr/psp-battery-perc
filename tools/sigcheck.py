#!/usr/bin/env python3
"""Check on a PC that the plugin's signatures match decrypted firmware modules.

usage: sigcheck.py FILE...   (vshmain.prx and impose_XXg.prx, decrypted ELFs)

Mirrors the scans in src/patch_vsh.c and src/patch_impose.c. Each signature
must match exactly once, and the impose clock routine and the code around the
vsh call site must have the expected instructions at the offsets the plugin
reads.
"""
import struct
import sys

VSH_SIG = [(0x02002021, 0xFFFFFFFF), (0x24060064, 0xFFFFFFFF),
           (0x0C000000, 0xFC000000), (0x03A02821, 0xFFFFFFFF)]
IMPOSE_SIG = [(0x240601C2, 0xFFFFFFFF), (0x240501E0, 0xFFFFFFFF),
              (0x8E070060, 0xFFFFFFFF), (0x26040000, 0xFFFF0000),
              (0x00C7280B, 0xFFFFFFFF), (0x00003821, 0xFFFFFFFF),
              (0x0C000000, 0xFC000000), (0x24060005, 0xFFFFFFFF)]
# words from the vsh call site -> (value, mask)
KEPT = {-26: (0x3C110000, 0xFFFF0000), -17: (0x26240000, 0xFFFF0000),
        -15: (0x94820002, 0xFFFFFFFF), 66: (0x94830008, 0xFFFFFFFF)}
# offset in clock routine -> (value, mask)
CLOCK = {0x008: (0x3C150000, 0xFFFF0000), 0x03C: (0x8EA30000, 0xFFFF0000),
         0x274: (0x2C8E000A, 0xFFFFFFFF), 0x298: (0x8D0B0000, 0xFFFF0000),
         0x2B0: (0x0C000000, 0xFC000000), 0x2B4: (0x2407000F, 0xFFFFFFFF),
         0x324: (0x8D390000, 0xFFFF0000)}


def load_text(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF":
        sys.exit("%s: not a decrypted ELF" % path)
    phoff, = struct.unpack_from("<I", d, 0x1C)
    _, off, vaddr, _, filesz = struct.unpack_from("<5I", d, phoff)
    words = struct.unpack_from("<%dI" % (filesz // 4), d, off)
    return d, vaddr, words


def find(words, sig):
    return [i for i in range(len(words) - len(sig) + 1)
            if all(words[i + j] & m == v for j, (v, m) in enumerate(sig))]


def check(path):
    d, vaddr, words = load_text(path)
    if b"sceImpose_Driver" in d:
        hits = find(words, IMPOSE_SIG)
        if len(hits) != 1:
            return "impose call site: %d matches" % len(hits)
        clk = ((words[hits[0] + 6] & 0x03FFFFFF) << 2) - vaddr
        for off, (v, m) in CLOCK.items():
            if words[(clk + off) // 4] & m != v:
                return "clock routine +0x%X: %08X" % (off, words[(clk + off) // 4])
        s16 = lambda w: (w & 0xFFFF) - ((w & 0x8000) << 1)
        return "ok  call 0x%X clock 0x%X digits even/odd %d/%d" % (
            (hits[0] + 6) * 4 + vaddr, clk + vaddr,
            s16(words[(clk + 0x324) // 4]), s16(words[(clk + 0x298) // 4]))
    hits = find(words, VSH_SIG)
    if len(hits) != 1:
        return "vsh call site: %d matches" % len(hits)
    for off, (v, m) in KEPT.items():
        if words[hits[0] + off] & m != v:
            return "vsh kept time %+d: %08X" % (off, words[hits[0] + off])
    return "ok  call 0x%X" % ((hits[0] + 2) * 4 + vaddr)


def main():
    bad = 0
    for path in sys.argv[1:]:
        res = check(path)
        bad += not res.startswith("ok")
        print("%-60s %s" % (path[-60:], res))
    sys.exit(1 if bad or len(sys.argv) < 2 else 0)


if __name__ == "__main__":
    main()
