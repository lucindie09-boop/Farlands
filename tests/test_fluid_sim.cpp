#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "fluids/chunk_fluid.hpp"
#include "fluids/fluid_rules.hpp"
#include "fluids/fluid_sim.hpp"
#include "fluids/fluid_state_table.hpp"

#include <string>
#include <vector>

using namespace VoxelEngine;
namespace fluids = VoxelEngine::fluids;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::fluids::FluidSim;
using VoxelEngine::fluids::FluidStateTable;
using VoxelEngine::fluids::FluidWriteRecord;

namespace {

[[nodiscard]] BlockID registry_block(const char* name) {
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
void insert_pool_chunk(ChunkMap& map, int32_t cx, int32_t cy, int32_t cz, BlockID source,
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

} // namespace

// -----------------------------------------------------------------------------
// The state table: states and block ids meet here and nowhere else.
// -----------------------------------------------------------------------------

TEST_CASE("fluid: the state table finds the states a registry declares") {
    BlockRegistry::get_instance().initialize_default_blocks();
    fluids::FluidStateTable table;
    const int found = table.build_from(BlockRegistry::get_instance());

    // Nine blocks declare a water state: water is the source, plus seven runoff
    // depths and the falling column. surface_water is deliberately NOT one of
    // them — generated ocean is not part of the flowing simulation (see
    // block_definitions.json), so nothing about it can tick.
    CHECK(found == 9);
    CHECK(table.any());
    CHECK_FALSE(table.is_fluid(registry_block("surface_water")));

    const BlockID source = table.block_for(FluidCell{ FluidKind::Water, 0, false });
    CHECK(source != BlockIDs::AIR);
    CHECK(table.is_fluid(source));
    CHECK(table.state_of(source).is_source());
    CHECK(table.state_of(source).kind == FluidKind::Water);

    for (uint8_t depth = 1; depth <= 7; ++depth) {
        const BlockID runoff = table.block_for(FluidCell{ FluidKind::Water, depth, false });
        CHECK(runoff != BlockIDs::AIR);
        CHECK(table.state_of(runoff).depth == depth);
        CHECK_FALSE(table.state_of(runoff).is_source());
    }

    const BlockID falling = table.block_for(FluidCell{ FluidKind::Water, 0, true });
    CHECK(falling != BlockIDs::AIR);
    CHECK(table.state_of(falling).falling);
    CHECK(falling != source);  // a source and a falling column are different blocks

    // A state this registry has no block for cannot be written, and says so.
    CHECK(table.block_for(FluidCell{ FluidKind::Lava, 0, false }) == BlockIDs::AIR);
    CHECK_FALSE(table.is_fluid(BlockIDs::STONE));
    CHECK_FALSE(table.is_fluid(BlockIDs::AIR));
    CHECK_FALSE(table.state_of(BlockIDs::STONE).present());
}

// -----------------------------------------------------------------------------
// The water has shapes: each depth sits lower than the one feeding it.
// -----------------------------------------------------------------------------

TEST_CASE("fluid: each depth is its own height, and a falling cell is full") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockRegistry& registry = BlockRegistry::get_instance();

    // The reference's liquid height: the surface of a cell at depth d sits at
    // 1 - (d + 1) / 9, so a stream steps down as it runs. Without this every
    // water state renders as the same block and a stream looks like a flat sheet
    // of tiles. The source keeps the project's established 0.12 offset.
    CHECK(registry.get_block(registry_block("water")).top_face_offset == doctest::Approx(0.12f));
    const float expected[8] = { 0.0f, 0.22f, 0.33f, 0.44f, 0.56f, 0.67f, 0.78f, 0.89f };
    float previous = 0.12f;
    for (int depth = 1; depth <= 7; ++depth) {
        const BlockType& runoff =
            registry.get_block(registry_block(("water_runoff_" + std::to_string(depth)).c_str()));
        CHECK(runoff.is_fluid_state());
        CHECK(runoff.top_face_offset == doctest::Approx(expected[depth]));
        // Strictly lower than the depth that feeds it, or the slope is lost.
        CHECK(runoff.top_face_offset > previous);
        previous = runoff.top_face_offset;
        // Still the fast mesh path: a lowered top face the greedy flush can emit.
        CHECK(runoff.greedy_mergeable);
    }

    // A falling cell is a column: full height, never a shallow block. It also has
    // no lowered top at all, so nothing offsets its face away from the block edge.
    const BlockType& falling = registry.get_block(registry_block("water_fallen"));
    CHECK(falling.is_fluid_state());
    CHECK(falling.fluid_falling);
    CHECK(falling.top_face_offset == 0.0f);
    CHECK(falling.is_full_cube());
    CHECK(falling.greedy_mergeable);
}

// -----------------------------------------------------------------------------
// The chunk adapter: a window is either complete or unusable.
// -----------------------------------------------------------------------------

TEST_CASE("fluid: a window reports when the chunk it needs is not resident") {
    BlockRegistry::get_instance().initialize_default_blocks();
    fluids::FluidStateTable table;
    table.build_from(BlockRegistry::get_instance());
    ChunkMap map;
    fluids::ChunkFluidWorld world;
    world.set_context(BlockRegistry::get_instance(), table);
    chunktest::insert_floor_chunk(map, 0, 0, 0);

    // Well inside the chunk: everything the window needs is here.
    CHECK(world.read_window(map, 8, 1, 8));
    CHECK(world.window_complete());

    // Five blocks from the edge: the window reaches into a chunk that does not
    // exist, so the cell cannot be judged at all.
    CHECK_FALSE(world.read_window(map, 2, 1, 8));
    CHECK_FALSE(world.window_complete());

    // The stone floor is not something fluid can be in.
    CHECK(world.read_window(map, 8, 1, 8));
    CHECK(world.blocked(8, 0, 8));
    CHECK_FALSE(world.blocked(8, 1, 8));
    CHECK_FALSE(world.fluid_at(8, 1, 8).present());
    // Below the world is a wall, not a hole to pour into.
    CHECK(world.blocked(8, -1, 8));

    // Generated ocean water is a wall: a liquid, but not a state the simulation
    // owns. Reading it as an empty cell would let a poured bucket write runoff
    // over the sea, and those cells — with no supply of their own — would then
    // dry up and delete themselves, leaving holes in the ocean.
    const BlockID ocean = registry_block("surface_water");
    const BlockID placed = registry_block("water");
    CHECK(ocean != BlockIDs::AIR);
    CHECK(placed != BlockIDs::AIR);
    map.get_chunk_data(0, 0, 0)->set_block(9, 1, 8, ocean);
    map.get_chunk_data(0, 0, 0)->set_block(10, 1, 8, placed);
    CHECK(world.read_window(map, 8, 1, 8));
    CHECK(world.blocked(9, 1, 8));
    CHECK_FALSE(world.fluid_at(9, 1, 8).present());
    CHECK_FALSE(world.blocked(10, 1, 8));   // dynamic water is not a wall
    CHECK(world.fluid_at(10, 1, 8).is_source());
}

// -----------------------------------------------------------------------------
// The simulation, through a real chunk map.
// -----------------------------------------------------------------------------

TEST_CASE("fluid: a source floods a floor into rings and stores the states as blocks") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);

    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.enabled());
    const int ticks_run = fixture.sim.run_to_settled();
    CHECK(ticks_run > 0);

    // The source stands...
    CHECK(fixture.cell_at(8, 1, 8).is_source());
    // ...and the depth at each distance is the distance, stored as a block.
    for (int d = 1; d <= 7; ++d) {
        CHECK(fixture.cell_at(8 + d, 1, 8).depth == d);
        CHECK(fixture.cell_at(8, 1, 8 + d).depth == d);
    }
    CHECK(fixture.cell_at(9, 1, 9).depth == 2);
    // Nothing at distance 8, and the whole flood is a diamond of radius 7.
    CHECK_FALSE(fixture.cell_at(8, 1, 16).present());
    CHECK_FALSE(fixture.cell_at(16, 1, 8).present());
    CHECK(fixture.count_fluid() == 113);

    // And the block the map holds really is the runoff state, not some other id.
    CHECK(fixture.map.get_block_world(10, 1, 8) ==
          static_cast<int>(fixture.table.block_for(FluidCell{ FluidKind::Water, 2, false })));
}

