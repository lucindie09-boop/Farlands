#ifndef FARLANDS_MESH_BUILDER_TYPES_HPP
#define FARLANDS_MESH_BUILDER_TYPES_HPP
#include <array>
#include <cmath>
#include <cstdint>

#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/chunk_data.hpp"
#include "mesh/mesh_types.hpp"

// The value types the mesher works in: the faces it queues, the neighbour bundle
// it samples through, and the bounds it is asked to fill. Split out of
// mesh_builder.hpp so that header stays about the builder itself.
namespace VoxelEngine {

// Face data for greedy meshing
struct Face {
    int32_t x, y, z;         // Position
    FaceDirection direction; // Face direction
    BlockID block_id;        // Block type
    int32_t u_max;           // Extension in u direction
    int32_t v_max;           // Extension in v direction
};

// Per-face data for mesh extraction (used by raycasting / face queries)
struct FaceData {
    FaceDirection direction;
    BlockPos      position;
    BlockID       block_id;
    bool          visible;

    FaceData() noexcept
        : direction(FaceDirection::Top),
          block_id(BlockIDs::AIR),
          visible(false) {}
};

// Pre-computed block-light brightness curve for levels 0–15.
// The shape is the GPU's own: level<=2 → 0.0025 + level*0.01875,
// level>2 → 0.04 + (level-2)^1.5 * 0.008 -- normalised so that the TOP of the
// curve, a cell in full sun or beside a level-15 light block, is exactly 1.0.
//
// That normalisation is a contract the shaders rely on: light is a fraction OF THE
// TEXEL, so a fully lit surface renders at the colour it was authored with and
// nothing brighter (see shaders/item_lighting.gdshaderinc and voxel_shader). The
// un-normalised curve topped out at 0.415, which is why every full-sun surface
// used to need the shaders' flat +0.15 "bounce" to look lit at all -- and why that
// bounce could push a texel past its own colour wherever the cell was already
// bright. probes/probe_sky_brightness.gd reads a known albedo back off the frame
// and holds both ends of this.
inline const std::array<float, 16> kBlockBrightness = []() {
    std::array<float, 16> arr{};
    for (int i = 0; i < 16; i++) {
        if (i <= 2) {
            arr[i] = 0.0025f + i * 0.01875f;
        } else {
            float x = static_cast<float>(i - 2);
            arr[i] = 0.04f + x * std::sqrt(x) * 0.008f;
        }
    }
    const float top = arr[15];
    for (float& value : arr) {
        value /= top;
    }
    return arr;
}();

// Bounding box for sub-chunk meshing. Only blocks within [x_min, x_max) etc.
// will be processed. Default is the full chunk.
struct SubChunkBounds {
    int32_t x_min = 0;
    int32_t x_max = CHUNK_WIDTH;
    int32_t y_min = 0;
    int32_t y_max = CHUNK_HEIGHT;
    int32_t z_min = 0;
    int32_t z_max = CHUNK_DEPTH;
};

// Bundle of the 26 neighboring ChunkData pointers used by mesh building.
// Used by build_mesh_incremental to keep its signature manageable.
// The default ctor is user-declared (defined out-of-line as `= default`) so
// that Clang accepts the `const NeighborPtrs& neighbors = NeighborPtrs()`
// default argument below: the implicit ctor's NSDMIs are not available until
// the enclosing class is complete, which rejects the default argument.
struct NeighborPtrs {
    NeighborPtrs();
    const ChunkData* neg_x = nullptr;
    const ChunkData* pos_x = nullptr;
    const ChunkData* neg_y = nullptr;
    const ChunkData* pos_y = nullptr;
    const ChunkData* neg_z = nullptr;
    const ChunkData* pos_z = nullptr;
    const ChunkData* neg_x_neg_z = nullptr;
    const ChunkData* neg_x_pos_z = nullptr;
    const ChunkData* pos_x_neg_z = nullptr;
    const ChunkData* pos_x_pos_z = nullptr;
    const ChunkData* neg_x_neg_y = nullptr;
    const ChunkData* pos_x_neg_y = nullptr;
    const ChunkData* neg_x_pos_y = nullptr;
    const ChunkData* pos_x_pos_y = nullptr;
    const ChunkData* neg_y_neg_z = nullptr;
    const ChunkData* neg_y_pos_z = nullptr;
    const ChunkData* pos_y_neg_z = nullptr;
    const ChunkData* pos_y_pos_z = nullptr;
    const ChunkData* neg_x_neg_y_neg_z = nullptr;
    const ChunkData* pos_x_neg_y_neg_z = nullptr;
    const ChunkData* neg_x_pos_y_neg_z = nullptr;
    const ChunkData* pos_x_pos_y_neg_z = nullptr;
    const ChunkData* neg_x_neg_y_pos_z = nullptr;
    const ChunkData* pos_x_neg_y_pos_z = nullptr;
    const ChunkData* neg_x_pos_y_pos_z = nullptr;
    const ChunkData* pos_x_pos_y_pos_z = nullptr;
};

struct GreedyVerticalStatsSnapshot {
uint64_t merge_attempts = 0;
uint64_t merge_successes = 0;
uint64_t reject_ao_mismatch = 0;
uint64_t reject_ao_occlusion = 0;
uint64_t reject_light_mismatch = 0;
uint64_t reject_rotation_mismatch = 0;
uint64_t reject_block_mismatch = 0;
uint64_t reject_distance_limit = 0;
uint64_t lod_cells_visited = 0;
uint64_t lod_cells_skipped_air = 0;
uint64_t lod_faces_culled = 0;
uint64_t lod_faces_emitted = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_MESH_BUILDER_TYPES_HPP
