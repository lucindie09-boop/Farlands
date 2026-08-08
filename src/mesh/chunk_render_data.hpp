#ifndef FUK_MINECRAFT_CHUNK_RENDER_DATA_HPP
#define FUK_MINECRAFT_CHUNK_RENDER_DATA_HPP
#include "core/chunk_data.hpp"
#include "mesh/mesh_types.hpp"
#include <godot_cpp/variant/rid.hpp>
#include <memory>
#include <atomic>
#include <limits>
#include <vector>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Per-chunk render data (stored in the chunk map)
// -------------------------------------------------------------------------
struct CachedFarChunkMesh {
    PackedBuiltMeshData mesh_data;
    PackedBuiltMeshData water_mesh_data;
};

struct ChunkRenderData {
    std::unique_ptr<ChunkData> data;
    godot::RID mesh_rid;
    godot::RID instance_rid;
    bool is_mesh_dirty = true;
    std::atomic<int> pending_mesh_builds{0};
    std::atomic<int> pending_mesh_uploads{0};
    std::atomic<uint64_t> mesh_job_serial{0};
    
    // Version stamps for skipping redundant mesh builds
    uint32_t mesh_version = 1;
    uint32_t last_built_version = 0;
    uint32_t last_built_neighbor_versions[6] = {0, 0, 0, 0, 0, 0};

    // Bitmask of dirty 16³ sub-chunks (bit 0..7, (x*2 + y*2*2 + z*2*2*2) encoding).
    // 0xFF = all dirty (initial state), 0 = nothing dirty.
    uint8_t dirty_subchunks = 0xFF;

    // Block-level dirty bbox accumulated from set_block edits. Drives partial
    // remeshing so a single-block edit rebuilds only a ~3³ neighborhood instead
    // of the whole 16³ sub-chunk. [min, max] inclusive; empty when min > max.
    int32_t dirty_min_x = std::numeric_limits<int32_t>::max();
    int32_t dirty_min_y = std::numeric_limits<int32_t>::max();
    int32_t dirty_min_z = std::numeric_limits<int32_t>::max();
    int32_t dirty_max_x = std::numeric_limits<int32_t>::min();
    int32_t dirty_max_y = std::numeric_limits<int32_t>::min();
    int32_t dirty_max_z = std::numeric_limits<int32_t>::min();

    bool has_dirty_bbox() const {
        return dirty_min_x <= dirty_max_x && dirty_min_y <= dirty_max_y && dirty_min_z <= dirty_max_z;
    }

    void mark_block_dirty(int32_t lx, int32_t ly, int32_t lz) {
        if (lx < dirty_min_x) dirty_min_x = lx;
        if (ly < dirty_min_y) dirty_min_y = ly;
        if (lz < dirty_min_z) dirty_min_z = lz;
        if (lx > dirty_max_x) dirty_max_x = lx;
        if (ly > dirty_max_y) dirty_max_y = ly;
        if (lz > dirty_max_z) dirty_max_z = lz;
    }

    void reset_dirty_bbox() {
        dirty_min_x = std::numeric_limits<int32_t>::max();
        dirty_min_y = std::numeric_limits<int32_t>::max();
        dirty_min_z = std::numeric_limits<int32_t>::max();
        dirty_max_x = std::numeric_limits<int32_t>::min();
        dirty_max_y = std::numeric_limits<int32_t>::min();
        dirty_max_z = std::numeric_limits<int32_t>::min();
    }

    // Content hash for upload deduplication (0 = unset/first upload)
    uint64_t mesh_content_hash = 0;

    // Whether the shader material has been set on this mesh RID (avoids redundant RS calls)
    bool material_set = false;

    // Track the last built detail level for LOD transitions
    float last_built_detail_level = 1.0f;

    // Whether the last build used the far-mode heightmap-only emitter
    bool last_built_far_mode = false;

    // Quad cache + light checksums from the last build, used for partial
    // remeshing of block edits (carried forward for everything outside the
    // dirty region). Only populated for chunks near the player.
    std::vector<CachedQuad> cached_quads;
    MeshLightChecksums light_checksums;

    // Whether the last applied build used the per-face (non-greedy) fallback.
    bool last_built_greedy_mode = true;

    // Cached packed mesh for merged far-field region rendering.
    std::shared_ptr<CachedFarChunkMesh> far_mesh_cache;
};

// -------------------------------------------------------------------------
// Completed mesh from worker thread
// -------------------------------------------------------------------------
struct CompletedMesh {
    int32_t chunk_x = 0;
    int32_t chunk_y = 0;
    int32_t chunk_z = 0;
    uint64_t epoch = 0;
    uint64_t mesh_job_serial = 0;
    ChunkRenderData* source_chunk = nullptr;
    PackedBuiltMeshData mesh_data;
    PackedBuiltMeshData water_mesh_data;
    uint64_t mesh_content_hash = 0;
    float detail_level = 1.0f;
    // Quad cache + light checksums for partial remeshing of the next edit.
    std::vector<CachedQuad> quads;
    MeshLightChecksums light_checksums;
    // Emitter mode used by this build (greedy vs per-face fallback).
    bool greedy_mode = true;
    // Snapshot of dirty_subchunks taken at enqueue (apply clears only these bits).
    uint8_t dirty_subchunks = 0xFF;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_RENDER_DATA_HPP
