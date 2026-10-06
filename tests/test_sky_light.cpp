// Tests for the sky-light walk (src/lighting/light_propagator_sky.cpp).
//
// What is being pinned here is the shape of the light, not a number that happens
// to fall out of the code: a 3x3 roof must leave its ring at 14 and its middle at
// 13, a 4x4 must leave an outer ring at 14 with 13 inside it, an open chunk must
// walk nothing at all, and the edit path must land on exactly the same field as
// the install pass -- and take it back to full light when the roof comes off.
//
// The fixture stores a 3x3x3 chunk grid so the walks have somewhere to go: a level
// never travels more than 15 cells and a chunk is 32 wide, so a region of three
// chunks per axis is the whole reach of any single pass.
#include "doctest.h"
#include "lighting/light_propagator.hpp"
#include "lighting/block_light_region.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_data.hpp"
#include "core/chunk_types.hpp"
#include "core/block_types.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

using namespace VoxelEngine;

namespace {

// ChunkRenderData carries engine-typed members the test build never touches, so it
// is allocated raw and zeroed the way the other light tests do.
std::unique_ptr<ChunkRenderData> make_test_chunk() {
    void* buf = ::operator new(sizeof(ChunkRenderData));
    std::memset(buf, 0, sizeof(ChunkRenderData));
    auto* rd = reinterpret_cast<ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<ChunkData>(std::make_unique<ChunkData>());
    rd->is_mesh_dirty = false;
    return std::unique_ptr<ChunkRenderData>(rd);
}

struct SkyFixture {
    ChunkMap map;
    LightPropagator propagator;

    SkyFixture() {
        BlockRegistry::get_instance().initialize_default_blocks();
        for (int32_t cy = -1; cy <= 1; ++cy)
            for (int32_t cz = -1; cz <= 1; ++cz)
                for (int32_t cx = -1; cx <= 1; ++cx)
                    map.insert(map.get_chunk_key(cx, cy, cz), make_test_chunk());
        propagator.set_chunk_map(&map);
        // Every chunk runs its own column scan first, exactly as generation does:
        // a column that starts under an unscanned chunk above would otherwise seed
        // itself from that chunk's zeros instead of from open sky.
        for (int32_t cy = -1; cy <= 1; ++cy)
            for (int32_t cz = -1; cz <= 1; ++cz)
                for (int32_t cx = -1; cx <= 1; ++cx) {
                    ChunkData* c = chunk_of(cx, cy, cz);
                    if (c) c->propagate_sky_light(nullptr);
                }
    }

    ChunkData* chunk_of(int32_t cx, int32_t cy, int32_t cz) {
        auto lock = map.lock_keys_exclusive({map.get_chunk_key(cx, cy, cz)});
        return map.get_chunk_data_fast(cx, cy, cz);
    }

    uint8_t sky(int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z) {
        ChunkData* c = chunk_of(cx, cy, cz);
        return c ? c->get_sky_light_unsafe(x, y, z) : 0;
    }

    // The band the pipeline holds for a block change: the origin chunk and its 26
    // neighbours, exclusive.
    ExclusiveShardLock band(int32_t cx, int32_t cy, int32_t cz) {
        uint64_t keys[27];
        int idx = 0;
        for (int32_t dz = -1; dz <= 1; ++dz)
            for (int32_t dy = -1; dy <= 1; ++dy)
                for (int32_t dx = -1; dx <= 1; ++dx)
                    keys[idx++] = map.get_chunk_key(cx + dx, cy + dy, cz + dz);
        return map.lock_keys_exclusive(keys);
    }

    // One cell change through the same path BlockEditor takes: write the block and,
    // when that can move the column's sky light, re-scan and re-propagate.
    void place(int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID id) {
        auto lock = band(cx, cy, cz);
        ChunkData* c = map.get_chunk_data_fast(cx, cy, cz);
        const BlockID old_id = c->get_block_unsafe(x, y, z);
        const BlockType& old_type = BlockRegistry::get_instance().get_block(old_id);
        const BlockType& new_type = BlockRegistry::get_instance().get_block(id);
        const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
        const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);
        c->set_block(x, y, z, id);
        if (old_opaque != new_opaque || old_type.light_opacity != new_type.light_opacity) {
            propagator.sky_light_recompute_column_locked(cx, cy, cz, x, z);
        }
    }

