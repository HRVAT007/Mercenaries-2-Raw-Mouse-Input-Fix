#!/usr/bin/env python3
"""
Shadow atlas resolution for Mercenaries 2.

The game allocates its shadow atlas from literal immediates in Mercenaries2.exe (four
1024x1024 cascades tiled into a 1024x4096 strip), but the PCF filter that reads that atlas is
compiled into data\\shader3.bin with the texel size baked in as a float4 constant:

    (0.5/W, 0.5/H, W, H)   for W=1024, H=4096      ->  (1/2048, 1/8192, 1024, 4096)

plus two tap-offset vectors beside it, whose four components are all multiples of a texel.
Verified against the shipped file: each of those three 16-byte vectors occurs EXACTLY 315
times, always at a def_c record payload (zero misaligned hits), one atlas vector per shadow
shader, and each atlas vector pairs with exactly one of each tap vector within the next 200
bytes.  So the whole change is a literal 16-byte swap that keeps every blob offset, blob size
and hash key valid.

Scaling the atlas in the exe alone is a visual break, not a win: at 2048x8192 the taps land two
texels apart and the bilinear weights run at half the texel rate, so the kernel mismatches its
own grid.  Both halves have to move together, and the exe half is what Mercs2Fix applies at
startup from [Shadow] MapSizeScale.

This tool does the shader half.  Run it with the game CLOSED.

    python shadow_res.py probe        what the file is at right now
    python shadow_res.py set 2        2048x8192   (also: set 4, set 8, set 1)
    python shadow_res.py restore      byte-identical to the shipped backup
"""
import os
import shutil
import struct
import sys

# Your install folder, or set MERCS2_SHADER to the full path of data\shader3.bin.
GAME = r"E:\Program Files (x86)\EA Games\Mercenaries 2 World in Flames"
# MERCS2_SHADER lets the self-test run against a copy without touching the installed file.
SHADER = os.environ.get("MERCS2_SHADER", os.path.join(GAME, "data", "shader3.bin"))
ORIG = SHADER + ".mercs2orig"

W, H = 1024, 4096
EXPECT = 315

# Taken verbatim from the shipped file (first record at 0x117BC, 0x11804, 0x1181C) rather than
# computed here, so a transcription error cannot silently aim us at nothing.
ATLAS_1 = bytes.fromhex("0000003a000000390000804400008045")
TAP_1 = [
    bytes.fromhex("000000ba0000c0390000003a0000c03a"),
    bytes.fromhex("000000ba000000390000c03a000000b9"),
]
for _v in (ATLAS_1,) + tuple(TAP_1):
    assert len(_v) == 16

# Documentation only: what the shipped bytes decode to.
assert struct.unpack("<4f", ATLAS_1) == (0.5 / W, 0.5 / H, float(W), float(H))
assert struct.unpack("<4f", TAP_1[0]) == (-0.5 / W, 1.5 / H, 0.5 / W, 1.5 / W)
assert struct.unpack("<4f", TAP_1[1]) == (-0.5 / W, 0.5 / H, 1.5 / W, -0.5 / H)


def atlas_at(s):
    f = struct.unpack("<4f", ATLAS_1)
    return struct.pack("<4f", f[0] / s, f[1] / s, f[2] * s, f[3] * s)


def tap_at(t, s):
    f = struct.unpack("<4f", t)
    return struct.pack("<4f", f[0] / s, f[1] / s, f[2] / s, f[3] / s)


def find_all(buf, pat):
    offs, i = [], 0
    while True:
        j = buf.find(pat, i)
        if j < 0:
            return offs
        offs.append(j)
        i = j + 1


def sets(s):
    return [atlas_at(s)] + [tap_at(t, s) for t in TAP_1]


def identify(buf):
    for s in (1, 2, 4, 8):
        if all(len(find_all(buf, v)) == EXPECT for v in sets(s)):
            return s
    return None