TEST_CASE("fluid: a settled flood costs nothing and stops being asked") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);

    // Everything has drained out of the queue, so there is nothing left to do
    // and the simulation is idle rather than re-checking a quiet pool forever.
    CHECK(fixture.sim.pending_count() == 0);
    CHECK(fixture.sim.stats().settled > 0);
    CHECK(fixture.sim.run_to_settled() == 0);

    const int cells_before = fixture.sim.stats().last_tick_cells;
    fixture.sim.advance(5.0);
    CHECK(fixture.sim.stats().last_tick_cells == cells_before);  // no ticks ran
}

TEST_CASE("fluid: the frame budget caps a tick without losing the flood") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    // Four cells a tick: the flood has to make progress in small helpings.
    fixture.sim.configure(FluidSim::Config{ 20.0, 4, 100, 1.0 });

    // Water waits five game ticks before its first move, so advance to the tick
    // the source is actually due on; that tick is the one the budget caps.
    fixture.sim.advance(5.0 / 20.0);
    CHECK(fixture.sim.stats().last_tick_cells <= 4);
    CHECK(fixture.sim.stats().last_tick_cells > 0);
    CHECK(fixture.sim.pending_count() > 0);  // work left over, not dropped

    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.count_fluid() == 113);  // and it still gets there in the end
}