    // A roof laid the quick way (write the blocks, re-scan the columns) with the
    // install pass then asked to fill it -- the path a pasted build or a chunk
    // arriving with a structure in it takes.
    void roof_install(int32_t cx, int32_t cy, int32_t cz,
                      int32_t x0, int32_t x1, int32_t z0, int32_t z1, int32_t y) {
        {
            auto lock = band(cx, cy, cz);
            ChunkData* c = map.get_chunk_data_fast(cx, cy, cz);
            for (int32_t z = z0; z <= z1; ++z)
                for (int32_t x = x0; x <= x1; ++x)
                    c->set_block(x, y, z, BlockIDs::STONE);
        }
        for (int32_t z = z0; z <= z1; ++z)
            for (int32_t x = x0; x <= x1; ++x)
                rescan_column(cx, cy, cz, x, z);
    }

    void rescan_column(int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t z) {
        auto lock = band(cx, cy, cz);
        ChunkData* c = map.get_chunk_data_fast(cx, cy, cz);
        ChunkData* above = map.get_chunk_data_fast(cx, cy + 1, cz);
        c->propagate_sky_light_column(x, z, above, nullptr);
    }

    uint32_t scatter(int32_t cx, int32_t cy, int32_t cz) {
        return propagator.scatter_sky_light_region(cx, cy, cz);
    }

    // Lowest sky level in the centre chunk, for the "nothing is shaded" checks.
    uint8_t min_sky_in_chunk(int32_t cx, int32_t cy, int32_t cz) {
        ChunkData* c = chunk_of(cx, cy, cz);
        uint8_t m = 15;
        for (int32_t y = 0; y < CHUNK_HEIGHT; ++y)
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z)
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x)
                    m = std::min(m, c->get_sky_light_unsafe(x, y, z));
        return m;
    }
};

} // namespace

TEST_CASE("sky: a 3x3 roof leaves a one-step dimple, not a hole") {
    SkyFixture f;
    const int32_t y = 20;
    f.roof_install(0, 0, 0, 10, 12, 10, 12, y);
    CHECK(f.chunk_of(0, 0, 0)->has_sky_shade());

    const uint32_t modified = f.scatter(0, 0, 0);
    CHECK(modified != 0);

    // The ring under the roof's own edge: open air one step away, so 15 - 1.
    CHECK(f.sky(0, 0, 0, 10, y - 1, 10) == 14);  // corner
    CHECK(f.sky(0, 0, 0, 11, y - 1, 10) == 14);  // edge
    CHECK(f.sky(0, 0, 0, 10, y - 1, 11) == 14);
    CHECK(f.sky(0, 0, 0, 12, y - 1, 12) == 14);
    // The middle: every neighbour is under the roof and holds 14.
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 13);
    // Open air beside the roof is untouched.
    CHECK(f.sky(0, 0, 0, 9, y - 1, 11) == 15);
    CHECK(f.sky(0, 0, 0, 13, y - 1, 11) == 15);
    // The dimple is a column, not a single layer: the side light reaches every
    // depth at the same distance.
    CHECK(f.sky(0, 0, 0, 11, 5, 11) == 13);
    CHECK(f.sky(0, 0, 0, 10, 5, 11) == 14);
    CHECK(f.sky(0, 0, 0, 11, 5, 10) == 14);
    // The roof block itself holds no sky light at all.
    CHECK(f.sky(0, 0, 0, 11, y, 11) == 0);

    // A second pass finds nothing to improve: the walk is a relaxation, so a
    // re-run is a no-op rather than an oscillation.
    CHECK(f.scatter(0, 0, 0) == 0);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 13);
}

