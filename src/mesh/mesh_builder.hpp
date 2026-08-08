#ifndef FUK_MINECRAFT_MESH_BUILDER_HPP
#define FUK_MINECRAFT_MESH_BUILDER_HPP
#include <atomic>

#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "mesh/mesh_types.hpp"
#include "mesh/chunk_neighbor_accessor.hpp"
#include "core/performance_timer.hpp"
#include "mesh/ambient_occlusion.hpp"
#include <vector>
#include <array>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <cmath>

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
// Matches the GPU function: level<=2 → 0.0025 + level*0.01875,
// level>2 → 0.04 + (level-2)^1.5 * 0.008
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
    return arr;
}();

class MeshBuilder {
public:

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

    MeshBuilder() {
        vertices.reserve(kVertexReserve);
        indices.reserve(kIndexReserve);
        water_vertices.reserve(kVertexReserve);
        water_indices.reserve(kIndexReserve);
    }

    void clear();

    void build_mesh(const ChunkData& chunk,
                    const ChunkData* neighbor_x_neg = nullptr,
                    const ChunkData* neighbor_x_pos = nullptr,
                    const ChunkData* neighbor_y_neg = nullptr,
                    const ChunkData* neighbor_y_pos = nullptr,
                    const ChunkData* neighbor_z_neg = nullptr,
                    const ChunkData* neighbor_z_pos = nullptr,
                    const ChunkData* neg_x_neg_z = nullptr,
                    const ChunkData* neg_x_pos_z = nullptr,
                    const ChunkData* pos_x_neg_z = nullptr,
                    const ChunkData* pos_x_pos_z = nullptr,
                    const ChunkData* neg_x_neg_y = nullptr,
                    const ChunkData* pos_x_neg_y = nullptr,
                    const ChunkData* neg_x_pos_y = nullptr,
                    const ChunkData* pos_x_pos_y = nullptr,
                    const ChunkData* neg_y_neg_z = nullptr,
                    const ChunkData* neg_y_pos_z = nullptr,
                    const ChunkData* pos_y_neg_z = nullptr,
                    const ChunkData* pos_y_pos_z = nullptr,
                    const ChunkData* neg_x_neg_y_neg_z = nullptr,
                    const ChunkData* pos_x_neg_y_neg_z = nullptr,
                    const ChunkData* neg_x_pos_y_neg_z = nullptr,
                    const ChunkData* pos_x_pos_y_neg_z = nullptr,
                    const ChunkData* neg_x_neg_y_pos_z = nullptr,
                    const ChunkData* pos_x_neg_y_pos_z = nullptr,
                    const ChunkData* neg_x_pos_y_pos_z = nullptr,
                    const ChunkData* pos_x_pos_y_pos_z = nullptr);

    const std::vector<Vertex>& get_vertices() const { return vertices; }
    const std::vector<uint32_t>& get_indices() const { return indices; }
    const std::vector<Vertex>& get_water_vertices() const { return water_vertices; }
    const std::vector<uint32_t>& get_water_indices() const { return water_indices; }
    size_t get_vertex_count() const { return vertices.size(); }
    size_t get_index_count() const { return indices.size(); }
    size_t get_triangle_count() const { return indices.size() / 3; }

    static PerformanceTimer& get_perf_timer() { return perf_timer; }
    static double get_avg_vertices_per_chunk() {
        uint64_t tv = total_vertices.load(std::memory_order_relaxed);
        uint64_t tc = total_chunks.load(std::memory_order_relaxed);
        return tc > 0 ? static_cast<double>(tv) / tc : 0.0;
    }
    static void reset_vertex_tracking() {
        total_vertices.store(0, std::memory_order_relaxed);
        total_chunks.store(0, std::memory_order_relaxed);
    }

static GreedyVerticalStatsSnapshot get_greedy_vertical_stats();
static void reset_greedy_vertical_stats();

    void set_greedy_enabled(bool enabled) { passive_greedy_enabled = enabled; }
    bool is_greedy_enabled() const { return passive_greedy_enabled; }

    void set_smooth_lighting(bool enabled) {
smooth_lighting_enabled = enabled;
if (enabled) passive_greedy_enabled = false;
}
bool is_smooth_lighting_enabled() const { return smooth_lighting_enabled; }

    void set_detail_level(float level);
    float get_detail_level() const { return detail_level_; }
    int get_stride_xz() const { return stride_xz_; }

    // Far-mode: emit a heightmap-only mesh (one merged top quad per macro
    // column, no side/underside faces, no AO, no caves, no water columns).
    // Used for chunks far enough that only their silhouette matters; the
    // region-merging pipeline renders these as merged far meshes.
    void set_far_mode(bool enabled) { far_mode_ = enabled; }
    bool is_far_mode() const { return far_mode_; }