TEST_CASE("fluid: water advances one cell per tick delay, not all at once") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    // Water's delay is 5 game ticks, so a quarter second moves one cell.
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });

    const BlockID source = fixture.water_source_block();
    fixture.map.get_chunk_data(0, 0, 0)->set_block(8, 1, 8, source);

    // 5 ticks: the cells next to the source, and nothing further.
    fixture.sim.advance(5.0 / 20.0);
    CHECK(fixture.cell_at(9, 1, 8).depth == 1);
    CHECK_FALSE(fixture.cell_at(10, 1, 8).present());

    // 10 ticks: one more ring.
    fixture.sim.advance(5.0 / 20.0);
    CHECK(fixture.cell_at(10, 1, 8).depth == 2);
    CHECK_FALSE(fixture.cell_at(11, 1, 8).present());
}

TEST_CASE("fluid: a block change wakes the fluid around it") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.cell_at(8, 1, 12).depth == 4);
    CHECK_FALSE(fixture.cell_at(8, 0, 12).present());

    // Open a hole under that water, exactly as a player breaking the block would,
    // and tell the simulation about it. Without the wake-up the pool sits there
    // and never notices.
    fixture.map.get_chunk_data(0, 0, 0)->set_block(8, 0, 12, BlockIDs::AIR);
    fixture.sim.notify_block_changed(8, 0, 12);
    CHECK(fixture.sim.run_to_settled() > 0);

    CHECK(fixture.cell_at(8, 0, 12).falling);  // the pool poured into the hole
    CHECK(fixture.cell_at(8, 1, 12).present());
    // And the rest of the pool is unharmed.
    CHECK(fixture.cell_at(8, 1, 8).is_source());
    CHECK(fixture.cell_at(11, 1, 8).depth == 3);
}

