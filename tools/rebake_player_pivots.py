#!/usr/bin/env python3
"""Re-bake player.glb so each part's node origin sits on its true pivot.

Blockbench 5.1.6's glTF exporter anchors every part's node at a bottom-corner
position (e.g. arm at (-8.5, 12, 0)) instead of the cube's real pivot, and
glTF has no other place to store a pivot — the node origin IS the rotation
anchor for every importer. This script shifts each mesh's vertices by the
delta between the current node origin and the intended pivot, then moves the
node to the pivot, so:

  * the mesh keeps its exact world position (T_new + V_new == T_old + V_old),
  * Godot (and Blender) now rotate each part around the intended joint.

Pivots come from the Blockbench project (as shown in the Blockbench UI),
centered on each cube's top/center where the joint visually is:
    leg1/leg2 tops   (-2/2, 12, 1.5)   (dead-center on the leg cubes)
    torso center     (0, 18, 0)        (z=0 per Blockbench; cubes are z 1.5)
    arm1/arm2 tops   (-5.5/5.5, 24, 1.5)  (dead-center: cubes span x -7..-4 / 4..7)
    head center      (0, 28, 0)        (z=0 per Blockbench)

This script is idempotent: it uses each node's CURRENT translation as the
origin to shift from, so it can be re-run after editing the PIVOTS table.

Usage:  python tools/rebake_player_pivots.py [path-to-player.glb]
"""

import json
import struct
import sys

GLB = sys.argv[1] if len(sys.argv) > 1 else "player.glb"

# mesh index -> new pivot (node origin to place at; vertices are shifted to
# keep the world placement unchanged).
PIVOTS = {
    0: (-2.0, 12.0, 1.5),     # leg
    1: (2.0, 12.0, 1.5),      # leg2
    2: (0.0, 18.0, 0.0),      # torso
    3: (-5.5, 24.0, 1.5),     # arm
    4: (5.5, 24.0, 1.5),      # arm2
    5: (0.0, 28.0, 0.0),      # head
}

COMP_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
COMP_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
              "MAT2": 4, "MAT3": 9, "MAT4": 16}


def main() -> None:
    raw = open(GLB, "rb").read()
    header, rest = raw[:12], raw[12:]
    json_len, json_type = struct.unpack("<II", rest[:8])
    json_chunk = rest[8:8 + json_len]
    bin_len, bin_type = struct.unpack("<II", rest[8 + json_len:16 + json_len])
    bin_chunk = bytearray(rest[16 + json_len:16 + json_len + bin_len])
    assert json_type == 0x4E4F534A and bin_type == 0x004E4942

    j = json.loads(json_chunk.decode("utf-8"))

    for mesh_idx, new_p in PIVOTS.items():
        node = next(n for n in j["nodes"] if n.get("mesh") == mesh_idx)
        old_t = tuple(node.get("translation", (0, 0, 0)))

        prim = j["meshes"][mesh_idx]["primitives"][0]
        pos_idx = prim["attributes"]["POSITION"]
        acc = j["accessors"][pos_idx]
        bv = j["bufferViews"][acc["bufferView"]]
        comp = COMP_SIZE[acc["componentType"]]
        cc = COMP_COUNT[acc["type"]]
        base = bv["byteOffset"] + acc.get("byteOffset", 0)
        assert acc["componentType"] == 5126 and acc["type"] == "VEC3", "unexpected position format"
        count = acc["count"]

        # shift = old origin - new pivot; adding it to vertices preserves the
        # world placement once the node moves to the pivot.
        shift = tuple(old_t[i] - new_p[i] for i in range(3))
        mins = [1e9, 1e9, 1e9]
        maxs = [-1e9, -1e9, -1e9]
        for v in range(count):
            off = base + v * comp * cc
            x, y, z = struct.unpack_from("<3f", bin_chunk, off)
            nx, ny, nz = x + shift[0], y + shift[1], z + shift[2]
            struct.pack_into("<3f", bin_chunk, off, nx, ny, nz)
            for i, val in enumerate((nx, ny, nz)):
                mins[i] = min(mins[i], val)
                maxs[i] = max(maxs[i], val)

        node["translation"] = list(new_p)
        acc["min"] = mins
        acc["max"] = maxs
        print(f"mesh {mesh_idx} ({node['name']}): origin {old_t} -> {new_p}, "
              f"vertices shifted by {shift}")

    # Reassemble the glb (JSON chunk is re-serialized; BIN chunk rewritten).
    new_json = json.dumps(j, separators=(",", ":")).encode("utf-8")
    pad4 = lambda b: b + b" " * ((4 - len(b) % 4) % 4)
    new_json = pad4(new_json)
    bin_chunk = pad4(bin_chunk)
    total = 12 + 8 + len(new_json) + 8 + len(bin_chunk)
    out = bytearray(struct.pack("<III", 0x46546C67, 2, total))
    out += struct.pack("<II", len(new_json), 0x4E4F534A) + new_json
    out += struct.pack("<II", len(bin_chunk), 0x004E4942) + bin_chunk

    with open(GLB, "wb") as f:
        f.write(out)
    print(f"wrote {GLB} ({total} bytes)")


if __name__ == "__main__":
    main()