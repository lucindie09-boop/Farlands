#include "mesh/mesh_builder.hpp"
#include <algorithm>

namespace VoxelEngine {

// Defined out-of-line so the NSDMIs of this nested struct are only instantiated
// after MeshBuilder is complete (see the header for the Clang rationale).
MeshBuilder::NeighborPtrs::NeighborPtrs() = default;

// bru bru bru

PerformanceTimer MeshBuilder::perf_timer;
std::atomic<uint64_t> MeshBuilder::total_vertices{0};
std::atomic<uint64_t> MeshBuilder::total_chunks{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_merge_attempts{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_merge_successes{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_reject_ao_mismatch{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_reject_ao_occlusion{0}; 
std::atomic<uint64_t> MeshBuilder::greedy_v_reject_light_mismatch{0}; 
std::atomic<uint64_t> MeshBuilder::greedy_v_reject_rotation_mismatch{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_reject_block_mismatch{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_reject_distance_limit{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_lod_cells_visited{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_lod_cells_skipped_air{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_lod_faces_culled{0};
std::atomic<uint64_t> MeshBuilder::greedy_v_lod_faces_emitted{0};


MeshBuilder::GreedyVerticalStatsSnapshot MeshBuilder::get_greedy_vertical_stats() {
    GreedyVerticalStatsSnapshot stats;
    stats.merge_attempts = greedy_v_merge_attempts.load(std::memory_order_relaxed);
    stats.merge_successes = greedy_v_merge_successes.load(std::memory_order_relaxed);
    stats.reject_ao_mismatch = greedy_v_reject_ao_mismatch.load(std::memory_order_relaxed);
    stats.reject_ao_occlusion = greedy_v_reject_ao_occlusion.load(std::memory_order_relaxed);
    stats.reject_light_mismatch = greedy_v_reject_light_mismatch.load(std::memory_order_relaxed);
    stats.reject_rotation_mismatch = greedy_v_reject_rotation_mismatch.load(std::memory_order_relaxed);
    stats.reject_block_mismatch = greedy_v_reject_block_mismatch.load(std::memory_order_relaxed);
    stats.reject_distance_limit = greedy_v_reject_distance_limit.load(std::memory_order_relaxed);
    stats.lod_cells_visited = greedy_v_lod_cells_visited.load(std::memory_order_relaxed);
    stats.lod_cells_skipped_air = greedy_v_lod_cells_skipped_air.load(std::memory_order_relaxed);
    stats.lod_faces_culled = greedy_v_lod_faces_culled.load(std::memory_order_relaxed);
    stats.lod_faces_emitted = greedy_v_lod_faces_emitted.load(std::memory_order_relaxed);
    return stats;
}

void MeshBuilder::reset_greedy_vertical_stats() {
greedy_v_merge_attempts.store (0, std::memory_order_relaxed); greedy_v_merge_successes.store (0, std::memory_order_relaxed); greedy_v_reject_ao_mismatch.store (0, std::memory_order_relaxed); greedy_v_reject_ao_occlusion.store (0, std::memory_order_relaxed);
greedy_v_reject_light_mismatch.store(0, std::memory_order_relaxed);
greedy_v_reject_rotation_mismatch.store(0, std::memory_order_relaxed);
greedy_v_reject_block_mismatch.store(0, std::memory_order_relaxed);
greedy_v_reject_distance_limit.store(0, std::memory_order_relaxed);
greedy_v_lod_cells_visited.store(0, std::memory_order_relaxed);
greedy_v_lod_cells_skipped_air.store(0, std::memory_order_relaxed);
greedy_v_lod_faces_culled.store(0, std::memory_order_relaxed);
greedy_v_lod_faces_emitted.store(0, std::memory_order_relaxed);
}

void MeshBuilder::set_detail_level(float level) {
    detail_level_ = std::clamp(level, 0.125f, 1.0f);
    int raw = static_cast<int>(std::round(1.0f / detail_level_));
    stride_xz_ = 1;
    while (stride_xz_ * 2 <= raw && stride_xz_ < 8) stride_xz_ *= 2;
}

void MeshBuilder::clear() {
    vertices.clear();
    indices.clear();
    water_vertices.clear();
    water_indices.clear();
    vertices.reserve(kVertexReserve);
    indices.reserve(kIndexReserve);
    water_vertices.reserve(kVertexReserve);
    water_indices.reserve(kIndexReserve);
    greedy_v_stats_local = {};
    active_bounds = {};
    quads.clear();
    partial_mode_ = false;
    partial_bounds_ = {};
    last_partial_bounds_ = {};
    solid_bounds_ = {};
}

void MeshBuilder::init_accessor(const ChunkData& chunk, const NeighborPtrs& neighbors) {
    accessor.center = &chunk;
    accessor.neg_x = neighbors.neg_x;
    accessor.pos_x = neighbors.pos_x;
    accessor.neg_y = neighbors.neg_y;
    accessor.pos_y = neighbors.pos_y;
    accessor.neg_z = neighbors.neg_z;
    accessor.pos_z = neighbors.pos_z;
    accessor.neg_x_neg_z = neighbors.neg_x_neg_z;
    accessor.neg_x_pos_z = neighbors.neg_x_pos_z;
    accessor.pos_x_neg_z = neighbors.pos_x_neg_z;
    accessor.pos_x_pos_z = neighbors.pos_x_pos_z;
    accessor.neg_x_neg_y = neighbors.neg_x_neg_y;
    accessor.pos_x_neg_y = neighbors.pos_x_neg_y;
    accessor.neg_x_pos_y = neighbors.neg_x_pos_y;
    accessor.pos_x_pos_y = neighbors.pos_x_pos_y;
    accessor.neg_y_neg_z = neighbors.neg_y_neg_z;
    accessor.neg_y_pos_z = neighbors.neg_y_pos_z;
    accessor.pos_y_neg_z = neighbors.pos_y_neg_z;
    accessor.pos_y_pos_z = neighbors.pos_y_pos_z;
    accessor.neg_x_neg_y_neg_z = neighbors.neg_x_neg_y_neg_z;
    accessor.pos_x_neg_y_neg_z = neighbors.pos_x_neg_y_neg_z;
    accessor.neg_x_pos_y_neg_z = neighbors.neg_x_pos_y_neg_z;
    accessor.pos_x_pos_y_neg_z = neighbors.pos_x_pos_y_neg_z;
    accessor.neg_x_neg_y_pos_z = neighbors.neg_x_neg_y_pos_z;
    accessor.pos_x_neg_y_pos_z = neighbors.pos_x_neg_y_pos_z;
    accessor.neg_x_pos_y_pos_z = neighbors.neg_x_pos_y_pos_z;
    accessor.pos_x_pos_y_pos_z = neighbors.pos_x_pos_y_pos_z;
}

void MeshBuilder::accumulate_greedy_stats() {
    total_vertices.fetch_add(vertices.size(), std::memory_order_relaxed);

    greedy_v_merge_attempts.fetch_add(greedy_v_stats_local.merge_attempts, std::memory_order_relaxed);
    greedy_v_merge_successes.fetch_add(greedy_v_stats_local.merge_successes, std::memory_order_relaxed);
    greedy_v_reject_ao_mismatch.fetch_add(greedy_v_stats_local.reject_ao_mismatch, std::memory_order_relaxed);
    greedy_v_reject_ao_occlusion.fetch_add(greedy_v_stats_local.reject_ao_occlusion, std::memory_order_relaxed);
    greedy_v_reject_light_mismatch.fetch_add(greedy_v_stats_local.reject_light_mismatch, std::memory_order_relaxed);
    greedy_v_reject_rotation_mismatch.fetch_add(greedy_v_stats_local.reject_rotation_mismatch, std::memory_order_relaxed);
    greedy_v_reject_block_mismatch.fetch_add(greedy_v_stats_local.reject_block_mismatch, std::memory_order_relaxed);
    greedy_v_reject_distance_limit.fetch_add(greedy_v_stats_local.reject_distance_limit, std::memory_order_relaxed);
    greedy_v_lod_cells_visited.fetch_add(greedy_v_stats_local.lod_cells_visited, std::memory_order_relaxed);
    greedy_v_lod_cells_skipped_air.fetch_add(greedy_v_stats_local.lod_cells_skipped_air, std::memory_order_relaxed);
    greedy_v_lod_faces_culled.fetch_add(greedy_v_stats_local.lod_faces_culled, std::memory_order_relaxed);
    greedy_v_lod_faces_emitted.fetch_add(greedy_v_stats_local.lod_faces_emitted, std::memory_order_relaxed);
}

void MeshBuilder::build_mesh(const ChunkData& chunk,
                             const ChunkData* neighbor_x_neg,
                             const ChunkData* neighbor_x_pos,
                             const ChunkData* neighbor_y_neg,
                             const ChunkData* neighbor_y_pos,
                             const ChunkData* neighbor_z_neg,
                             const ChunkData* neighbor_z_pos,
                             const ChunkData* neg_x_neg_z,
                             const ChunkData* neg_x_pos_z,
                             const ChunkData* pos_x_neg_z,
                             const ChunkData* pos_x_pos_z,
                             const ChunkData* neg_x_neg_y,
                             const ChunkData* pos_x_neg_y,
                             const ChunkData* neg_x_pos_y,
                             const ChunkData* pos_x_pos_y,
                             const ChunkData* neg_y_neg_z,
                             const ChunkData* neg_y_pos_z,
                             const ChunkData* pos_y_neg_z,
                             const ChunkData* pos_y_pos_z,
                             const ChunkData* neg_x_neg_y_neg_z,
                             const ChunkData* pos_x_neg_y_neg_z,
                             const ChunkData* neg_x_pos_y_neg_z,
                             const ChunkData* pos_x_pos_y_neg_z,
                             const ChunkData* neg_x_neg_y_pos_z,
                             const ChunkData* pos_x_neg_y_pos_z,
                             const ChunkData* neg_x_pos_y_pos_z,
                             const ChunkData* pos_x_pos_y_pos_z) {
    ScopedTimer build_timer(perf_timer, TimerID::BuildMesh);

    clear();
    if (!registry_) { registry_ = &BlockRegistry::get_instance(); }
    const BlockRegistry& registry = *registry_;

    NeighborPtrs neighbors;
    neighbors.neg_x = neighbor_x_neg;
    neighbors.pos_x = neighbor_x_pos;
    neighbors.neg_y = neighbor_y_neg;
    neighbors.pos_y = neighbor_y_pos;
    neighbors.neg_z = neighbor_z_neg;
    neighbors.pos_z = neighbor_z_pos;
    neighbors.neg_x_neg_z = neg_x_neg_z;
    neighbors.neg_x_pos_z = neg_x_pos_z;
    neighbors.pos_x_neg_z = pos_x_neg_z;
    neighbors.pos_x_pos_z = pos_x_pos_z;
    neighbors.neg_x_neg_y = neg_x_neg_y;
    neighbors.pos_x_neg_y = pos_x_neg_y;
    neighbors.neg_x_pos_y = neg_x_pos_y;
    neighbors.pos_x_pos_y = pos_x_pos_y;
    neighbors.neg_y_neg_z = neg_y_neg_z;
    neighbors.neg_y_pos_z = neg_y_pos_z;
    neighbors.pos_y_neg_z = pos_y_neg_z;
    neighbors.pos_y_pos_z = pos_y_pos_z;
    neighbors.neg_x_neg_y_neg_z = neg_x_neg_y_neg_z;
    neighbors.pos_x_neg_y_neg_z = pos_x_neg_y_neg_z;
    neighbors.neg_x_pos_y_neg_z = neg_x_pos_y_neg_z;
    neighbors.pos_x_pos_y_neg_z = pos_x_pos_y_neg_z;
    neighbors.neg_x_neg_y_pos_z = neg_x_neg_y_pos_z;
    neighbors.pos_x_neg_y_pos_z = pos_x_neg_y_pos_z;
    neighbors.neg_x_pos_y_pos_z = neg_x_pos_y_pos_z;
    neighbors.pos_x_pos_y_pos_z = pos_x_pos_y_pos_z;
    init_accessor(chunk, neighbors);

    if (chunk.is_all_air()) {
        return;
    }

    total_chunks.fetch_add(1, std::memory_order_relaxed);

    populate_solid_cache(chunk, registry);
    emit_faces(chunk, registry);
    accumulate_greedy_stats();

    if (record_quads_) {
        compute_light_checksums(chunk);
    }
}

MeshBuilder::SubChunkBounds MeshBuilder::expand_bounds(const SubChunkBounds& b) {
    SubChunkBounds r;
    r.x_min = std::max(0, b.x_min - 1);
    r.x_max = std::min(CHUNK_WIDTH, b.x_max + 1);
    r.y_min = std::max(0, b.y_min - 1);
    r.y_max = std::min(CHUNK_HEIGHT, b.y_max + 1);
    r.z_min = std::max(0, b.z_min - 1);
    r.z_max = std::min(CHUNK_DEPTH, b.z_max + 1);
    return r;
}

void MeshBuilder::compute_light_checksums(const ChunkData& chunk) {
    light_checksums_.columns.fill(0);
    for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
        for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
            uint32_t h = 2166136261u;
            for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
                const uint16_t w = chunk.get_light_packed_word_unsafe(x, y, z);
                h ^= static_cast<uint8_t>(w);
                h *= 16777619u;
                h ^= static_cast<uint8_t>(w >> 8);
                h *= 16777619u;
            }
            light_checksums_.columns[z * CHUNK_WIDTH + x] = h;
        }
    }
}

void MeshBuilder::build_mesh_incremental(const ChunkData& chunk,
                                         const std::vector<CachedQuad>& prev_quads,
                                         const MeshLightChecksums& prev_light,
                                         const SubChunkBounds& dirty_bounds,
                                         const NeighborPtrs& neighbors) {
    ScopedTimer build_timer(perf_timer, TimerID::BuildMesh);

    clear();
    if (!registry_) { registry_ = &BlockRegistry::get_instance(); }
    const BlockRegistry& registry = *registry_;
    init_accessor(chunk, neighbors);

    if (chunk.is_all_air()) {
        return;
    }

    total_chunks.fetch_add(1, std::memory_order_relaxed);

    // Light-change region: every column whose light grid changed since the
    // previous build. A face within ±1 of a changed column samples the changed
    // light, so the re-emit region must cover those columns expanded by 1.
    compute_light_checksums(chunk);
    SubChunkBounds light_bounds{0, 0, 0, 0, 0, 0};  // empty
    bool light_changed = false;
    {
        int32_t cx_min = CHUNK_WIDTH, cx_max = -1, cz_min = CHUNK_DEPTH, cz_max = -1;
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                if (light_checksums_.columns[z * CHUNK_WIDTH + x] != prev_light.columns[z * CHUNK_WIDTH + x]) {
                    if (x < cx_min) cx_min = x;
                    if (x > cx_max) cx_max = x;
                    if (z < cz_min) cz_min = z;
                    if (z > cz_max) cz_max = z;
                }
            }
        }
        if (cx_min <= cx_max && cz_min <= cz_max) {
            light_changed = true;
            light_bounds.x_min = std::max(0, cx_min - 1);
            light_bounds.x_max = std::min(CHUNK_WIDTH, cx_max + 2);
            light_bounds.y_min = 0;
            light_bounds.y_max = CHUNK_HEIGHT;
            light_bounds.z_min = std::max(0, cz_min - 1);
            light_bounds.z_max = std::min(CHUNK_DEPTH, cz_max + 2);
        }
    }

    // Re-emit region = dirty geometry (expanded by the AO/light ring) ∪
    // light-changed columns. light_bounds is only valid when a changed span was
    // actually found — its "empty" init {0,0,0,0,0,0} is a literal zero box, so
    // unioning it unconditionally would pull the region down to (0,0,0).
    const SubChunkBounds geom_bounds = expand_bounds(dirty_bounds);
    partial_mode_ = true;
    partial_bounds_ = geom_bounds;
    if (light_changed) {
        partial_bounds_.x_min = std::min(partial_bounds_.x_min, light_bounds.x_min);
        partial_bounds_.x_max = std::max(partial_bounds_.x_max, light_bounds.x_max);
        partial_bounds_.y_min = std::min(partial_bounds_.y_min, light_bounds.y_min);
        partial_bounds_.y_max = std::max(partial_bounds_.y_max, light_bounds.y_max);
        partial_bounds_.z_min = std::min(partial_bounds_.z_min, light_bounds.z_min);
        partial_bounds_.z_max = std::max(partial_bounds_.z_max, light_bounds.z_max);
    }

    // Solid_cache populate box = re-emit region + 1 ring (the passes read up to
    // one cell past their iterate range for culling/above-neighbor checks).
    solid_bounds_ = expand_bounds(partial_bounds_);

    populate_solid_cache(chunk, registry);
    emit_faces(chunk, registry);

    // Carry forward cached quads outside the re-emit region.
    for (const CachedQuad& q : prev_quads) {
        if (!should_drop_quad(q)) {
            append_quad(q);
        }
    }
    accumulate_greedy_stats();
    last_partial_bounds_ = partial_bounds_;
    partial_mode_ = false;
    partial_bounds_ = {};
    solid_bounds_ = {};

    if (record_quads_) {
        // light_checksums_ already computed above; keep for the next build.
    }
}


} // namespace VoxelEngine
