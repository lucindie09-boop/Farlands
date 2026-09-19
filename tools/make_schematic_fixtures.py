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
# The palette fixtures are a different size, and their cell order is checked
# against the same kind of coordinate formula as the classic ones.
SPONGE_W, SPONGE_H, SPONGE_L = 3, 2, 2
HEADER = "tests/schematic_test_data.hpp"

SPONGE_V2_STATES = {
    "minecraft:air": 0,
    "minecraft:stone": 1,
    "minecraft:oak_stairs[facing=east,half=top,waterlogged=false]": 2,
    "minecraft:spruce_slab[type=top]": 3,
}

# Written out of index order on purpose: the palette is keyed by its indices, not
# by the order its entries were printed in.
SPONGE_V3_STATES = {
    "minecraft:sandstone": 2,
    "minecraft:air": 0,
    "minecraft:birch_planks": 1,
}


def tag(tag_type, name, payload):
    raw = name.encode() if isinstance(name, str) else name
    return bytes([tag_type]) + struct.pack(">H", len(raw)) + raw + payload


def sh(value):
    return struct.pack(">h", value)


def s(value):
    return struct.pack(">H", len(value)) + value.encode()


def barr(values):
    return struct.pack(">i", len(values)) + bytes(values)


def compound(entries):
    return b"".join(entries) + b"\x00"


def root(name, payload):
    """A root compound, whose name may be empty (writers do that, and wrap the
    build in a child compound instead)."""
    return bytes([10]) + struct.pack(">H", len(name)) + name + payload


def i32(value):
    return struct.pack(">i", value)


def int_array(values):
    return struct.pack(">i", len(values)) + b"".join(struct.pack(">i", v) for v in values)


def compound_list(items):
    return struct.pack(">B", 10) + struct.pack(">i", len(items)) + b"".join(items)


def palette(mapping):
    """A palette compound: state name -> index, entries in whatever order the
    mapping was written in."""
    return compound([tag(3, name, i32(index)) for name, index in mapping.items()])


def varints(values):
    out = bytearray()
    for value in values:
        while True:
            byte = value & 0x7F
            value >>= 7
            if value:
                out.append(byte | 0x80)
            else:
                out.append(byte)
                break
    return bytes(out)


def classic_entries(w, h, l, blocks, data, add=None):
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
    return entries


def schematic(w, h, l, blocks, data, add=None, root_name=b"Schematic"):
    return root(root_name, compound(classic_entries(w, h, l, blocks, data, add)))


def wrapped_classic(w, h, l, blocks, data):
    """The same build inside an unnamed root, as some writers emit it."""
    return root(b"", compound([tag(10, b"Schematic", compound(classic_entries(w, h, l, blocks, data)))]))


def sponge_v2(w, h, l, states, index_of, wrap=True, truncate=0):
    """Flat palette file: Palette and BlockData at the root.

    `truncate` drops that many cells from the end of the cell data, which is how
    the "stops short of the volume" fixture is built.
    """
    cells = [index_of(x, y, z) for y in range(h) for z in range(l) for x in range(w)]
    if truncate:
        cells = cells[:-truncate]
    entries = [
        tag(3, "Version", i32(2)),
        tag(3, "DataVersion", i32(3465)),
        tag(2, "Width", sh(w)),
        tag(2, "Height", sh(h)),
        tag(2, "Length", sh(l)),
        tag(11, "Offset", int_array([-1, 0, 2])),
        tag(3, "PaletteMax", i32(len(states))),
        tag(10, "Palette", palette(states)),
        tag(7, "BlockData", barr(varints(cells))),
        compound_list_key("Entities", []),
        tag(10, "Metadata", compound([tag(8, "Name", s("test"))])),
    ]
    if wrap:
        return root(b"", compound([tag(10, b"Schematic", compound(entries))]))
    return root(b"Schematic", compound(entries))


def sponge_v3(w, h, l, states, index_of, block_entities=(), biome_cells=8):
    """Nested palette file: Palette and Data under "Blocks", plus sections to skip."""
    cells = [index_of(x, y, z) for y in range(h) for z in range(l) for x in range(w)]
    blocks = compound([
        tag(3, "PaletteMax", i32(len(states))),
        tag(10, "Palette", palette(states)),
        tag(7, "Data", barr(varints(cells))),
        compound_list_key("BlockEntities", block_entities),
    ])
    biomes = compound([
        tag(3, "PaletteMax", i32(1)),
        tag(10, "Palette", palette({"minecraft:plains": 0})),
        tag(7, "Data", barr(bytes([0] * biome_cells))),
    ])
    entries = [
        tag(3, "Version", i32(3)),
        tag(3, "DataVersion", i32(4556)),
        tag(2, "Width", sh(w)),
        tag(2, "Height", sh(h)),
        tag(2, "Length", sh(l)),
        tag(11, "Offset", int_array([4, -2, 7])),
        tag(10, "Blocks", blocks),
        tag(10, "Biomes", biomes),
        compound_list_key("Entities", []),
        tag(10, "Metadata", compound([tag(8, "Name", s("test")), tag(3, "Origin", i32(0))])),
    ]
    return root(b"Schematic", compound(entries))


def compound_list_key(name, items):
    return tag(9, name, compound_list(items))


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
        "kSchemWrongRoot": gz(schematic(W, H, L, blocks, data_bytes, root_name=b"NotASchematic")),
        "kSchemWrapped": gz(wrapped_classic(W, H, L, blocks, data_bytes)),
        "kSchemAddPadded": gz(schematic(W, H, L, [b & 0xFF for b in high_ids], data_bytes,
                                        bytes(add) + b"\x00")),
        "kSchemNibblePadded": gz(schematic(W, H, L, blocks, bytes(nibbles) + b"\x00")),
        "kSchemSpongeV2": gz(sponge_v2(SPONGE_W, SPONGE_H, SPONGE_L, SPONGE_V2_STATES,
                                       lambda x, y, z: (x + 2 * y + z) % 4)),
        "kSchemSpongeV3": gz(sponge_v3(SPONGE_W, SPONGE_H, SPONGE_L, SPONGE_V3_STATES,
                                       lambda x, y, z: (x + 3 * y + 2 * z) % 3,
                                       block_entities=[compound([
                                           tag(8, "Id", s("minecraft:chest")),
                                           tag(11, "Pos", int_array([1, 0, 1])),
                                       ])])),
        "kSchemSpongeVarint": gz(sponge_v2(2, 1, 1, {"minecraft:stone": 128},
                                           lambda x, y, z: 128, wrap=False)),
        "kSchemSpongeShort": gz(sponge_v2(SPONGE_W, SPONGE_H, SPONGE_L, SPONGE_V2_STATES,
                                          lambda x, y, z: 0, truncate=1)),
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
