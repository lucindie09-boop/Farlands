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

BuiltMeshData MeshBuilder::build_mesh_data(
    const ChunkData& chunk_data,
    const ChunkData* neg_x,
    const ChunkData* pos_x,
    const ChunkData* neg_y,
    const ChunkData* pos_y,
    const ChunkData* neg_z,
    const ChunkData* pos_z,
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
    const ChunkData* pos_x_pos_y_pos_z
) {
    build_mesh(chunk_data, neg_x, pos_x, neg_y, pos_y, neg_z, pos_z,
               neg_x_neg_z, neg_x_pos_z, pos_x_neg_z, pos_x_pos_z,
               neg_x_neg_y, pos_x_neg_y, neg_x_pos_y, pos_x_pos_y,
               neg_y_neg_z, neg_y_pos_z, pos_y_neg_z, pos_y_pos_z,
               neg_x_neg_y_neg_z, pos_x_neg_y_neg_z,
               neg_x_pos_y_neg_z, pos_x_pos_y_neg_z,
               neg_x_neg_y_pos_z, pos_x_neg_y_pos_z,
               neg_x_pos_y_pos_z, pos_x_pos_y_pos_z);
    BuiltMeshData result;
    result.vertices = std::move(vertices);
    result.indices = std::move(indices);
    result.water_vertices = std::move(water_vertices);
    result.water_indices = std::move(water_indices);
    result.empty = (result.vertices.empty() || result.indices.empty()) &&
                   (result.water_vertices.empty() || result.water_indices.empty());
    return result;
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

void MeshBuilder::populate_solid_cache(const ChunkData& chunk, const BlockRegistry& registry) {
    if (partial_mode_) {
        // Tight-populate only the box around the re-emit region. Everything
        // outside solid_bounds_ is read live via solid_at(), so it must never be
        // populated (and stale values there are never read).
        if (debug_poison_solid_cache_) {
            for (auto& plane : solid_cache)
                for (auto& row : plane)
                    row.fill(0xFFFFu);
        }
        for (int32_t y = solid_bounds_.y_min; y < solid_bounds_.y_max; ++y) {
            for (int32_t z = solid_bounds_.z_min; z < solid_bounds_.z_max; ++z) {
                const int32_t zi = z + 1;
                for (int32_t x = solid_bounds_.x_min; x < solid_bounds_.x_max; ++x) {
                    solid_cache[y][zi][x + 1] = chunk.get_block_unsafe(x, y, z);
                }
            }
        }
        return;
    }
    for (auto& plane : solid_cache)
        for (auto& row : plane)
            row.fill(0);

    // solid_cache is laid out [y][z][x] (see header) so this population pass
    // walks it with x as the fastest-varying index.
    {
        ScopedTimer cache_timer(perf_timer, TimerID::SolidCachePopulation);
        // Populate interior (x: 1..SC_W-2 = CHUNK_WIDTH, z: 1..SC_D-2 = CHUNK_DEPTH)
        for (int32_t s = 0; s < CHUNK_SECTIONS; s++) {
            if (chunk.is_section_all_air(s)) continue;
            int32_t y0 = s * SECTION_HEIGHT;
            int32_t y1 = y0 + SECTION_HEIGHT;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t z = 1; z <= CHUNK_DEPTH; z++) {
                    int32_t z_src = ((z - 1) / stride_xz_) * stride_xz_;
                    for (int32_t x = 1; x <= CHUNK_WIDTH; x++) {
                        int32_t x_src = ((x - 1) / stride_xz_) * stride_xz_;
                        BlockID representative = BlockIDs::AIR;
                        for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                            for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                                BlockID sample = chunk.get_block_unsafe(x_src + dx, y, z_src + dz);
                                if (sample != BlockIDs::AIR) {
                                    if (representative == BlockIDs::AIR) representative = sample;
                                }
                            }
                        }
                        // LOD meshing must be conservative: if any voxel in the coarse footprint
                        // is present, keep the cell alive so exposed faces are not dropped.
                        solid_cache[y][z][x] = representative;
                    }
                }
            }
        }

        // X boundaries — store the actual BlockID (or BlockIDs::AIR if neighbor null)
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            for (int32_t z = 1; z <= CHUNK_DEPTH; z++) {
                int32_t z_src = ((z - 1) / stride_xz_) * stride_xz_;
                solid_cache[y][z][0] = accessor.neg_x
                    ? accessor.neg_x->get_block_unsafe(CHUNK_WIDTH - 1, y, z_src)
                    : BlockIDs::AIR;
                solid_cache[y][z][SC_W - 1] = accessor.pos_x
                    ? accessor.pos_x->get_block_unsafe(0, y, z_src)
                    : BlockIDs::AIR;
            }
        }

        // Z boundaries
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            for (int32_t x = 1; x <= CHUNK_WIDTH; x++) {
                int32_t x_src = ((x - 1) / stride_xz_) * stride_xz_;
                solid_cache[y][0][x] = accessor.neg_z
                    ? accessor.neg_z->get_block_unsafe(x_src, y, CHUNK_DEPTH - 1)
                    : BlockIDs::AIR;
                solid_cache[y][SC_D - 1][x] = accessor.pos_z
                    ? accessor.pos_z->get_block_unsafe(x_src, y, 0)
                    : BlockIDs::AIR;
            }
        }

        // Four corner columns (x=0 or SC_W-1, z=0 or SC_D-1)
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            solid_cache[y][0][0] = accessor.neg_x_neg_z
                ? accessor.neg_x_neg_z->get_block_unsafe(CHUNK_WIDTH - 1, y, CHUNK_DEPTH - 1)
                : BlockIDs::AIR;
            solid_cache[y][0][SC_W - 1] = accessor.pos_x_neg_z
                ? accessor.pos_x_neg_z->get_block_unsafe(0, y, CHUNK_DEPTH - 1)
                : BlockIDs::AIR;
            solid_cache[y][SC_D - 1][0] = accessor.neg_x_pos_z
                ? accessor.neg_x_pos_z->get_block_unsafe(CHUNK_WIDTH - 1, y, 0)
                : BlockIDs::AIR;
            solid_cache[y][SC_D - 1][SC_W - 1] = accessor.pos_x_pos_z
                ? accessor.pos_x_pos_z->get_block_unsafe(0, y, 0)
                : BlockIDs::AIR;
        }
    }
}

