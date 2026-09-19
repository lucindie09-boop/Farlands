#!/usr/bin/env python3
"""Regenerates the byte fixtures embedded in tests/schematic_test_data.hpp.

The fixtures are compressed and encoded here, by python's own gzip/zlib, and by
a hand-written big-endian NBT writer -- never by the C++ reader these tests are
checking. Running this rewrites the hex literals in the header in place, so the
embedded bytes can never drift from what the encoders actually produce.

    python tools/make_schematic_fixtures.py [--check]

--check only reports whether the header is already up to date.
"""

import gzip
import io
import re
import struct
import sys
import zlib

W, H, L = 4, 3, 5
HEADER = "tests/schematic_test_data.hpp"


def tag(tag_type, name, payload):
    return bytes([tag_type]) + struct.pack(">H", len(name)) + name.encode() + payload


def sh(value):
    return struct.pack(">h", value)


def s(value):
    return struct.pack(">H", len(value)) + value.encode()


def barr(values):
    return struct.pack(">i", len(values)) + bytes(values)


def compound(entries):
    return b"".join(entries) + b"\x00"


def schematic(w, h, l, blocks, data, add=None, root=b"Schematic"):
    entries = [
        tag(2, "Width", sh(w)),
        tag(2, "Height", sh(h)),
        tag(2, "Length", sh(l)),
        tag(7, "Blocks", barr(blocks)),
        tag(7, "Data", barr(data)),
        tag(8, "Materials", s("Alpha")),
    ]
    if add is not None:
        entries.append(tag(7, "AddBlocks", barr(add)))
    return bytes([10]) + struct.pack(">H", len(root)) + root + compound(entries)


def gz(payload):
    """Deterministic gzip: mtime zero, so the fixture never changes by itself."""
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as handle:
        handle.write(payload)
    return buf.getvalue()


def build_fixtures():
    cells = [(x, y, z) for y in range(H) for z in range(L) for x in range(W)]
    blocks = [1 + (x * 3 + y * 5 + z * 7) % 200 for (x, y, z) in cells]
    data_bytes = [(x + y * 2 + z * 3) & 0xF for (x, y, z) in cells]

    nibbles = bytearray((len(data_bytes) + 1) // 2)
    for i, value in enumerate(data_bytes):
        if i % 2 == 0:
            nibbles[i >> 1] |= value & 0xF
        else:
            nibbles[i >> 1] |= (value & 0xF) << 4

    high_ids = [0x100 + ((x * 7 + y * 3 + z * 5) % 90) for (x, y, z) in cells]
    add = bytearray((len(high_ids) + 1) // 2)
    for i, block_id in enumerate(high_ids):
        if i % 2 == 0:
            add[i >> 1] |= (block_id >> 8) & 0xF
        else:
            add[i >> 1] |= ((block_id >> 8) & 0xF) << 4

    plain = b"the quick brown fox jumps over the lazy dog. " * 12
    dynamic = gz(plain)
    corrupt = bytearray(dynamic)
    corrupt[len(corrupt) // 2] ^= 0xFF

    raw = zlib.compressobj(9, zlib.DEFLATED, -15)
    return {
        "kGzipDynamic": dynamic,
        "kGzipCorrupt": bytes(corrupt),
        "kZlibDynamic": zlib.compress(plain),
        "kRawDeflate": raw.compress(plain) + raw.flush(),
        "kSchemByte": gz(schematic(W, H, L, blocks, data_bytes)),
        "kSchemNibble": gz(schematic(W, H, L, blocks, bytes(nibbles))),
        "kSchemAdd": gz(schematic(W, H, L, [b & 0xFF for b in high_ids], data_bytes, bytes(add))),
        "kSchemBadData": gz(schematic(W, H, L, blocks, data_bytes[:10])),
        "kSchemWrongRoot": gz(schematic(W, H, L, blocks, data_bytes, root=b"NotASchematic")),
        "kSchemMissingDims": gz(bytes([10]) + struct.pack(">H", 9) + b"Schematic" +
                               compound([tag(7, "Blocks", barr(blocks))])),
    }


def main():
    check_only = "--check" in sys.argv[1:]
    fixtures = build_fixtures()
    # Read as bytes-ish: newline="" keeps the file's own line endings, so only
    # the hex literals change when this rewrites the header.
    text = open(HEADER, encoding="utf-8", newline="").read()
    original = text
    newline = "\r\n" if "\r\n" in text else "\n"

    for name, blob in fixtures.items():
        lines = newline.join('    "%s"' % blob[i:i + 16].hex() for i in range(0, len(blob), 16))
        pattern = re.compile(r"(inline const char\* " + name + r"Hex =\r?\n)(.*?)(;)", re.S)
        text, count = pattern.subn(lambda m: m.group(1) + lines + m.group(3), text)
        if count != 1:
            print("expected exactly one %sHex literal in %s, found %d" % (name, HEADER, count))
            return 1

    if text == original:
        print("%s is already up to date (%d fixtures)" % (HEADER, len(fixtures)))
        return 0
    if check_only:
        print("%s is stale -- rerun without --check" % HEADER)
        return 1
    open(HEADER, "w", encoding="utf-8", newline="").write(text)
    print("rewrote %d fixtures in %s" % (len(fixtures), HEADER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