    void set_subchunk_bounds(const SubChunkBounds& bounds) { active_bounds = bounds; }
    const SubChunkBounds& get_subchunk_bounds() const { return active_bounds; }

    // Partial remesh: rebuilds the mesh for only the dirty region (given by
    // dirty_bounds, expanded by 1 for the AO/light/culling neighborhood) plus
    // any columns whose light changed since the previous build (prev_light).
    // Everything else is carried forward from prev_quads, so the output is
    // bit-identical to a full build of the same chunk. The previous build's
    // quad list + light checksums must be passed in (see get_quads() /
    // get_light_checksums()).
    void build_mesh_incremental(const ChunkData& chunk,
                                const std::vector<CachedQuad>& prev_quads,
                                const MeshLightChecksums& prev_light,
                                const SubChunkBounds& dirty_bounds,
                                const NeighborPtrs& neighbors = NeighborPtrs());

    // Quad list emitted by the last build (full builds: the entire chunk;
    // incremental builds: carried + re-emitted). Feed this into the next
    // build_mesh_incremental call to enable partial remeshing.
    const std::vector<CachedQuad>& get_quads() const { return quads; }

    // Per-column light-grid checksums of the last build.
    const MeshLightChecksums& get_light_checksums() const { return light_checksums_; }

    // The re-emit region actually used by the last build_mesh_incremental call
    // (expanded by the AO/light ring, plus any light-changed columns). Useful for
    // tests/debugging: with no light change it should equal expand_bounds() of
    // the dirty bbox, NOT a region pulled down toward the chunk origin.
    const SubChunkBounds& get_last_partial_bounds() const { return last_partial_bounds_; }

    // Test-only: fill solid_cache with an invalid sentinel before the next
    // partial build so any read of an unpopulated cell (a bug in the tight
    // populate box or a solid_cache read not routed through solid_at()) shows up
    // as garbage instead of a stale-but-plausible block ID.
    void debug_poison_solid_cache() { debug_poison_solid_cache_ = true; }

    // When disabled, emitted faces are not recorded into the quad cache.
    // MeshManager disables this for chunks far from the player (which can never
    // be edited) to keep the cache memory bounded.
    void set_quad_recording(bool enabled) { record_quads_ = enabled; }

private:
    // -------------------------------------------------------------------------
    // Constants
    // -------------------------------------------------------------------------
    static constexpr int SC_W = CHUNK_WIDTH + 2;  // 34 with 32-wide chunks
    static constexpr int SC_D = CHUNK_DEPTH + 2;  // 34 with 32-deep chunks

    static constexpr float kWaterSurfaceDrop = 0.12f;
    static constexpr float kLoweredBlockOffset = 0.0625f;  // 1/16
    static constexpr int   kMaxGreedyMergeDistance = 32;
    static constexpr size_t kVertexReserve = static_cast<size_t>(CHUNK_VOLUME) * 6 * 4;
    static constexpr size_t kIndexReserve  = kVertexReserve * 3 / 2;