TEST_CASE("sky: a 4x4 roof has an outer ring and a darker inside") {
    SkyFixture f;
    const int32_t y = 20;
    f.roof_install(0, 0, 0, 10, 13, 10, 13, y);
    f.scatter(0, 0, 0);

    // Outer ring: one step from open air.
    CHECK(f.sky(0, 0, 0, 10, y - 1, 10) == 14);
    CHECK(f.sky(0, 0, 0, 12, y - 1, 13) == 14);
    CHECK(f.sky(0, 0, 0, 13, y - 1, 11) == 14);
    // Inside: two steps from open air on both axes.
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 13);
    CHECK(f.sky(0, 0, 0, 12, y - 1, 12) == 13);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 12) == 13);
    CHECK(f.sky(0, 0, 0, 12, y - 1, 11) == 13);
}

TEST_CASE("sky: an open chunk walks nothing") {
    SkyFixture f;
    CHECK_FALSE(f.chunk_of(0, 0, 0)->has_sky_shade());
    CHECK(f.min_sky_in_chunk(0, 0, 0) == 15);
    // The install path's gate asks the whole 3x3x3 for shade, so the band it is
    // allowed to skip is one with no shade anywhere in it. Pin that input here:
    // if any chunk in the band reported shade, skipping it would drop real work
    // (the shaded-neighbour case below is what that work looks like).
    int shaded_in_band = 0;
    for (int32_t dz = -1; dz <= 1; ++dz)
        for (int32_t dy = -1; dy <= 1; ++dy)
            for (int32_t dx = -1; dx <= 1; ++dx)
                if (f.chunk_of(dx, dy, dz)->has_sky_shade()) ++shaded_in_band;
    CHECK(shaded_in_band == 0);
    // No writes, no mask: this is the case that has to stay free while streaming.
    CHECK(f.scatter(0, 0, 0) == 0);
    CHECK(f.min_sky_in_chunk(0, 0, 0) == 15);
}

TEST_CASE("sky: the edit path casts the same dimple as the install pass") {
    SkyFixture f;
    const int32_t y = 20;
    for (int32_t z = 10; z <= 12; ++z)
        for (int32_t x = 10; x <= 12; ++x)
            f.place(0, 0, 0, x, y, z, BlockIDs::STONE);

    CHECK(f.sky(0, 0, 0, 10, y - 1, 11) == 14);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 10) == 14);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 13);
    CHECK(f.sky(0, 0, 0, 11, 5, 11) == 13);
    CHECK(f.sky(0, 0, 0, 9, y - 1, 11) == 15);
    CHECK(f.sky(0, 0, 0, 11, y, 11) == 0);
}

TEST_CASE("sky: growing a roof darkens the ring it swallowed") {
    SkyFixture f;
    const int32_t y = 20;
    for (int32_t z = 10; z <= 12; ++z)
        for (int32_t x = 10; x <= 12; ++x)
            f.place(0, 0, 0, x, y, z, BlockIDs::STONE);

    // The 3x3's far edge was the outer ring at 14. A fourth row and column of roof
    // turn those cells into the interior of a 4x4, and only a removal that follows
    // the old source can take them down to 13.
    for (int32_t z = 10; z <= 13; ++z) {
        f.place(0, 0, 0, 13, y, z, BlockIDs::STONE);
    }
    for (int32_t x = 10; x <= 12; ++x) {
        f.place(0, 0, 0, x, y, 13, BlockIDs::STONE);
    }

    // Now a 4x4: outer ring 14, inner 2x2 13.
    CHECK(f.sky(0, 0, 0, 10, y - 1, 10) == 14);
    CHECK(f.sky(0, 0, 0, 13, y - 1, 13) == 14);
    CHECK(f.sky(0, 0, 0, 12, y - 1, 11) == 13);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 12) == 13);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 13);
    CHECK(f.sky(0, 0, 0, 12, y - 1, 12) == 13);
    // The newly covered column is one step from open air on its own side.
    CHECK(f.sky(0, 0, 0, 13, y - 1, 11) == 14);
    // Nothing outside got brighter or darker.
    CHECK(f.sky(0, 0, 0, 9, y - 1, 11) == 15);
    CHECK(f.sky(0, 0, 0, 14, y - 1, 11) == 15);
}

