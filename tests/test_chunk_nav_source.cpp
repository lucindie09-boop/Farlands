// Tests for the in-game half of the planner: the sampler that reads the live
// chunk map.
//
// Three things here are worth guarding. First, the ranged column read batches a
// whole column into one lock, which is only correct if the chunk and local
// coordinates line up exactly across chunk boundaries — hence the cross-check
// against a raw block query, and the equivalence check against the per-cell
// sampler. Second, neither read path may hold a lock between calls: a held shard
// lock blocks generation from writing that shard, and in the worst case (the
// same thread writing) it deadlocks outright. Third, the source must never
// report ungenerated space as air: that is the difference between routing around
// a cliff and routing straight through one.

#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "core/block_types.hpp"
#include "pathfinding/chunk_nav_source.hpp"
#include "pathfinding/pathfinder.hpp"

#include <utility>
#include <vector>

using namespace VoxelEngine;
namespace nav = VoxelEngine::nav;
using chunktest::insert_chunk;
using chunktest::insert_floor_chunk;

TEST_CASE("ChunkMapNavSource reads across a chunk boundary exactly like a raw query") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    insert_floor_chunk(cm, 0, 0, 0);  // stone floor at world y=0
    insert_chunk(cm, 0, 1, 0, [](ChunkData& d) {
        for (int32_t x = 0; x < CHUNK_WIDTH; ++x)
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z)
                d.set_block(x, 0, z, BlockIDs::STONE);  // world y=32
        d.set_block(2, 5, 3, BlockIDs::DIRT);           // world y=37, one cell only
    });
    nav::ChunkMapNavSource src(cm);

    // Every cell in a window straddling the y=32 boundary must classify the same
    // as reading the block straight out of the map.
    int32_t mismatches = 0;
    for (int32_t x = 0; x < 6; ++x) {
        for (int32_t z = 0; z < 6; ++z) {
            for (int32_t y = 28; y <= 40; ++y) {
                const nav::Cell got = src.sample(x, y, z);
                const nav::Cell want =
                    nav::classify_block(static_cast<BlockID>(cm.get_block_world(x, y, z)));
                if (got.cls != want.cls || got.lo != want.lo || got.hi != want.hi) ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);

    // The lone block in the upper chunk is read at its own coordinates and
    // nowhere else: a y-offset or local-coordinate slip would show up here.
    CHECK(src.sample(2, 37, 3).blocks_body());
    CHECK_FALSE(src.sample(3, 37, 3).blocks_body());
    CHECK_FALSE(src.sample(2, 36, 3).blocks_body());
}

TEST_CASE("The ranged column read answers exactly like the per-cell sampler") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    insert_floor_chunk(cm, 0, 0, 0);
    insert_floor_chunk(cm, 0, 1, 0);
    insert_chunk(cm, 0, 2, 0, [](ChunkData& d) {
        for (int32_t x = 0; x < 8; ++x)
            for (int32_t z = 0; z < 8; ++z) {
                d.set_block(x, 0, z, BlockIDs::STONE);
                d.set_block(x, 1, z, BlockIDs::STONE);  // a two-block step at y=64
            }
    });
    nav::ChunkMapNavSource src(cm);

    // A range spanning three chunk heights, including the ungenerated column
    // beside it and the world-height edges.
    int32_t mismatches = 0;
    int32_t cells = 0;
    for (const auto& col : {std::pair<int32_t, int32_t>{3, 4}, {40, 5}, {70, 6}, {120, 7}}) {
        std::vector<nav::Cell> out(60);
        src.read_column(col.first, col.second, 50, 109, out.data());
        for (int32_t y = 50; y <= 109; ++y) {
            const nav::Cell want = src.sample(col.first, y, col.second);
            const nav::Cell got = out[static_cast<size_t>(y - 50)];
            if (got.cls != want.cls || got.lo != want.lo || got.hi != want.hi) ++mismatches;
            ++cells;
        }
    }
    CHECK(cells == 240);
    CHECK(mismatches == 0);

    // Out of the world's vertical range: unknown, not air, and indexed the way
    // the caller asked for.
    std::vector<nav::Cell> edge(3);
    src.read_column(3, 4, -1, 1, edge.data());
    CHECK(edge[0].cls == nav::CellClass::Unknown);  // y = -1
    CHECK(edge[1].blocks_body());                   // y = 0, the floor
    CHECK(edge[2].cls == nav::CellClass::Air);      // y = 1
}

TEST_CASE("ChunkMapNavSource reports ungenerated space as Unknown, never as air") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    insert_floor_chunk(cm, 0, 0, 0);
    nav::ChunkMapNavSource src(cm);

    CHECK(src.sample(4, 0, 4).cls != nav::CellClass::Unknown);

    // A raw block query answers air for a chunk that was never generated, which
    // is exactly the answer the planner must not accept.
    CHECK(cm.get_block_world(96, 0, 96) == static_cast<int>(BlockIDs::AIR));
    CHECK(src.sample(96, 0, 96).cls == nav::CellClass::Unknown);

    std::vector<nav::Cell> out(4);
    src.read_column(96, 96, 0, 3, out.data());
    for (const nav::Cell& c : out) CHECK(c.cls == nav::CellClass::Unknown);
}

