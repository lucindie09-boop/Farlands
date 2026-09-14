#ifndef FARLANDS_CHUNK_FLUID_HPP
#define FARLANDS_CHUNK_FLUID_HPP

// -----------------------------------------------------------------------------
// The fluid system's chunk-side half: reading fluid state out of a live
// ChunkMap, and writing it back in.
//
// Like the pathfinder's chunk_nav_source.hpp this is deliberately NOT part of
// the rules — it is the adapter that lets them run against the real world, and
// it is the only file in src/fluids that knows a block id exists. It stays free
// of Godot includes so the whole simulation can be tested against a bare
// ChunkMap, with a recording sink standing in for persistence and remeshing.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/chunk_map.hpp"
#include "fluids/fluid_rules.hpp"
#include "fluids/fluid_state_table.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace VoxelEngine {
namespace fluids {

// One block a tick put into a chunk, in that chunk's local coordinates.
struct FluidWriteRecord {
    int32_t local_x = 0;
    int32_t local_y = 0;
    int32_t local_z = 0;
    BlockID block = BlockIDs::AIR;
    BlockID previous = BlockIDs::AIR;  // what the cell held before (never a fluid-free guess: read under the lock)
};

// What the game does when a tick's writes land: persist them (the edit map is
// what survives a save) and refresh the chunk's mesh. Both are the caller's
// because both need game-side objects.
class FluidWriteSink {
public:
    virtual ~FluidWriteSink() = default;

    // Called once per chunk touched by a tick, after that chunk's blocks are in
    // place and its render data is marked dirty. `writes` is that chunk's share
    // of the tick, and it is the ONLY call that chunk gets for the tick — which
    // is the whole point of batching: a 200-write tick over 3 chunks costs 3
    // exclusive locks and 3 remesh marks, not 600.
    virtual void on_chunk_updated(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                                  const std::vector<FluidWriteRecord>& writes) = 0;
};

// Reads fluid state out of a ChunkMap and writes fluid states back into it.
//
// The read side copies a whole WINDOW around the cell being ticked under ONE
// ranged lock and then answers the rules from that copy. Two reasons, both
// load-bearing:
//
//   * A per-cell read takes a shard shared_lock per cell, and the drop-seeking
//     search reads up to ~1400 cells — lock-dominated, exactly the trap the
//     pathfinder hit.
//   * Never hold a lock across calls: the window is copied and the lock is
//     released before a single rule runs, so nothing the rules do can stall
//     chunk generation or a writer.
//
// read_window() reports whether every chunk the window touches is RESIDENT, and
// the driver must refuse to tick a cell when it is not. A missing chunk reads as
// air, so judging a cell against one would let a flood pour into space that will
// be regenerated over it — and, worse, "nothing beside me feeds me" would be
// read as "dry up", deleting the water at the edge of loaded space.
class ChunkFluidWorld final : public FluidWorld {
public:
    // One step to reach a horizontal neighbour, plus the fluid's bounded search
    // (FluidTraits::search_distance, 4). The rules never read outside this.
    static constexpr int kSearchRadius = 5;
    static constexpr int kWidth = 2 * kSearchRadius + 1;  // 11 cells across
    static constexpr int kLayers = 3;                     // y - 1 .. y + 1

    void set_context(const BlockRegistry& registry, const FluidStateTable& table) noexcept {
        registry_ = &registry;
        table_ = &table;
    }

    // Copy the window around (x, y, z). False when any chunk in range is not
    // resident, in which case the caller must not decide anything about the cell.
    [[nodiscard]] bool read_window(const ChunkMap& map, int32_t x, int32_t y, int32_t z) noexcept;

    // The rules' two questions, answered from the window. Reads outside it are
    // impossible by construction (the window is sized from the rules' own
    // bounds); they answer conservatively rather than reading uninitialised.
    [[nodiscard]] FluidCell fluid_at(int x, int y, int z) const override;
    [[nodiscard]] bool blocked(int x, int y, int z) const override;

    [[nodiscard]] bool window_complete() const noexcept { return complete_; }

    // Write one tick's worth of writes, grouped by chunk: one exclusive lock,
    // one render-dirty and one sink call per chunk for the whole batch.
    // Returns how many blocks actually changed.
    int apply_writes(const ChunkMap& map, FluidWriteSink* sink, const std::vector<CellWrite>& writes);

    // Counts writes whose opacity or emission changed, i.e. the ones that would
    // need a light refresh. Every water state is transparent and non-emissive
    // and every cell fluid flows into is non-blocking, so this should stay 0;
    // the simulation logs once if it ever is not, rather than quietly lighting
    // the world wrong.
    [[nodiscard]] int opacity_changes() const noexcept { return opacity_changes_; }

private:
    struct CellSample {
        FluidCell fluid{};
        bool blocks = false;  // cannot receive fluid and cannot be passed
    };

    [[nodiscard]] static CellSample sample(const BlockRegistry& registry, BlockID id) noexcept;

    // Flat index into window_ for a local cell (py, pz, px), widened before the
    // products so the row length is applied in size_t rather than to an int.
    [[nodiscard]] static constexpr size_t window_index(int py, int pz, int px) noexcept {
        return static_cast<size_t>(py) * kWidth * kWidth + static_cast<size_t>(pz) * kWidth +
               static_cast<size_t>(px);
    }

    const BlockRegistry* registry_ = nullptr;
    const FluidStateTable* table_ = nullptr;
    int32_t origin_x_ = 0;
    int32_t origin_y_ = 0;
    int32_t origin_z_ = 0;
    bool complete_ = false;
    std::array<CellSample, static_cast<size_t>(kWidth) * kWidth * kLayers> window_{};

    // Scratch reused every tick, so a long flood does not allocate per cell.
    struct ChunkGroup {
        uint64_t key = 0;
        int32_t cx = 0;
        int32_t cy = 0;
        int32_t cz = 0;
        std::vector<FluidWriteRecord> records;
    };
    std::vector<ChunkGroup> groups_;
    int opacity_changes_ = 0;
};

} // namespace fluids
} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_FLUID_HPP
