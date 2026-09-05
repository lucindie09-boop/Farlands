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
    head neck        (0, 24, 1.5)      (bottom-center of the head cube, so it tilts at the
                                       neck and rotates in place like ModelBiped, which
                                       centers the head box on its pivot axis)

It also insets every face island's UVs by half a texel (see
inset_all_uvs) so nearest sampling at a face edge can never round into the
skin atlas's transparent gutters — the alpha-masked material would otherwise
discard those edge fragments, which reads as hairline see-through at the
model's edges.

Both steps are idempotent: the pivot shift uses each node's CURRENT
translation as the origin to shift from, and the UV inset skips itself once
the UVs leave the 1/64 texel grid.

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
    5: (0.0, 24.0, 1.5),      # head (neck joint, on the cube's center axis)
}

COMP_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
COMP_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
              "MAT2": 4, "MAT3": 9, "MAT4": 16}


# Half-texel UV inset: every face island's UV rect is pulled in by half a
# texel (0.5/64) on all four sides, so nearest sampling at a face's edge can
# never round into the neighbouring transparent gutter texel of the skin
# atlas. Blockbench writes UV corners exactly on the 1/64 texel grid, and at
# the exact island boundary nearest sampling ties with the adjacent gutter —
# the alpha-masked glb material discards those fragments, which reads as
# hairline see-through at the model's edges. Positions and pivots are
# untouched. Idempotent: once UVs leave the 1/64 grid this step has already
# run, so it skips itself (re-running would keep pulling the islands inwards).
def inset_all_uvs(j: dict, bin_chunk: bytearray) -> None:
    half = 0.5 / 64.0

    # Idempotency guard: skip if any UV is already off the 1/64 grid.
    for mesh in j["meshes"]:
        prim = mesh["primitives"][0]
        acc = j["accessors"][prim["attributes"]["TEXCOORD_0"]]
        bv = j["bufferViews"][acc["bufferView"]]
        base = bv["byteOffset"] + acc.get("byteOffset", 0)
        for v in range(acc["count"]):
            u, w = struct.unpack_from("<2f", bin_chunk, base + v * 8)
            if abs(u * 64.0 - round(u * 64.0)) > 1e-4 or \
                    abs(w * 64.0 - round(w * 64.0)) > 1e-4:
                print("UVs already inset (off the 1/64 grid) — skipping inset step")
                return

    for mesh_idx, mesh in enumerate(j["meshes"]):
        prim = mesh["primitives"][0]
        uv_acc = j["accessors"][prim["attributes"]["TEXCOORD_0"]]
        idx_acc = j["accessors"][prim["indices"]]
        uvb = j["bufferViews"][uv_acc["bufferView"]]
        ivb = j["bufferViews"][idx_acc["bufferView"]]
        ubase = uvb["byteOffset"] + uv_acc.get("byteOffset", 0)
        ibase = ivb["byteOffset"] + idx_acc.get("byteOffset", 0)
        assert uv_acc["componentType"] == 5126 and uv_acc["type"] == "VEC2"
        assert idx_acc["componentType"] == 5123, "expected uint16 indices"
        assert uv_acc["count"] == 24 and idx_acc["count"] == 36, \
            "expected a Blockbench box: 24 vertices, 36 indices"

        node = next(n for n in j["nodes"] if n.get("mesh") == mesh_idx)
        insetted = 0
        for f in range(6):
            idxs = [struct.unpack_from("<H", bin_chunk, ibase + 2 * i)[0]
                    for i in range(f * 6, f * 6 + 6)]
            verts = list(dict.fromkeys(idxs))
            assert len(verts) == 4, "expected a quad face"
            uvs = [struct.unpack_from("<2f", bin_chunk, ubase + 8 * v)
                   for v in verts]
            u_min = min(p[0] for p in uvs)
            u_max = max(p[0] for p in uvs)
            v_min = min(p[1] for p in uvs)
            v_max = max(p[1] for p in uvs)
            for v, (u, w) in zip(verts, uvs):
                nu = (u_min + half) if abs(u - u_min) < 1e-9 else (u_max - half)
                nw = (v_min + half) if abs(w - v_min) < 1e-9 else (v_max - half)
                struct.pack_into("<2f", bin_chunk, ubase + 8 * v, nu, nw)
                insetted += 1
        # Recompute the accessor bounds from the insetted vertices.
        all_uvs = [struct.unpack_from("<2f", bin_chunk, ubase + 8 * v)
                   for v in range(uv_acc["count"])]
        uv_acc["min"] = [min(p[0] for p in all_uvs), min(p[1] for p in all_uvs)]
        uv_acc["max"] = [max(p[0] for p in all_uvs), max(p[1] for p in all_uvs)]
        print(f"mesh {mesh_idx} ({node['name']}): insetted {insetted} face UVs "
              f"by 0.5/64 texel")


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

    inset_all_uvs(j, bin_chunk)

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