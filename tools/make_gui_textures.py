#!/usr/bin/env python3
"""Generate the air HUD's bubble sprite.

Run from the project root:

    python tools/make_gui_textures.py

Writes one 9x9 RGBA PNG into textures/gui/. Idempotent: it rewrites the file it
owns byte for byte, so regenerating is safe and the result can be diffed.

Why a generator rather than hand-drawn art: the bubble is a single shape whose
only interesting part is the ring and the highlight, and it is drawn at the same
9-unit size the heart is (see scripts/healthbar.gd) so the two rows share a
pitch. Everything is authored to that grid: the outline sits on the radius-4
ring, the fill inside it, and the highlight is the two-three texels in the
upper-left that make it read as a bubble rather than a dot. A depleted bubble
reuses the same sprite under a dark modulate (scripts/airbar.gd), so there is no
second "empty" file to drift from this one.

Alpha is real here (unlike the block textures): a GUI Control draws with normal
blending, so transparency is the background.
"""

import struct
import zlib
from pathlib import Path

W = H = 9
CX = CY = 4  # the pixel centre of a 9x9 sprite
OUT_DIR = Path("textures/gui")

OUTLINE = (22, 44, 74, 255)   # the ring
FILL = (150, 212, 242, 255)   # the body
HIGHLIGHT = (255, 255, 255, 255)


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


def bubble():
    px = blank()
    for y in range(H):
        for x in range(W):
            dx, dy = x - CX, y - CY
            d2 = dx * dx + dy * dy
            if d2 <= 9:            # inside the radius-3 inner ring: the body
                px[y][x] = FILL
            elif d2 <= 16:         # the radius-4 outer ring: the outline
                px[y][x] = OUTLINE
    # Upper-left glint, inside the body.
    for x, y in ((2, 2), (3, 2), (2, 3)):
        px[y][x] = HIGHLIGHT
    return px


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    write_png(OUT_DIR / "bubble.png", bubble())


if __name__ == "__main__":
    main()