TEST_CASE("fluid: an edit far from any fluid wakes nothing") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.sim.pending_count() == 0);

    // Eleven blocks from the source is outside the flood, so nothing there can
    // change a fluid's answer and the notification must cost nothing.
    fixture.sim.notify_block_changed(8, 1, 20);
    CHECK(fixture.sim.pending_count() == 0);

    // One block further in, and it does wake the fluid: this is the boundary.
    fixture.sim.notify_block_changed(8, 1, 15);
    CHECK(fixture.sim.pending_count() > 0);
}

TEST_CASE("fluid: a missing chunk freezes the flood instead of draining it") {
    SimFixture fixture;
    fixture.build_three_by_three();
    // At the edge of chunk (0,0,0), so the flood straddles two chunks and its
    // eastern half gets its supply from the chunk this test takes away.
    fixture.put_source(30, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.count_fluid() == 113);
    CHECK(fixture.count_fluid_in_chunk(1, 0, 0) > 0);

    // Take the source's chunk away, the way a player walking away would, and
    // leave the water that was fed from it standing in the loaded one.
    fixture.map.erase(fixture.map.get_chunk_key(0, 0, 0));
    const int east_before = fixture.count_fluid_in_chunk(1, 0, 0);

    // Wake water on the edge of loaded space. Its window now reaches into a
    // chunk that is not there, so the cell cannot be judged at all: it must be
    // left EXACTLY as it was and asked again later. Reading a missing chunk as
    // empty would report "nothing beside me feeds me" — this cell's only
    // supplier — and delete a band of water along the edge of loaded space.
    fixture.sim.notify_block_changed(32, 1, 8);
    fixture.sim.run_ticks(25);

    CHECK(fixture.cell_at(32, 1, 8).depth == 2);              // the edge water survives
    CHECK(fixture.count_fluid_in_chunk(1, 0, 0) == east_before);  // nothing drained
    CHECK(fixture.sim.stats().frozen > 0);
    CHECK(fixture.sim.pending_count() > 0);  // queued to look again once it loads
}

TEST_CASE("fluid: a tick writes each chunk once, not once per cell") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);

    CHECK(fixture.sink.calls.size() > 0);
    // Batching means far fewer persist/remesh calls than cells written.
    CHECK(fixture.sink.total_records() > 100);
    CHECK(fixture.sink.calls.size() < fixture.sink.total_records() / 4);

    // And every record really is in the chunk its call named, holding the state
    // it claims — the check that the grouping cannot have mixed chunks up.
    for (const RecordingSink::Call& call : fixture.sink.calls) {
        for (const fluids::FluidWriteRecord& record : call.records) {
            const int32_t wx = call.cx * CHUNK_WIDTH + record.local_x;
            const int32_t wy = call.cy * CHUNK_HEIGHT + record.local_y;
            const int32_t wz = call.cz * CHUNK_DEPTH + record.local_z;
            CHECK(fixture.map.get_block_world(wx, wy, wz) == static_cast<int>(record.block));
            CHECK(fixture.table.is_fluid(record.block));
        }
    }
}

TEST_CASE("fluid: the same scene floods the same way twice") {
    // Ordering is (due tick, wake order, position), all of which are the
    // simulation's own, so a replay cannot wander. This is what makes a bug
    // report reproducible.
    auto run_once = [](std::string& out) {
        SimFixture fixture;
        fixture.build_three_by_three();
        fixture.put_source(8, 8);
        fixture.sim.configure(FluidSim::Config{ 20.0, 64, 100, 1.0 });
        fixture.sim.run_to_settled();
        for (int32_t z = 0; z < 16; ++z) {
            for (int32_t x = 0; x < 16; ++x) {
                const FluidCell cell = fixture.cell_at(x, 1, z);
                if (!cell.present()) {
                    out += ".";
                    continue;
                }
                out += cell.is_source() ? "S" : std::to_string(static_cast<int>(cell.depth));
                out += cell.falling ? "F" : "";
            }
        }
    };
    std::string first;
    std::string second;
    run_once(first);
    run_once(second);
    CHECK(first == second);
    CHECK(first.find('S') != std::string::npos);
}