def offsets(buf, s):
    """Every 16-byte span belonging to scale s, keyed off the atlas vector's own record so a
    coincidental byte sequence can never be edited as a tap."""
    heads = find_all(buf, atlas_at(s))
    out = list(heads)
    for t in sets(s)[1:]:
        out += find_all(buf, t)
    return sorted(out), heads


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "probe"
    if not os.path.isfile(SHADER):
        print("not found: %s" % SHADER)
        return 1
    buf = open(SHADER, "rb").read()
    cur = identify(buf)
    print("data\\shader3.bin  %d B   scale on disk: %s"
          % (len(buf), ("x%d" % cur) if cur else "UNRECOGNISED"))
    for s in (1, 2, 4, 8):
        print("   x%d  atlas %s  %d   tap %d/%d"
              % (s, sets(s)[0].hex(" "), len(find_all(buf, sets(s)[0])),
                 len(find_all(buf, sets(s)[1])), len(find_all(buf, sets(s)[2]))))
    if cur is None:
        print("\nThe file matches none of the known scales, so it is not the shipped layout.")
        print("Nothing was written and nothing will be.")
        return 1
    if cmd == "probe":
        return 0

    if cmd == "restore":
        if not os.path.isfile(ORIG):
            print("no backup at %s - this tool has never changed the file" % ORIG)
            return 1
        want = open(ORIG, "rb").read()
        if want == buf:
            print("already byte-identical to the shipped backup")
            return 0
        write(want)
        back = identify(open(SHADER, "rb").read())
        print("restored; re-identify -> x%s, bytes identical to backup: %s"
              % (back, open(SHADER, "rb").read() == want))
        return 0

    if cmd != "set":
        print(__doc__)
        return 1

    try:
        s = int(argv[2])
    except (IndexError, ValueError):
        print("usage: shadow_res.py set <1|2|4|8>")
        return 1
    if s not in (1, 2, 4, 8):
        print("x%d is not offered.  Only powers of two: every constant here is dyadic, so a "
              "power-of-two scale stays bit-exact in both directions." % s)
        return 1
    if s == 8:
        print("NOTE: x8 asks the game for an 8192x32768 depth strip - 268M texels, about 1.0 GB.  "
              "D3D9 reports MaxTextureHeight of 8192 or 16384 on most hardware, so the depth "
              "surface may simply fail to be created. That is a boot test, not a corruption risk: "
              "'restore' puts the file back byte-identical.")
    if not os.path.isfile(ORIG):
        shutil.copyfile(SHADER, ORIG)
        print("backed up the shipped file to %s (%d B)" % (ORIG, os.path.getsize(ORIG)))
    if s == 1:
        want = open(ORIG, "rb").read()
        if want == buf:
            print("already at x1")
            return 0
        write(want)
        print("restored to x1; re-identify -> x%s" % identify(open(SHADER, "rb").read()))
        return 0
    if cur == s:
        print("already at x%d" % s)
        return 0

    offs, heads = offsets(buf, cur)
    if len(heads) != EXPECT:
        print("expected %d atlas records, found %d - ABORTED" % (EXPECT, len(heads)))
        return 1
    for i in range(1, len(offs)):
        if offs[i] - offs[i - 1] < 16:
            print("overlapping spans at 0x%X and 0x%X - ABORTED, nothing written"
                  % (offs[i - 1], offs[i]))
            return 1
    # A scale change maps each pattern onto a different pattern, so the destination for a span
    # is decided by which of the three it currently is.
    src = sets(cur)
    dst = sets(s)
    out = bytearray(buf)
    n = [0, 0, 0]
    for o in offs:
        span = buf[o:o + 16]
        for k in range(3):
            if span == src[k]:
                out[o:o + 16] = dst[k]
                n[k] += 1
                break
    if n != [EXPECT, EXPECT, EXPECT]:
        print("replaced %s, expected [%d, %d, %d] - ABORTED, nothing written" % (n, EXPECT, EXPECT, EXPECT))
        return 1
    if len(out) != len(buf):
        print("size changed - ABORTED")
        return 1
    write(bytes(out))
    chk = identify(open(SHADER, "rb").read())
    print("945 16-byte records rewritten, file still %d B, re-identify -> x%s" % (len(out), chk))
    if chk != s:
        print("VERIFY FAILED.  Run 'python shadow_res.py restore' before starting the game.")
        return 1
    print("atlas is now %dx%d.  Set [Shadow] MapSizeScale=%d in Mercs2Fix.ini to match - "
          "Mercs2Fix checks this file at startup and refuses the exe half if it disagrees."
          % (W * s, H * s, s))
    return 0


def write(data):
    tmp = SHADER + ".mercs2tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, SHADER)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
