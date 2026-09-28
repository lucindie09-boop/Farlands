#ifndef FARLANDS_MESH_TYPES_HPP
#define FARLANDS_MESH_TYPES_HPP
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
// Position is stored in Q8.8 fixed-point format for all three axes:
// 8 integer bits + 8 fractional bits, allowing sub-block precision
// for non-full blocks (fences, stairs, walls, slabs).
// Every byte of it is written. The two bytes between normal_pad and u would
// otherwise be padding the compiler leaves alone, and normal_pad itself is not
// assigned anywhere - and this struct is compared byte for byte by the meshing
// tests (tests/test_shape_resolver.cpp) and memcpy'd into cached quads, so a byte
// nothing writes is a byte that makes two identical meshes differ. The first
// version of those tests passed on one compiler and failed on another for exactly
// that reason. The default member initialisers are what makes `Vertex v;` - the
// way every mesher builds one - deterministic.
struct Vertex {
    uint16_t x, y, z;    // 6 bytes (Q8.8 fixed-point: 8 int + 8 frac)
    int8_t nx, ny, nz;    // 3 bytes
    uint8_t normal_pad = 0;  // 1 byte, kept for the layout: nothing reads it
    uint8_t reserved0 = 0;   // the two bytes the compiler used to leave alone
    uint8_t reserved1 = 0;
    float u, v;           // 8 bytes
    uint16_t texture_index; // 2 bytes
    uint8_t ao;           // 1 byte
    uint8_t emissive_index; // 1 byte (emissive texture layer, 0 = none)
    uint8_t light_r;      // 1 byte
    uint8_t light_g;      // 1 byte
    uint8_t light_b;      // 1 byte
    uint8_t sky_light;    // 1 byte
};

// The vertex is a byte-for-byte comparable block, so it may have no hole and no
// tail: its size is exactly the sum of its members. Adding a field that needs
// more alignment than the layout already provides breaks this line rather than the
// meshing tests, which is the point of writing it here.
static_assert(sizeof(Vertex) == 4 * sizeof(uint16_t) + 3 * sizeof(int8_t) +
                                   9 * sizeof(uint8_t) + 2 * sizeof(float),
              "Vertex has padding: see the note above its definition");

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

#endif // FARLANDS_MESH_TYPES_HPP