BlockID MeshBuilder::solid_at(int32_t y, int32_t zi, int32_t xi) const {
    if (partial_mode_) {
        const int32_t x = xi - 1;
        const int32_t z = zi - 1;
        if (x < solid_bounds_.x_min || x >= solid_bounds_.x_max ||
            y < solid_bounds_.y_min || y >= solid_bounds_.y_max ||
            z < solid_bounds_.z_min || z >= solid_bounds_.z_max) {
            return accessor.get_block(x, y, z);
        }
    }
    return solid_cache[y][zi][xi];
}

void MeshBuilder::emit_faces(const ChunkData& chunk, const BlockRegistry& registry) {
    if (passive_greedy_enabled) {
        {
            ScopedTimer greedy_h_timer(perf_timer, TimerID::GreedyMeshHorizontal);
            passive_greedy_mesh_horizontal(chunk, accessor, FaceDirection::Top, registry);
            // Bottom faces are never visible from a ground-level / top-down view.
            // Skipping them saves ~1/6 of mesh build time and reduces GPU upload bytes.
            // passive_greedy_mesh_horizontal(chunk, accessor, FaceDirection::Bottom, registry);
        }
        {
            ScopedTimer greedy_v_timer(perf_timer, TimerID::GreedyMeshVertical);
            passive_greedy_mesh_vertical(chunk, accessor, registry);
        }
    } else {
        for (int32_t s = 0; s < CHUNK_SECTIONS; s++) {
            if (chunk.is_section_all_air(s)) continue;
            int32_t y0 = s * SECTION_HEIGHT;
            int32_t y1 = y0 + SECTION_HEIGHT;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t z = 0; z < CHUNK_DEPTH; z += stride_xz_) {
                    for (int32_t x = 0; x < CHUNK_WIDTH; x += stride_xz_) {
                        if (partial_mode_ &&
                            !(x < partial_bounds_.x_max && x + stride_xz_ > partial_bounds_.x_min &&
                              y >= partial_bounds_.y_min && y < partial_bounds_.y_max &&
                              z < partial_bounds_.z_max && z + stride_xz_ > partial_bounds_.z_min)) {
                            continue;
                        }
                        const BlockID block_id = solid_at(y, z + 1, x + 1);
                        if (block_id == BlockIDs::AIR) continue;
                        if (stride_xz_ == 1) {
                            bool all_surrounded = true;
                            for (int i = 0; i < 6; i++) {
                                int32_t nx = x + kDirectionOffsets[i][0];
                                int32_t ny = y + kDirectionOffsets[i][1];
                                int32_t nz = z + kDirectionOffsets[i][2];
                                if (nx < 0 || nx >= CHUNK_WIDTH ||
                                    ny < 0 || ny >= CHUNK_HEIGHT ||
                                    nz < 0 || nz >= CHUNK_DEPTH) {
                                    all_surrounded = false;
                                    break;
                                }
                                BlockID neighbor = solid_at(ny, nz + 1, nx + 1);
                                if (!should_cull_against_neighbor(chunk, block_id, neighbor, kAllDirections[i], x, y, z, registry)) {
                                    all_surrounded = false;
                                    break;
                                }
                            }
                            if (all_surrounded) continue;
                        }
                        for (int i = 0; i < 6; i++) {
                            FaceDirection dir = kAllDirections[i];
                            if (dir == FaceDirection::Bottom) continue;
                            int32_t dir_idx = static_cast<int32_t>(dir);
                            int32_t nx = x + kDirectionOffsets[dir_idx][0] * stride_xz_;
                            int32_t ny = y + kDirectionOffsets[dir_idx][1];
                            int32_t nz = z + kDirectionOffsets[dir_idx][2] * stride_xz_;

                            BlockID neighbor = accessor.get_block(nx, ny, nz);
                            if (!should_cull_against_neighbor(chunk, block_id, neighbor, dir, x, y, z, registry)) {
                                add_face(chunk, accessor, x, y, z, dir, block_id, registry);
                            }
                        }
                    }
                }
            }
        }
    }
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

    NeighborPtrs n;
    n.neg_x = neighbor_x_neg;
    n.pos_x = neighbor_x_pos;
    n.neg_y = neighbor_y_neg;
    n.pos_y = neighbor_y_pos;
    n.neg_z = neighbor_z_neg;
    n.pos_z = neighbor_z_pos;
    n.neg_x_neg_z = neg_x_neg_z;
    n.neg_x_pos_z = neg_x_pos_z;
    n.pos_x_neg_z = pos_x_neg_z;
    n.pos_x_pos_z = pos_x_pos_z;
    n.neg_x_neg_y = neg_x_neg_y;
    n.pos_x_neg_y = pos_x_neg_y;
    n.neg_x_pos_y = neg_x_pos_y;
    n.pos_x_pos_y = pos_x_pos_y;
    n.neg_y_neg_z = neg_y_neg_z;
    n.neg_y_pos_z = neg_y_pos_z;
    n.pos_y_neg_z = pos_y_neg_z;
    n.pos_y_pos_z = pos_y_pos_z;
    n.neg_x_neg_y_neg_z = neg_x_neg_y_neg_z;
    n.pos_x_neg_y_neg_z = pos_x_neg_y_neg_z;
    n.neg_x_pos_y_neg_z = neg_x_pos_y_neg_z;
    n.pos_x_pos_y_neg_z = pos_x_pos_y_neg_z;
    n.neg_x_neg_y_pos_z = neg_x_neg_y_pos_z;
    n.pos_x_neg_y_pos_z = pos_x_neg_y_pos_z;
    n.neg_x_pos_y_pos_z = neg_x_pos_y_pos_z;
    n.pos_x_pos_y_pos_z = pos_x_pos_y_pos_z;
    init_accessor(chunk, n);

    if (chunk.is_all_air()) {
        return;
    }

    total_chunks.fetch_add(1, std::memory_order_relaxed);

    // Far-mode: heightmap-only mesh. No solid_cache, no AO, no side faces.
    if (far_mode_) {
        build_far_mesh(chunk, registry);
        return;
    }

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

