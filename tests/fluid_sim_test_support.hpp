#ifndef FARLANDS_TESTS_FLUID_SIM_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_FLUID_SIM_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the fluid simulation tests.
//
// `SimFixture` is the whole harness: a chunk map, the state table built from the
// block registry, a `RecordingSink` that remembers every write a tick made, and
// the sim wired to all three. The recording sink is what makes batching
// observable -- a test can assert both that few calls carried many cells and that
// every record landed in the chunk the call named.
//
// These were file-local to test_fluid_sim.cpp (an anonymous namespace); the split
// moved them here as `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "chunk_map_fixture.hpp"
#include "fluids/chunk_fluid.hpp"
#include "fluids/fluid_rules.hpp"
#include "fluids/fluid_sim.hpp"
#include "fluids/fluid_state_table.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace VoxelEngine;
namespace fluids = VoxelEngine::fluids;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::fluids::FluidSim;
using VoxelEngine::fluids::FluidStateTable;
using VoxelEngine::fluids::FluidWriteRecord;

namespace fluid_sim_test {


inline [[nodiscard]] BlockID registry_block(const char* name) {
    return BlockRegistry::get_instance().get_block_id_by_name(name);
}

// A block-by-block record of what a tick wrote, so a test can check both the
// batching (few calls, many cells) and that every record really landed in the
// chunk the call named.
struct RecordingSink final : fluids::FluidWriteSink {
    struct Call {
        int32_t cx = 0, cy = 0, cz = 0;
        std::vector<fluids::FluidWriteRecord> records;
    };
    std::vector<Call> calls;

    void on_chunk_updated(int32_t cx, int32_t cy, int32_t cz,
                          const std::vector<fluids::FluidWriteRecord>& writes) override {
        calls.push_back(Call{ cx, cy, cz, writes });
    }

    [[nodiscard]] int total_records() const {
        int n = 0;
        for (const Call& call : calls) n += static_cast<int>(call.records.size());
        return n;
    }
};

// One chunk of stone floor with a water source standing on it at (sx, 1, sz).
inline void insert_pool_chunk(ChunkMap& map, int32_t cx, int32_t cy, int32_t cz, BlockID source,
                       int32_t sx, int32_t sz) {
    chunktest::insert_chunk(map, cx, cy, cz, [&](ChunkData& d) {
        for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) d.set_block(x, 0, z, BlockIDs::STONE);
        }
        d.set_block(sx, 1, sz, source);
    });
}

struct SimFixture {
    ChunkMap map;
    fluids::FluidStateTable table;
    RecordingSink sink;
    fluids::FluidSim sim;

    SimFixture() {
        BlockRegistry::get_instance().initialize_default_blocks();
        table.build_from(BlockRegistry::get_instance());
        sim.set_context(map, BlockRegistry::get_instance(), table, &sink);
    }

    [[nodiscard]] FluidCell cell_at(int32_t x, int32_t y, int32_t z) const {
        return table.state_of(static_cast<BlockID>(map.get_block_world(x, y, z)));
    }

    // A 3x3 grid of floor chunks around (0, 0, 0), which is what the source in
    // the middle needs: a window reaches five blocks, so the outer chunks have to
    // exist or the flood freezes before it has spread.
    void build_three_by_three() {
        for (int32_t cz = -1; cz <= 1; ++cz) {
            for (int32_t cx = -1; cx <= 1; ++cx) {
                insert_pool_chunk(map, cx, 0, cz, BlockIDs::AIR, 0, 0);
            }
        }
    }

    void put_source(int32_t x, int32_t z) {
        const BlockID source = table.block_for(FluidCell{ FluidKind::Water, 0, false });
        map.get_chunk_data(0, 0, 0)->set_block(x, 1, z, source);
        sim.notify_block_changed(x, 1, z);
    }

    [[nodiscard]] BlockID water_source_block() const {
        return table.block_for(FluidCell{ FluidKind::Water, 0, false });
    }

    [[nodiscard]] int count_fluid() const {
        int n = 0;
        for (int32_t y = 0; y < 4; ++y) {
            for (int32_t z = -CHUNK_DEPTH; z < 2 * CHUNK_DEPTH; ++z) {
                for (int32_t x = -CHUNK_WIDTH; x < 2 * CHUNK_WIDTH; ++x) {
                    if (cell_at(x, y, z).present()) ++n;
                }
            }
        }
        return n;
    }

    // Fluid cells inside one chunk, so a test can tell "this chunk drained"
    // apart from "this chunk is not there any more".
    [[nodiscard]] int count_fluid_in_chunk(int32_t cx, int32_t cy, int32_t cz) const {
        int n = 0;
        for (int32_t y = 0; y < 4; ++y) {
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    const int32_t wx = cx * CHUNK_WIDTH + x;
                    const int32_t wy = cy * CHUNK_HEIGHT + y;
                    const int32_t wz = cz * CHUNK_DEPTH + z;
                    if (cell_at(wx, wy, wz).present()) ++n;
                }
            }
        }
        return n;
    }
};

} // namespace fluid_sim_test

#endif // FARLANDS_TESTS_FLUID_SIM_TEST_SUPPORT_HPP
