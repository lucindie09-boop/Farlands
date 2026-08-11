#ifndef FUK_MINECRAFT_MESH_TYPES_HPP
#define FUK_MINECRAFT_MESH_TYPES_HPP
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include "core/chunk_coords.hpp"
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#endif

namespace VoxelEngine {

// Face direction used by meshing
enum class FaceDirection : uint8_t {
    Top    = 0, // +Y
    Bottom = 1, // -Y
    Right  = 2, // +X
    Left   = 3, // -X
    Front  = 4, // +Z
    Back   = 5  // -Z
};

// Vertex layout for chunk meshes
// Position is stored in fixed-point format: uint8_t for x/z (0-31 block coords),
// uint16_t Q8.8 fixed-point for y (8 integer bits + 8 fractional bits)
// This reduces position storage from 12 bytes to 4 bytes while preserving
// fractional precision for top_face_offset (water, slabs, etc.)
struct Vertex {
    uint8_t x, z;         // 2 bytes (block coords 0-31)
    uint16_t y;           // 2 bytes (Q8.8 fixed-point: 8 int + 8 frac)
    int8_t nx, ny, nz;    // 3 bytes
    uint8_t normal_pad;   // 1 byte padding
    float u, v;           // 8 bytes
    uint16_t texture_index; // 2 bytes
    uint8_t ao;           // 1 byte
    uint8_t emissive_index; // 1 byte (emissive texture layer, 0 = none)
    uint8_t light_r;      // 1 byte
    uint8_t light_g;      // 1 byte
    uint8_t light_b;      // 1 byte
    uint8_t sky_light;    // 1 byte
};

// One emitted quad (a greedy merge run or a single face) with its final
// vertex/indices data. Used for partial remeshing: a rebuild carries forward
// every cached quad outside the dirty region (memcpy, no AO/light recompute)
// and only re-runs the greedy/fallback passes over the dirty region.
struct CachedQuad {
    // Emitting block origin (Face anchor) — used for region-intersection tests.
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    FaceDirection direction = FaceDirection::Top;
    bool water = false;
    // Footprint extents in block units ([x, x+ex) × [y, y+ey) × [z, z+ez)).
    // For greedy merged runs the extent covers the whole run; single faces are 1×1×1.
    int32_t ex = 1;
    int32_t ey = 1;
    int32_t ez = 1;
    std::array<Vertex, 4> verts;
    std::array<uint32_t, 6> idx;  // 0-relative local indices (either the normal or flipped winding)
};

// Per-column checksums of a chunk's light grid. A partial rebuild diffs these
// against the previous build's values to find every column whose light changed
// since then, so light-only changes are re-emitted too (not just block edits).
struct MeshLightChecksums {
    std::array<uint32_t,
               static_cast<std::size_t>(CHUNK_WIDTH) * static_cast<std::size_t>(CHUNK_DEPTH)>
        columns{};
};

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
struct PackedBuiltMeshData {
    godot::PackedVector3Array vertices;
    godot::PackedByteArray custom0;  // RGBA8_UNORM: light_r, light_g, light_b, sky_light
    godot::PackedByteArray custom1;  // RGBA8_UNORM: R=texture_index, G=ao, B=normal_encoded, A=emissive_index
    godot::PackedByteArray custom2;  // RG_HALF: u, v
    godot::PackedInt32Array indices;
    bool empty = true;
};
#endif

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_MESH_TYPES_HPP
