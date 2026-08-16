#ifndef FUK_MINECRAFT_MESH_MANAGER_INTERNAL_HPP
#define FUK_MINECRAFT_MESH_MANAGER_INTERNAL_HPP

// Internal header shared by the MeshManager translation units (mesh_manager.cpp,
// mesh_manager_worker.cpp, mesh_manager_upload.cpp, mesh_manager_rebuild.cpp,
// mesh_manager_far.cpp, mesh_manager_lifecycle.cpp). Not for public use.

#include "mesh/mesh_manager.hpp"

namespace VoxelEngine {

inline constexpr int32_t kGreedyDisableBlockRadius = 16;
inline constexpr int32_t kFarRegionUploadDivisor = 4;
inline constexpr int32_t kFarRegionBuildDivisor = 4;
// Coalescing window for far-region rebuilds: cache arrivals within this window
// are batched into a single rebuild instead of one rebuild per arrival.
inline constexpr double kFarRegionDebounceMs = 250.0;

// Floor division (rounds toward negative infinity).
inline int32_t floor_div(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;
    if (r != 0 && ((r > 0) != (divisor > 0))) {
        --q;
    }
    return q;
}

// Pack a built mesh into the GPU-friendly packed representation.
PackedBuiltMeshData pack_vertex_array(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
void append_packed_mesh_data(PackedBuiltMeshData& dst, const PackedBuiltMeshData& src,
                             int32_t offset_x, int32_t offset_y, int32_t offset_z);

// Worker task: builds a chunk mesh (or partial remesh) on a thread-pool worker
// and pushes the result into the completed-mesh queue. Constructed on the main
// thread by rebuild_rendering_server_mesh; executed by MeshManager::worker.
struct MeshBuildTask : Task {
    ChunkMap* chunk_map;
    ChunkScheduler* chunk_scheduler;
    std::atomic<uint64_t>* async_epoch;
    ChunkRenderData* render_data;
    int32_t chunk_x, chunk_y, chunk_z;
    uint64_t epoch;
    uint64_t mesh_job_serial;
    int32_t player_bx, player_by, player_bz;
    bool smooth_lighting;
    float detail_level = 1.0f;
    uint8_t dirty_subchunks = 0xFF;
    // Partial remeshing: only re-emit the dirty region, carry everything else.
    bool partial = false;
    bool record_quads = false;
    bool greedy_enabled = true;
    // Raw block-level dirty bbox snapshot (inclusive block coords) taken at
    // enqueue. build_mesh_incremental expands it by the AO/light ring itself.
    MeshBuilder::SubChunkBounds dirty_bounds{0, 0, 0, 0, 0, 0};
    bool have_dirty_bounds = false;
    std::vector<CachedQuad> prev_quads;
    MeshLightChecksums prev_light_checksums;

    void execute() override;

    bool high_priority;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_MESH_MANAGER_INTERNAL_HPP
