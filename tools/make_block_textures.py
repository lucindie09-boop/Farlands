#!/usr/bin/env python3
"""Generate the block textures the fence/torch/pane batch needs.

Run from the project root:

    python tools/make_block_textures.py

Writes 16x16 RGBA PNGs into textures/blocks/. Idempotent: it rewrites the
files it owns byte for byte, so regenerating is safe and the result can be
diffed.

Why a generator rather than hand-drawn art: the torch's layout is not free.
The per-AABB emitter derives a box's UVs from that box's extent in the cell
(see mesh_builder_faces.cpp), so the 2/16 x 10/16 torch stick samples exactly
x 7..9, y 6..16 of its texture -- the same region the reference model asks for
with an explicit uv. A torch texture drawn anywhere else would show the wrong
pixels, and drawing it by hand once is how that drifts. Everything here is
authored to the region the geometry actually samples:

  torch       sides sample x 7..9, y 6..16; top samples x 7..9, y 7..9;
              bottom (wall torches, and any raised box) samples x 7..9, y 14..16
  chain       sides sample x 7..9, y 0..16
  carpet      a full-face plate, so the whole texture is seen
  glass       a full cube, so the whole texture is seen
  ladder      a plate, so the whole texture is seen

Nothing here is transparent by design: the terrain pass draws with ALPHA fixed
at 1.0 (voxel_shader.gdshader), so an authored alpha channel is ignored and a
"hole" would render as an opaque pixel of whatever colour sits behind it. Art
that needs to read as see-through has to do it with colour.
"""

import struct
import zlib
from pathlib import Path

W = H = 16
OUT_DIR = Path("textures/blocks")


def rgba(r, g, b, a=255):
    return (r, g, b, a)


def blank(fill=(0, 0, 0, 0)):
    return [[fill for _ in range(W)] for _ in range(H)]


def write_png(path, pixels):
    raw = b"".join(
        b"\x00" + b"".join(struct.pack("4B", *pixels[y][x]) for x in range(W))
        for y in range(H)
    )

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)
    print(f"wrote {path} ({len(png)} bytes)")


# ---------------------------------------------------------------------------
# torch: a stick with a flame, laid out for the regions the geometry samples
# ---------------------------------------------------------------------------
def torch_base():
    px = blank()
    stick_dark = rgba(84, 56, 30)
    stick_mid = rgba(112, 76, 42)
    stick_lit = rgba(138, 99, 56)
    # rows 6..9 flame, hottest at the top; rows 10..15 stick
    flame = {
        6: rgba(255, 236, 140),
        7: rgba(255, 206, 74),
        8: rgba(246, 158, 46),
        9: rgba(214, 108, 34),
    }
    for y in range(6, 16):
        for x in range(7, 9):
            if y in flame:
                px[y][x] = flame[y]
            else:
                px[y][x] = stick_lit if x == 7 else stick_mid
    # a darker base so the stick reads as rounded at the bottom
    for x in range(7, 9):
        px[15][x] = stick_dark
    return px


def torch_emit():
    # Black where nothing glows: the emissive map is ADDED to the lit colour, so
    # a non-black pixel outside the flame would light the whole stick.
    px = blank(rgba(0, 0, 0))
    for y, c in ((6, rgba(255, 214, 120)), (7, rgba(255, 176, 72)),
                 (8, rgba(232, 132, 40)), (9, rgba(150, 74, 20))):
        for x in range(7, 9):
            px[y][x] = c
    return px


# ---------------------------------------------------------------------------
# glass: pale, with a frame and a highlight, since alpha cannot carry it
# ---------------------------------------------------------------------------
def glass():
    px = blank(rgba(196, 226, 236))
    body = rgba(180, 214, 228)
    frame = rgba(226, 246, 252)
    inner = rgba(160, 198, 214)
    for y in range(H):
        for x in range(W):
            px[y][x] = body
            if x in (0, W - 1) or y in (0, H - 1):
                px[y][x] = frame
            elif x in (1, W - 2) or y in (1, H - 2):
                px[y][x] = inner
    # two highlight streaks, the usual read for a pane of glass
    for i in range(6):
        px[3 + i][4 + i] = frame
    for i in range(4):
        px[4 + i][11 + i] = frame
    return px


# ---------------------------------------------------------------------------
# ladder: dark backing with rails and rungs
# ---------------------------------------------------------------------------
def ladder():
    back = rgba(58, 41, 23)
    rail = rgba(146, 104, 54)
    rail_dark = rgba(112, 78, 40)
    rung = rgba(128, 90, 46)
    px = blank(back)
    for y in range(H):
        for x in (2, 3):
            px[y][x] = rail
        for x in (12, 13):
            px[y][x] = rail_dark
    for y in (3, 7, 11):
        for x in range(2, 14):
            px[y][x] = rung
        for x in range(2, 14):
            px[y + 1][x] = rgba(100, 70, 36)
    return px


# ---------------------------------------------------------------------------
# chain: links down the x 7..9 column the geometry samples
# ---------------------------------------------------------------------------
def chain():
    px = blank()
    link = rgba(158, 158, 170)
    link_dark = rgba(78, 78, 92)
    link_lit = rgba(196, 196, 208)
    for y in range(H):
        for x in range(7, 9):
            if (y // 2) % 2 == 0:
                px[y][x] = link if x == 7 else link_dark
            else:
                px[y][x] = link_lit if x == 8 else link_dark
    return px


# ---------------------------------------------------------------------------
# carpet: a two-tone weave
# ---------------------------------------------------------------------------
def carpet():
    deep = rgba(122, 44, 44)
    mid = rgba(148, 58, 56)
    lit = rgba(168, 74, 68)
    px = blank(deep)
    for y in range(H):
        for x in range(W):
            px[y][x] = mid if ((x + y) % 4) < 2 else deep
            if x % 4 == 0 or y % 4 == 0:
                px[y][x] = lit if (x % 8 == 0 or y % 8 == 0) else mid
    return px


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    write_png(OUT_DIR / "torch.png", torch_base())
    write_png(OUT_DIR / "torch_emit.png", torch_emit())
    write_png(OUT_DIR / "glass.png", glass())
    write_png(OUT_DIR / "ladder.png", ladder())
    write_png(OUT_DIR / "chain.png", chain())
    write_png(OUT_DIR / "carpet.png", carpet())


if __name__ == "__main__":
    main()