void MeshBuilder::append_quad(const CachedQuad& q) {
    auto& dest_vertices = q.water ? water_vertices : vertices;
    auto& dest_indices = q.water ? water_indices : indices;
    const uint32_t base = static_cast<uint32_t>(dest_vertices.size());
    for (int i = 0; i < 4; ++i) dest_vertices.push_back(q.verts[i]);
    for (int i = 0; i < 6; ++i) dest_indices.push_back(base + q.idx[i]);
    quads.push_back(q);
}

bool MeshBuilder::should_drop_quad(const CachedQuad& q) const {
    const SubChunkBounds& b = partial_bounds_;
    if (b.x_max <= b.x_min || b.y_max <= b.y_min || b.z_max <= b.z_min) return false;
    if (q.x + q.ex <= b.x_min || q.x >= b.x_max) return false;
    if (q.y + q.ey <= b.y_min || q.y >= b.y_max) return false;
    if (q.z + q.ez <= b.z_min || q.z >= b.z_max) return false;
    return true;
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
    if (far_mode_) {
        build_far_mesh(chunk, registry);
        return;
    }

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


void MeshBuilder::build_far_mesh(const ChunkData& chunk, const BlockRegistry& registry) {
    // For each macro column (stride_xz_ x stride_xz_) emit a single top quad at
    // the topmost non-air block. Runs of equal block type at equal height merge
    // along z so flat terrain collapses to a handful of quads. No AO (flat
    // light), no side/underside faces, no caves — this is a silhouette mesh for
    // the far distance, merged into regions by the MeshManager.
    for (int32_t x = 0; x < CHUNK_WIDTH; x += stride_xz_) {
        int32_t merge_start = -1;
        int32_t merge_y = -1;
        BlockID merge_block = BlockIDs::AIR;
        uint16_t merge_light = 0;

        auto flush_run = [&](int32_t z) {
            if (merge_start < 0) {
                return;
            }
            Face face;
            face.x = x;
            face.y = merge_y;
            face.z = merge_start;
            face.direction = FaceDirection::Top;
            face.block_id = merge_block;
            face.u_max = stride_xz_ - 1;
            face.v_max = (z - stride_xz_) - merge_start;
            
            // Far LOD: uniform AO for consistency with near/mid tiers
            // Face direction shade is applied by shader, so we just need uniform brightness
            const float ao[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            add_greedy_face(chunk, accessor, face, merge_light, 0, ao, registry);
            merge_start = -1;
        };

        for (int32_t z = 0; z < CHUNK_DEPTH; z += stride_xz_) {
            int32_t top_y = -1;
            BlockID top_block = BlockIDs::AIR;
            for (int32_t y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                const BlockID b = chunk.get_block_unsafe(x, y, z);
                if (b != BlockIDs::AIR) {
                    top_y = y;
                    top_block = b;
                    break;
                }
            }
            if (top_y < 0) {
                flush_run(z);
                continue;
            }
            const int32_t light_y = std::min(top_y + 1, CHUNK_HEIGHT - 1);
            const uint16_t light_key = chunk.get_light_packed_word_unsafe(x, light_y, z);

            if (merge_start != -1 && top_block == merge_block && top_y == merge_y) {
                continue;
            }
            flush_run(z);
            merge_start = z;
            merge_y = top_y;
            merge_block = top_block;
            merge_light = light_key;
        }
        flush_run(CHUNK_DEPTH);
    }
}

bool MeshBuilder::should_cull_against_neighbor(const ChunkData& chunk, BlockID current, BlockID neighbor,
                                                FaceDirection direction, int32_t x, int32_t y, int32_t z,
                                                const BlockRegistry& registry) const {
    if (neighbor == BlockIDs::AIR) {
         return false;
    }
    const BlockType& neighbor_type = registry.get_block(neighbor);
    const BlockType& current_type = registry.get_block(current);
    if (HasProperty(neighbor_type.properties, BlockProperty::Transparent)) {
        if (current != neighbor) {
            if (!HasProperty(neighbor_type.properties, BlockProperty::Liquid) ||
                !HasProperty(current_type.properties, BlockProperty::Liquid)) {
                return false;
            }
        }
    }
    if (current == neighbor && current_type.cull_against_same) return true;
    if (is_side_face(direction)) {
        float current_height = 1.0f - current_type.top_face_offset;
        float neighbor_height = 1.0f - neighbor_type.top_face_offset;
        if (neighbor_height < current_height) return false;
        if (neighbor_height > current_height) return true;
    }
    return true;
}

bool MeshBuilder::boundary_face_fully_occluded(const ChunkData& current_chunk, const ChunkData* neighbor,
                                               FaceDirection dir, int32_t x, int32_t y, int32_t z,
                                               int32_t stride, BlockID current_block,
                                               const BlockRegistry& registry) const {
    if (!neighbor) return false;
    for (int32_t i = 0; i < stride; i++) {
        int32_t nx, nz;
        switch (dir) {
            case FaceDirection::Right: nx = 0;                nz = z + i; break;
            case FaceDirection::Left:  nx = CHUNK_WIDTH - 1;  nz = z + i; break;
            case FaceDirection::Front: nx = x + i;            nz = 0;     break;
            case FaceDirection::Back:  nx = x + i;            nz = CHUNK_DEPTH - 1; break;
            default: return false;
        }
        BlockID nb = neighbor->get_block_unsafe(nx, y, nz);
        if (!should_cull_against_neighbor(current_chunk, current_block, nb, dir, x, y, z, registry)) {
            return false;
        }
    }
    return true;
}


} // namespace VoxelEngine
