#!/usr/bin/env python3
"""Generate a small Farlands texture pack with visibly tinted built-in textures.

Used as a repo test fixture (examples/texture_pack/demo/) and for the in-game
sanity check: activating it must visibly recolor grass/water/wood/leaves/dirt.

Usage:
  python tools/make_demo_pack.py <output_dir>
  e.g. python tools/make_demo_pack.py user_packs   (then copy into user://packs)

The generator is dependency-free: a ~60 line PNG reader/writer (zlib + struct)
decodes the built-in res://textures/blocks PNGs, scales each channel, and
re-encodes them under the engine texture names.
"""

import json
import os
import struct
import sys
import zlib

SIG = b"\x89PNG\r\n\x1a\n"

BUILTIN_DIR = "textures/blocks"

# Engine texture name -> (r, g, b) channel multipliers. Wood top does not exist
# in the built-ins, so it is derived from wood_side below.
JOBS = {
    "grass_top": (1.0, 1.6, 0.5),
    "grass_side": (1.0, 1.5, 0.6),
    "water": (0.3, 0.9, 1.4),
    "wood_side": (1.5, 0.9, 0.4),
    "leaves": (0.6, 1.2, 0.7),
    "dirt": (1.15, 1.0, 0.85),
}

PACK_NAME = "demo"
PACK_AUTHOR = "Farlands test fixture"


def _read_chunks(data):
    assert data[:8] == SIG, "not a PNG"
    pos = 8
    chunks = []
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8].decode("ascii")
        chunks.append((ctype, data[pos + 8:pos + 8 + length]))
        pos += 12 + length
        if ctype == "IEND":
            break
    return chunks


def png_decode(data):
    """Decode PNG bytes -> (w, h, bpp, raw pixel bytes)."""
    chunks = _read_chunks(data)
    ihdr = next(c for c in chunks if c[0] == "IHDR")[1]
    w, h, bd, ct, _comp, _filt, inter = struct.unpack(">IIBBBBB", ihdr)
    if bd != 8 or inter != 0:
        raise ValueError(f"unsupported PNG {bd}-bit, interlace {inter}")
    bpp = {6: 4, 2: 3, 0: 1}.get(ct)
    if bpp is None:
        raise ValueError(f"unsupported PNG colour type {ct}")
    idat = b"".join(p for c, p in chunks if c == "IDAT")
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ft == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
            elif ft != 0:
                raise ValueError(f"bad PNG filter {ft}")
        out += line
        prev = line
    return w, h, bpp, bytes(out)


def read_png(path, attempts=5):
    """Read+decode a PNG with retries for transient truncated reads."""
    last = None
    for _ in range(attempts):
        try:
            with open(path, "rb") as f:
                d = f.read()
            if len(d) != os.path.getsize(path):
                print("DEBUG short read", path, len(d), "of", os.path.getsize(path))
            return png_decode(d)
        except (zlib.error, struct.error, ValueError, AssertionError, OSError) as e:
            last = e
    raise last


def png_encode(w, h, pixels):
    def chunk(ctype, payload):
        body = ctype + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    raw = b"".join(b"\x00" + bytes(pixels[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    return SIG + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def tint(w, h, bpp, pix, r, g, b):
    out = bytearray()
    for i in range(0, len(pix), bpp):
        pr = pix[i]
        if bpp >= 3:
            pg, pb = pix[i + 1], pix[i + 2]
        else:
            pg = pb = pr
        a = pix[i + 3] if bpp == 4 else 255
        out += bytes((min(255, int(pr * r)), min(255, int(pg * g)), min(255, int(pb * b)), a))
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        print("usage: python tools/make_demo_pack.py <output_dir>")
        sys.exit(1)
    out = sys.argv[1]
    pack_dir = os.path.join(out, PACK_NAME)
    tex_dir = os.path.join(pack_dir, "textures")
    os.makedirs(tex_dir, exist_ok=True)

    for name, mult in JOBS.items():
        src = os.path.join(BUILTIN_DIR, name + ".png")
        if not os.path.isfile(src):
            print(f"  missing built-in {src} - skipping {name}")
            continue
        w, h, bpp, pix = read_png(src)
        rgba = tint(w, h, bpp, pix, *mult)
        with open(os.path.join(tex_dir, name + ".png"), "wb") as f:
            f.write(png_encode(w, h, rgba))
        print(f"  {name}: {w}x{h} tinted")

    # wood_top has no built-in: derive it from wood_side with a brighter, even
    # tint so the log caps are visibly distinct from the sides.
    side = os.path.join(BUILTIN_DIR, "wood_side.png")
    if os.path.isfile(side):
        w, h, bpp, pix = read_png(side)
        rgba = tint(w, h, bpp, pix, 1.35, 1.05, 0.6)
        with open(os.path.join(tex_dir, "wood_top.png"), "wb") as f:
            f.write(png_encode(w, h, rgba))
        print(f"  wood_top: {w}x{h} derived from wood_side")

    with open(os.path.join(pack_dir, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({
            "name": PACK_NAME,
            "schema": 1,
            "min_supported": 1,
            "max_supported": 1,
            "base_resolution": 16,
            "author": PACK_AUTHOR,
        }, f, indent=2)

    print(f"demo pack written to {pack_dir}")


if __name__ == "__main__":
    main()