TEST_CASE("sky: taking the roof off restores full light exactly") {
    SkyFixture f;
    const int32_t y = 20;
    for (int32_t z = 10; z <= 12; ++z)
        for (int32_t x = 10; x <= 12; ++x)
            f.place(0, 0, 0, x, y, z, BlockIDs::STONE);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 13);

    for (int32_t z = 10; z <= 12; ++z)
        for (int32_t x = 10; x <= 12; ++x)
            f.place(0, 0, 0, x, y, z, BlockIDs::AIR);

    // Not just *a* value: the whole chunk has to be back at open sky, which is the
    // idempotence the relaxation promises.
    CHECK(f.min_sky_in_chunk(0, 0, 0) == 15);
    CHECK(f.sky(0, 0, 0, 11, y - 1, 11) == 15);
}

TEST_CASE("sky: a roof on a chunk line keeps its gradient across the border") {
    SkyFixture f;
    const int32_t y = 20;
    // The roof straddles the +X face of the centre chunk: local x=31 there, and
    // x=0/1 of the neighbour, over z=10..12.
    {
        auto lock = f.band(0, 0, 0);
        ChunkData* c = f.map.get_chunk_data_fast(0, 0, 0);
        for (int32_t z = 10; z <= 12; ++z) c->set_block(31, y, z, BlockIDs::STONE);
    }
    {
        auto lock = f.band(1, 0, 0);
        ChunkData* c = f.map.get_chunk_data_fast(1, 0, 0);
        for (int32_t z = 10; z <= 12; ++z) {
            c->set_block(0, y, z, BlockIDs::STONE);
            c->set_block(1, y, z, BlockIDs::STONE);
        }
    }
    for (int32_t z = 10; z <= 12; ++z) f.rescan_column(0, 0, 0, 31, z);
    for (int32_t z = 10; z <= 12; ++z) {
        f.rescan_column(1, 0, 0, 0, z);
        f.rescan_column(1, 0, 0, 1, z);
    }

    // Both sides run their own install pass; the light crosses in both directions.
    f.scatter(0, 0, 0);
    f.scatter(1, 0, 0);

    CHECK(f.sky(0, 0, 0, 30, y - 1, 11) == 15);  // open, this chunk
    CHECK(f.sky(0, 0, 0, 31, y - 1, 11) == 14);  // outer ring, this chunk
    CHECK(f.sky(1, 0, 0, 0, y - 1, 11) == 13);   // middle of the roof, next chunk
    CHECK(f.sky(1, 0, 0, 1, y - 1, 11) == 14);   // outer ring, next chunk
    CHECK(f.sky(1, 0, 0, 2, y - 1, 11) == 15);   // open, next chunk
}

TEST_CASE("sky: a shaded neighbour is reason enough for the band to run") {
    // The install gate asks the whole band rather than the arriving chunk, because
    // an open chunk's own 15s are a LEGITIMATE source for a neighbour's dim border
    // cells: a band whose only shade is next door still has real work to do.
    SkyFixture f;
    const int32_t y = 20;
    // Roof over the +X neighbour's near columns, so what it shades sits against the
    // border with the fully open centre chunk.
    f.roof_install(1, 0, 0, 0, 2, 10, 12, y);
    CHECK(f.chunk_of(1, 0, 0)->has_sky_shade());
    CHECK_FALSE(f.chunk_of(0, 0, 0)->has_sky_shade());

    // The neighbour's own scan left its covered columns at 0, and nothing inside it
    // can reach them: the roof spans the whole 3-wide pocket and its own open
    // columns are three cells away.
    CHECK(f.sky(1, 0, 0, 0, y - 1, 11) == 0);

    // Only the centre's pass can help: its cell at the border is open sky, and the
    // neighbour's cell one step across takes 14 from it. A gate that looked at the
    // arriving chunk alone would skip this and leave the border a step too dark.
    const uint32_t modified = f.scatter(0, 0, 0);
    CHECK(modified != 0);
    CHECK((modified & BlockLightRegion::slot_bit(1, 0, 0)) != 0);
    CHECK(f.sky(1, 0, 0, 0, y - 1, 11) == 14);
    // The walk carries on from there into the pocket.
    CHECK(f.sky(1, 0, 0, 1, y - 1, 11) == 13);
    // The centre is untouched: it was already at full sky.
    CHECK(f.min_sky_in_chunk(0, 0, 0) == 15);
}