    // -------------------------------------------------------------------------
    // Lookup tables
    // -------------------------------------------------------------------------
    static constexpr float kFaceVertices[6][4][3] = {
        // Top (+Y)
        {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
        // Bottom (-Y)
        {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
        // Right (+X)
        {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}},
        // Left (-X)
        {{0, 0, 1}, {0, 0, 0}, {0, 1, 0}, {0, 1, 1}},
        // Front (+Z)
        {{1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {1, 1, 1}},
        // Back (-Z)
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}
    };

    static constexpr int32_t kDirectionOffsets[6][3] = {
        {0, 1, 0},   // Top
        {0, -1, 0},  // Bottom
        {1, 0, 0},   // Right
        {-1, 0, 0},  // Left
        {0, 0, 1},   // Front
        {0, 0, -1}   // Back
    };

    static constexpr float kFaceNormals[6][3] = {
        {0, 1, 0},   // Top
        {0, -1, 0},  // Bottom
        {1, 0, 0},   // Right
        {-1, 0, 0},  // Left
        {0, 0, 1},   // Front
        {0, 0, -1}   // Back
    };

    static constexpr float kFaceUVs[4][2] = {
        {0, 1}, // Bottom-Left
        {1, 1}, // Bottom-Right
        {1, 0}, // Top-Right
        {0, 0}  // Top-Left
    };

    static constexpr std::array<FaceDirection, 6> kAllDirections = {
        FaceDirection::Top, FaceDirection::Bottom,
        FaceDirection::Right, FaceDirection::Left,
        FaceDirection::Front, FaceDirection::Back
    };

    static constexpr std::array<FaceDirection, 4> kSideDirections = {
        FaceDirection::Right, FaceDirection::Left,
        FaceDirection::Front, FaceDirection::Back
    };

    // Maps FaceDirection enum (Top=0, Bottom=1, Right=2, Left=3, Front=4, Back=5)
    // to texture_indices convention (+X=0, -X=1, +Y=2, -Y=3, +Z=4, -Z=5).
    static constexpr int kDirToTexIdx[6] = {2, 3, 0, 1, 4, 5};

    // -------------------------------------------------------------------------
    // Member data
    // -------------------------------------------------------------------------
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Vertex> water_vertices;
    std::vector<uint32_t> water_indices;
    // Layout is [y][z][x] (not [x][z][y]) so that scans with x as the inner
    // loop variable (passive_greedy_mesh_horizontal) read sequential memory
    // instead of striding by SC_W * SC_D each step.
    // Stores BlockID of the block at chunk position (x-1, y, z-1) for interior
    // entries, or the neighbor-chunk block for boundary entries.
    std::array<std::array<std::array<uint16_t, SC_W>, SC_D>, CHUNK_HEIGHT> solid_cache{};

    // Neighbor chunk accessor for cross-chunk block/light access.
    ChunkNeighborAccessor accessor;

AmbientOcclusion ao;

    static PerformanceTimer perf_timer;
    static std::atomic<uint64_t> total_vertices;
    static std::atomic<uint64_t> total_chunks;
    static std::atomic<uint64_t> greedy_v_merge_attempts;
    static std::atomic<uint64_t> greedy_v_merge_successes;
    static std::atomic<uint64_t> greedy_v_reject_ao_mismatch;
    static std::atomic<uint64_t> greedy_v_reject_ao_occlusion;
    static std::atomic<uint64_t> greedy_v_reject_light_mismatch;
    static std::atomic<uint64_t> greedy_v_reject_rotation_mismatch;
    static std::atomic<uint64_t> greedy_v_reject_block_mismatch;
    static std::atomic<uint64_t> greedy_v_reject_distance_limit;
    static std::atomic<uint64_t> greedy_v_lod_cells_visited;
    static std::atomic<uint64_t> greedy_v_lod_cells_skipped_air;
    static std::atomic<uint64_t> greedy_v_lod_faces_culled;
    static std::atomic<uint64_t> greedy_v_lod_faces_emitted;

    bool passive_greedy_enabled = false;
bool smooth_lighting_enabled = false;
GreedyVerticalStatsSnapshot greedy_v_stats_local{};

    float detail_level_ = 1.0f;
    int stride_xz_ = 1;
    bool far_mode_ = false;

    SubChunkBounds active_bounds;

    // Partial-remesh state. When partial_mode_ is true the greedy/fallback
    // passes only iterate the expanded dirty region (partial_bounds_).
    bool partial_mode_ = false;
    SubChunkBounds partial_bounds_;
    // Copy of partial_bounds_ persisted past the end of build_mesh_incremental
    // (which resets partial_bounds_), for tests/debugging.
    SubChunkBounds last_partial_bounds_;

    // Solid_cache populate box (world coords, clamped to the chunk). In partial
    // mode only this tight box is populated; solid_at() live-reads everything
    // outside it. Empty {} degrades to all-live reads (correct, just slow).
    SubChunkBounds solid_bounds_;
    // Test-only poison-sentinel flag (see debug_poison_solid_cache()).
    bool debug_poison_solid_cache_ = false;

    // Quad list emitted by the last build (carried forward + newly emitted).
    std::vector<CachedQuad> quads;
    bool record_quads_ = true;
    MeshLightChecksums light_checksums_;

    const BlockRegistry* registry_ = nullptr;

    // -------------------------------------------------------------------------
    // Static helpers (small, keep inline)
    // -------------------------------------------------------------------------
    static bool is_side_face(FaceDirection direction) {
        return direction == FaceDirection::Right ||
               direction == FaceDirection::Left ||
               direction == FaceDirection::Front ||
               direction == FaceDirection::Back;
    }

    static void apply_special_block_offsets(float corners[4][3], BlockID block_id, FaceDirection dir);

    static int compute_face_rotation(int32_t x, int32_t y, int32_t z, FaceDirection dir) {
        int32_t wx = x + static_cast<int32_t>(dir) * 7;
        int32_t wz = z + y * 3;
        return ((wx * 13 + wz * 17) & 3);
    }

    static void apply_uv_rotation(float& u, float& v, int rotation) {
        if (rotation == 0) return;
        float uc = u - 0.5f;
        float vc = v - 0.5f;
        switch (rotation) {
            case 1: u = 0.5f - vc; v = 0.5f + uc; break; //  90°
            case 2: u = 0.5f - uc; v = 0.5f - vc; break; // 180°
            case 3: u = 0.5f + vc; v = 0.5f - uc; break; // 270°
            default: break;
        }
    }

    static bool block_face_rotates(BlockID id, int tex_idx) noexcept {
        switch (id) {
            case BlockIDs::AIR:
            case BlockIDs::SURFACE_WATER:
            case BlockIDs::WATER:
            case BlockIDs::LEAVES:
            case BlockIDs::LIGHT_BLOCK:
            case BlockIDs::LIGHT_RED:
            case BlockIDs::LIGHT_GREEN:
            case BlockIDs::LIGHT_BLUE:
                return false;
            case BlockIDs::GRASS:
                return tex_idx == 2 || tex_idx == 3;
            default:
                return true;
        }
    }

    static int get_face_rotation(BlockID block_id, int32_t x, int32_t y, int32_t z,
                                  FaceDirection dir, int dir_index) {
if (is_side_face(dir)) return 0;
        if (!block_face_rotates(block_id, kDirToTexIdx[dir_index])) return 0;
        return compute_face_rotation(x, y, z, dir);
    }

    bool should_cull_against_neighbor(const ChunkData& chunk, BlockID current, BlockID neighbor,
FaceDirection direction, int32_t x, int32_t y, int32_t z,
const BlockRegistry& registry) const;

    bool boundary_face_fully_occluded(const ChunkData& current_chunk, const ChunkData* neighbor,
                                       FaceDirection dir, int32_t x, int32_t y, int32_t z,
                                       int32_t stride, BlockID current_block,
                                       const BlockRegistry& registry) const;

    // -------------------------------------------------------------------------
    // Face emission (heavy — defined in .cpp)
    // -------------------------------------------------------------------------
    void add_face(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                  int32_t x, int32_t y, int32_t z,
                  FaceDirection direction, BlockID block_id, const BlockRegistry& registry);

    void add_greedy_face(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                         const Face& face, uint16_t face_light_key, int rotation,
                         const float ao[4], const BlockRegistry& registry);

    // -------------------------------------------------------------------------
    // Partial-remesh and shared build helpers (defined in .cpp)
    // -------------------------------------------------------------------------
    // Copy a carried quad into the output buffers and quad list (no AO/light
    // recompute — the final vertices are already baked).
    void append_quad(const CachedQuad& q);

    // Drop test: does a cached quad fall inside the region the current build
    // will re-emit? Depends on the active emitter mode (greedy vs fallback).
    bool should_drop_quad(const CachedQuad& q) const;

    static SubChunkBounds expand_bounds(const SubChunkBounds& b);

    void compute_light_checksums(const ChunkData& chunk);

    void init_accessor(const ChunkData& chunk, const NeighborPtrs& neighbors);
    void populate_solid_cache(const ChunkData& chunk, const BlockRegistry& registry);

    // Reads a solid_cache cell by its raw indices (y, z-index, x-index; world
    // x/z = index - 1). In partial mode cells outside solid_bounds_ are read live
    // from the chunk/neighbor data instead — this keeps the merge passes' full-
    // axis run-boundary scans exact while solid_cache is only populated for the
    // tight box around the dirty region.
    BlockID solid_at(int32_t y, int32_t zi, int32_t xi) const;
    void emit_faces(const ChunkData& chunk, const BlockRegistry& registry);
    void accumulate_greedy_stats();

    // Far-mode heightmap-only mesh emitter (see set_far_mode).
    void build_far_mesh(const ChunkData& chunk, const BlockRegistry& registry);

    // -------------------------------------------------------------------------
    // Passive greedy meshing (heavy — defined in .cpp)
    // -------------------------------------------------------------------------
    void flush_horizontal_merge(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                int32_t z_start, int32_t z_end,
                                int32_t y, int32_t x, FaceDirection direction,
                                BlockID block_id, uint16_t light_key, int rotation,
                                const float ao[4], const BlockRegistry& registry);

    void passive_greedy_mesh_horizontal(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                        FaceDirection direction, const BlockRegistry& registry);

    // -------------------------------------------------------------------------
    // Passive vertical greedy meshing (1D Y-axis merge for side faces)
    // -------------------------------------------------------------------------
    void flush_vertical_merge(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                int32_t y_start, int32_t y_end,
                                int32_t x, int32_t z, FaceDirection direction,
                                BlockID block_id, uint16_t light_key, int rotation,
                                const float ao[4], const BlockRegistry& registry);

    void passive_greedy_mesh_vertical(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                        const BlockRegistry& registry);

};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_MESH_BUILDER_HPP