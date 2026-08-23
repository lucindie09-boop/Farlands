#ifndef FARLANDS_CHUNK_TYPES_HPP
#define FARLANDS_CHUNK_TYPES_HPP
#include "core/chunk_data.hpp"
#include "mesh/chunk_render_data.hpp"
#include <memory>

namespace VoxelEngine {

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
// Pending block placement for chunks that haven't loaded yet
// -------------------------------------------------------------------------
struct PendingBlockPlacement {
    int32_t world_x = 0;
    int32_t world_y = 0;
    int32_t world_z = 0;
    int block_id = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_TYPES_HPP