TEST_CASE("sky: the walk never touches the block-light channels") {
    SkyFixture f;
    const int32_t y = 20;
    f.roof_install(0, 0, 0, 10, 12, 10, 12, y);
    {
        auto lock = f.band(0, 0, 0);
        ChunkData* c = f.map.get_chunk_data_fast(0, 0, 0);
        c->set_light_rgb(11, y - 1, 11, 7, 5, 3);
    }
    f.scatter(0, 0, 0);
    ChunkData* c = f.chunk_of(0, 0, 0);
    CHECK(c->get_sky_light_unsafe(11, y - 1, 11) == 13);
    CHECK(c->get_light_r_unsafe(11, y - 1, 11) == 7);
    CHECK(c->get_light_g_unsafe(11, y - 1, 11) == 5);
    CHECK(c->get_light_b_unsafe(11, y - 1, 11) == 3);
}

TEST_CASE("sky: a batch of columns in one pass matches the per-column path") {
    // The same roof built two ways: nine single-column recomputes (what a block edit
    // does) and one batch (what a paste does). They must leave identical light, and
    // the batch must report the chunks it wrote so a caller can dirty their meshes.
    SkyFixture per_column;
    SkyFixture batched;
    const int32_t y = 20;
    for (int32_t z = 10; z <= 12; ++z)
        for (int32_t x = 10; x <= 12; ++x)
            per_column.place(0, 0, 0, x, y, z, BlockIDs::STONE);

    {
        auto lock = batched.band(0, 0, 0);
        ChunkData* c = batched.map.get_chunk_data_fast(0, 0, 0);
        for (int32_t z = 10; z <= 12; ++z)
            for (int32_t x = 10; x <= 12; ++x)
                c->set_block(x, y, z, BlockIDs::STONE);
    }

    std::vector<uint32_t> columns;
    for (int32_t z = 10; z <= 12; ++z)
        for (int32_t x = 10; x <= 12; ++x)
            columns.push_back((static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(z));

    uint32_t mask = 0;
    {
        auto lock = batched.band(0, 0, 0);
        mask = batched.propagator.sky_light_recompute_columns_locked(0, 0, 0, columns);
    }
    CHECK((mask & BlockLightRegion::slot_bit(0, 0, 0)) != 0);

    ChunkData* a = per_column.chunk_of(0, 0, 0);
    ChunkData* b = batched.chunk_of(0, 0, 0);
    int mismatches = 0;
    for (int32_t z = 0; z < CHUNK_DEPTH; ++z)
        for (int32_t yy = 0; yy < CHUNK_HEIGHT; ++yy)
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x)
                if (a->get_sky_light_unsafe(x, yy, z) != b->get_sky_light_unsafe(x, yy, z)) ++mismatches;
    CHECK(mismatches == 0);
}

TEST_CASE("sky: the shade flag follows the scans, not the walk") {
    SkyFixture f;
    CHECK_FALSE(f.chunk_of(0, 0, 0)->has_sky_shade());
    f.roof_install(0, 0, 0, 10, 12, 10, 12, 20);
    CHECK(f.chunk_of(0, 0, 0)->has_sky_shade());
    // A wipe clears it; only the scan may set it again.
    f.chunk_of(0, 0, 0)->clear_sky_light();
    CHECK_FALSE(f.chunk_of(0, 0, 0)->has_sky_shade());
}
