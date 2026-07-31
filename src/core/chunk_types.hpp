#ifndef FUK_MINECRAFT_CHUNK_TYPES_HPP
#define FUK_MINECRAFT_CHUNK_TYPES_HPP
#include "core/chunk_data.hpp"
#include "mesh/mesh_types.hpp"
#include <godot_cpp/variant/rid.hpp>
#include <memory>
#include <atomic>

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

    // Content hash for upload deduplication (0 = unset/first upload)
    uint64_t mesh_content_hash = 0;

    // Whether the shader material has been set on this mesh RID (avoids redundant RS calls)
    bool material_set = false;

    // Track the last built detail level for LOD transitions
    float last_built_detail_level = 1.0f;

    // Whether the last build used the far-mode heightmap-only emitter
    bool last_built_far_mode = false;

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
};

// -------------------------------------------------------------------------
// Completed chunk from worker thread
// -------------------------------------------------------------------------
struct CompletedChunk {
    int32_t chunk_x = 0;
    int32_t chunk_y = 0;
    int32_t chunk_z = 0;
    uint64_t epoch = 0;
    std::unique_ptr<ChunkData> chunk_data;
    bool was_loaded_from_disk = false;
};

// -------------------------------------------------------------------------
// Completed light propagation from worker thread
// -------------------------------------------------------------------------
struct CompletedLightPropagation {
    int32_t chunk_x = 0;
    int32_t chunk_y = 0;
    int32_t chunk_z = 0;
    uint64_t epoch = 0;
};

// -------------------------------------------------------------------------
// Pending chunk stage (for staged install: chunk ??? light ??? mesh)
// -------------------------------------------------------------------------
struct PendingChunkStage {
    int32_t chunk_x = 0;
    int32_t chunk_y = 0;
    int32_t chunk_z = 0;
    uint64_t epoch = 0;
};

// -------------------------------------------------------------------------
// Dirty mesh queue entry (priority by distance)
// -------------------------------------------------------------------------
struct DirtyChunkEntry {
    uint64_t key = 0;
    int32_t dist_sq = 0;
    bool urgent = false;
    bool in_frustum = false;
    uint32_t priority_revision = 0;
    bool operator>(const DirtyChunkEntry& other) const {
        if (urgent != other.urgent) return !urgent && other.urgent;
        if (in_frustum != other.in_frustum) return !in_frustum && other.in_frustum;
        return dist_sq > other.dist_sq;
    }
};

// -------------------------------------------------------------------------
// Pending block placement for chunks that haven't loaded yet
// -------------------------------------------------------------------------
struct PendingBlockPlacement {
    int32_t world_x = 0;
    int32_t world_y = 0;
    int32_t world_z = 0;
    int block_id = 0;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_TYPES_HPP