TEST_CASE("ChunkMapNavSource freezes the residency it saw when the search started") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    nav::ChunkMapNavSource src(cm);

    CHECK(src.sample(40, 0, 4).cls == nav::CellClass::Unknown);

    // The chunk streams in mid-search — while the source is alive and mid-read,
    // which is also the case that deadlocks if a lock is held between calls.
    insert_floor_chunk(cm, 1, 0, 0);
    CHECK(src.sample(40, 0, 4).cls == nav::CellClass::Unknown);

    std::vector<nav::Cell> out(2);
    src.read_column(40, 4, 0, 1, out.data());
    CHECK(out[0].cls == nav::CellClass::Unknown);

    // A source created after the chunk loaded does see it — the data is there,
    // it is the in-flight search that refuses to change its mind.
    nav::ChunkMapNavSource fresh(cm);
    CHECK(fresh.sample(40, 0, 4).cls != nav::CellClass::Unknown);
}

TEST_CASE("ChunkMapNavSource locks once per column read, not once per cell") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    insert_floor_chunk(cm, 0, 0, 0);  // world y 0..31
    insert_floor_chunk(cm, 0, 1, 0);  // world y 32..63
    nav::ChunkMapNavSource src(cm);

    // One column, 41 cells, spanning both chunks: a single locked pass.
    std::vector<nav::Cell> out(41);
    src.read_column(5, 5, 20, 60, out.data());
    CHECK(src.cells_read() == 41);
    CHECK(src.lock_acquisitions() == 1);

    // The per-cell path is the honest one-lock-per-cell one, and that is what it
    // charges for — the cost the ranged read exists to avoid.
    const size_t before_cells = src.cells_read();
    const size_t before_locks = src.lock_acquisitions();
    for (int32_t y = 20; y >= 0; --y) (void)src.sample(6, y, 5);
    CHECK(src.cells_read() == before_cells + 21);
    CHECK(src.lock_acquisitions() == before_locks + 21);

    // A tall range over four chunk heights costs a single lock, so the cost
    // still scales with columns rather than cells.
    std::vector<nav::Cell> tall(97);
    src.read_column(9, 9, 1, 97, tall.data());
    CHECK(src.lock_acquisitions() == before_locks + 21 + 1);
}

TEST_CASE("A plan over the in-game chunk source never crosses ungenerated space") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    insert_floor_chunk(cm, 0, 0, 0);  // world x 0..31
    insert_floor_chunk(cm, 1, 0, 0);  // world x 32..63
    nav::ChunkMapNavSource src(cm);

    const nav::NavBox box{0, 0, 0, 95, 63, 31};
    nav::NavView view(box,
                      [&src](int32_t x, int32_t y, int32_t z) { return src.sample(x, y, z); },
                      nav::NavCosts{},
                      [&src](int32_t x, int32_t z, int32_t y_lo, int32_t y_hi, nav::Cell* out) {
                          src.read_column(x, z, y_lo, y_hi, out);
                      });
    nav::Pathfinder finder(view, nav::NavCosts{});

    nav::NavQuery q;
    q.start = nav::NavNode{2, 5, 2};   // handed in above the floor, snapped down
    q.goal = nav::NavNode{40, 5, 2};
    const nav::NavPath path = finder.search(q);

    CHECK(path.found);
    CHECK_FALSE(path.truncated);
    CHECK(path.nodes.front() == nav::NavNode{2, 1, 2});
    CHECK(path.nodes.back().x == 40);
    for (const nav::NavNode& n : path.nodes) CHECK(n.x < 64);

    // A plan is dominated by column resolution, so the ranged read has to show up
    // over a whole search: many cells read per lock taken.
    CHECK(src.lock_acquisitions() * 2 <= src.cells_read());

    // The same plan with a goal in the never-generated chunk next door: the
    // planner refuses to end on unknown ground rather than routing into it.
    nav::ChunkMapNavSource src2(cm);
    nav::NavView view2(box,
                       [&src2](int32_t x, int32_t y, int32_t z) { return src2.sample(x, y, z); },
                       nav::NavCosts{},
                       [&src2](int32_t x, int32_t z, int32_t y_lo, int32_t y_hi, nav::Cell* out) {
                           src2.read_column(x, z, y_lo, y_hi, out);
                       });
    nav::Pathfinder finder2(view2, nav::NavCosts{});

    nav::NavQuery q2;
    q2.start = nav::NavNode{2, 5, 2};
    q2.goal = nav::NavNode{80, 5, 2};
    const nav::NavPath path2 = finder2.search(q2);

    CHECK_FALSE(path2.found);
    CHECK_FALSE(path2.truncated);
    CHECK(finder2.last_error() == "goal column has no navigable surface");
}
