// What a 3x3x3 block-light region pass reports about itself.
//
// The pass used to be invisible to its caller: every region pass dirtied all 27
// chunks for a remesh, so a paste queued 27 remeshes per chunk it touched and every
// arriving chunk during streaming queued 27 more — for light that, over ordinary
// daylight terrain, had not moved at all. It now reports which chunks it wrote
// (BlockLightRegion::modified_mask), and the clear it starts with reports whether
// there was anything to clear. These tests pin both halves of that, including the
// half that keeps it honest: light that DOES exist must still be found and cleared,
// because a fast path that skipped it would leave stale light in the world.
#include "doctest.h"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "lighting/block_light_region.hpp"
#include "lighting/light_propagation.hpp"

#include <array>
#include <memory>

using namespace VoxelEngine;

namespace {

BlockID find_emissive() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    for (BlockID id = 1; id < 2048; ++id) {
        const BlockType& t = registry.get_block(id);
        if (HasProperty(t.properties, BlockProperty::Emissive) &&
            (t.light_r != 0 || t.light_g != 0 || t.light_b != 0)) {
            return id;
        }
    }
    return BlockIDs::AIR;
}

// Stone below y=16 (chunks here are 32 tall), air above, and (optionally) SKY
// light varying per cell, which is what a real chunk has and what makes the light
// sections non-uniform. Note the channel: sky light is the low nibble, block
// light the high twelve bits.
std::unique_ptr<ChunkData> make_chunk(bool sky_light) {
    auto c = std::make_unique<ChunkData>();
    for (int y = 0; y < 16; ++y) {
        for (int z = 0; z < CHUNK_DEPTH; ++z) {
            for (int x = 0; x < CHUNK_WIDTH; ++x) {
                c->set_block(x, y, z, BlockIDs::STONE);
            }
        }
    }
    if (sky_light) {
        for (int y = 0; y < CHUNK_HEIGHT; ++y) {
            for (int z = 0; z < CHUNK_DEPTH; ++z) {
                for (int x = 0; x < CHUNK_WIDTH; ++x) {
                    const uint8_t sky = static_cast<uint8_t>((x + z + y) % 16);
                    c->set_sky_light(x, y, z, sky);
                }
            }
        }
    }
    return c;
}

struct Grid {
    std::array<std::unique_ptr<ChunkData>, 27> keep;
    ChunkData* cells[3][3][3] = {};

    void fill(bool sky_light, bool torch) {
        std::size_t i = 0;
        for (int dz = 0; dz < 3; ++dz) {
            for (int dy = 0; dy < 3; ++dy) {
                for (int dx = 0; dx < 3; ++dx) {
                    keep[i] = make_chunk(sky_light);
                    if (torch) keep[i]->set_block(16, 101, 16, find_emissive());
                    cells[dx][dy][dz] = keep[i].get();
                    ++i;
                }
            }
        }
    }
};

int modified_slots(const BlockLightRegion& region) {
    int n = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((region.modified_mask() & BlockLightRegion::slot_bit(dx, dy, dz)) != 0) ++n;
            }
        }
    }
    return n;
}

int count_block_light(const ChunkData& c) {
    int n = 0;
    for (int y = 0; y < CHUNK_HEIGHT; ++y) {
        for (int z = 0; z < CHUNK_DEPTH; ++z) {
            for (int x = 0; x < CHUNK_WIDTH; ++x) {
                if (c.get_light_r(x, y, z) || c.get_light_g(x, y, z) || c.get_light_b(x, y, z)) ++n;
            }
        }
    }
    return n;
}

// Runs a pass over `grid` either way: the full clear of all 27 slots, or the narrow
// one that wipes only the centre chunk and its six faces (the rest of the region is
// then the boundary the narrow pass seeds itself from).
void run_pass(Grid& grid, bool affected) {
    BlockLightRegion region(grid.cells);
    std::vector<EmissiveSource> sources;
    if (affected) {
        region.clear_block_light_affected();
        region.collect_emissive_sources(sources, /*only_cleared=*/true);
    } else {
        region.clear_block_light();
        region.collect_emissive_sources(sources);
    }
    region.propagate_additive(sources, /*already_cleared=*/true);
}

// Every cell of block light that differs between two identically-built grids, plus
// the largest per-channel gap and the net change, so a failure says HOW wrong rather
// than just that it is.
struct LightDiff {
    long cells = 0;
    int max_levels = 0;
    long net = 0;
};

LightDiff diff_block_light(const Grid& a, const Grid& b) {
    LightDiff d;
    for (int dz = 0; dz < 3; ++dz) {
        for (int dy = 0; dy < 3; ++dy) {
            for (int dx = 0; dx < 3; ++dx) {
                const ChunkData& ca = *a.cells[dx][dy][dz];
                const ChunkData& cb = *b.cells[dx][dy][dz];
                for (int y = 0; y < CHUNK_HEIGHT; ++y) {
                    for (int z = 0; z < CHUNK_DEPTH; ++z) {
                        for (int x = 0; x < CHUNK_WIDTH; ++x) {
                            const int ar = ca.get_light_r(x, y, z), br = cb.get_light_r(x, y, z);
                            const int ag = ca.get_light_g(x, y, z), bg = cb.get_light_g(x, y, z);
                            const int ab = ca.get_light_b(x, y, z), bb = cb.get_light_b(x, y, z);
                            if (ar == br && ag == bg && ab == bb) continue;
                            ++d.cells;
                            d.max_levels = std::max({d.max_levels, std::abs(ar - br), std::abs(ag - bg),
                                                     std::abs(ab - bb)});
                            d.net += (br + bg + bb) - (ar + ag + ab);
                        }
                    }
                }
            }
        }
    }
    return d;
}

} // namespace

// The narrow clear is ~2-3x cheaper and it is only sound WITH the boundary seeding:
// on its own it came out dimmer than the full clear (up to 9 levels on a channel),
// because light from an emitter in an untouched chunk travels through cells that
// already hold their steady value and the search only continues from a cell it
// improved. This is the test that decides whether the seeding made it exact, with an
// emitter in an UNTOUCHED slot so light genuinely has to cross back in.
TEST_CASE("the affected clear lands exactly where the full clear does") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID emissive = find_emissive();
    if (emissive == BlockIDs::AIR) return;  // no emissive block registered

    Grid full;
    Grid narrow;
    full.fill(/*sky_light=*/true, /*torch=*/false);
    narrow.fill(/*sky_light=*/true, /*torch=*/false);
    // The same fixture in both: a source inside the chunk whose blocks "changed", and
    // one in an untouched corner slot, so the pass depends on light crossing back in.
    for (Grid* g : {&full, &narrow}) {
        g->cells[1][1][1]->set_block(16, 20, 16, emissive);
        g->cells[2][0][2]->set_block(10, 20, 10, emissive);
    }

    // Settle both with a full pass, so the two start from a real lit state rather than
    // from the fixture's bare sources.
    run_pass(full, /*affected=*/false);
    run_pass(narrow, /*affected=*/false);
    CHECK(diff_block_light(full, narrow).cells == 0);

    // The pass under test, from that identical state.
    run_pass(full, /*affected=*/false);
    run_pass(narrow, /*affected=*/true);

    const LightDiff d = diff_block_light(full, narrow);
    INFO("cells differing: " << d.cells << ", worst channel gap: " << d.max_levels
                             << ", net light change: " << d.net);
    CHECK(d.cells == 0);
}

TEST_CASE("a narrow pass rebuilds the wiped chunks from light crossing the boundary") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID emissive = find_emissive();
    if (emissive == BlockIDs::AIR) return;

    Grid grid;
    grid.fill(/*sky_light=*/true, /*torch=*/false);
    // In an EDGE slot the narrow clear keeps (x and y both +1, z centred), sitting on
    // the corner it shares with the wiped centre: its light crosses into the wiped
    // chunks and from there into the centre chunk's own far corner.
    grid.cells[2][2][1]->set_block(0, 0, 16, emissive);

    run_pass(grid, /*affected=*/false);
    CHECK(count_block_light(*grid.cells[1][1][1]) > 0);

    // Nothing inside the wiped seven holds a source, so this pass has to rebuild the
    // centre's light ENTIRELY from light arriving across the boundary. Without the
    // seeding it comes back dark — which is the failure the note above records.
    run_pass(grid, /*affected=*/true);
    CHECK(count_block_light(*grid.cells[1][1][1]) > 0);
    CHECK(grid.cells[1][1][1]->get_light_r(31, 31, 16) > 0);
}

TEST_CASE("clear_block_light reports only real block light") {
    BlockRegistry::get_instance().initialize_default_blocks();

    auto plain = make_chunk(/*sky_light=*/false);
    CHECK(plain->clear_block_light() == false);  // nothing there to clear

    auto lit = make_chunk(/*sky_light=*/false);
    lit->set_light_rgb(1, 1, 1, 9, 4, 2);
    CHECK(count_block_light(*lit) == 1);
    CHECK(lit->clear_block_light() == true);  // real block light: found, and said so
    CHECK(count_block_light(*lit) == 0);      // ...and actually gone
    CHECK(lit->get_sky_light(1, 1, 1) == 0);  // sky is a different channel: untouched

    // Sky light on its own is not block light, and clearing block light must leave
    // the sky values exactly as they were.
    auto sky = make_chunk(/*sky_light=*/true);
    const uint8_t sky_before = sky->get_sky_light(3, 3, 3);
    CHECK(sky_before != 0);
    CHECK(sky->clear_block_light() == false);
    CHECK(sky->get_sky_light(3, 3, 3) == sky_before);
}

TEST_CASE("a light pass over sky-lit terrain writes nothing at all") {
    BlockRegistry::get_instance().initialize_default_blocks();

    Grid grid;
    grid.fill(/*sky_light=*/true, /*torch=*/false);

    BlockLightRegion region(grid.cells);
    std::vector<EmissiveSource> sources;
    region.collect_emissive_sources(sources);
    CHECK(sources.empty());
    region.clear_block_light();
    region.propagate_additive(sources, /*already_cleared=*/true);

    // Nothing was written, so the caller queues no remeshes for it. This is the
    // difference between streaming costing 27 remeshes per arriving chunk and
    // costing none for light that never moved.
    CHECK(region.modified_mask() == 0u);
    CHECK(modified_slots(region) == 0);
}

TEST_CASE("a repeat pass over light that did not move queues no remeshes") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID emissive = find_emissive();
    if (emissive == BlockIDs::AIR) return;

    Grid grid;
    grid.fill(/*sky_light=*/true, /*torch=*/false);
    grid.cells[1][1][1]->set_block(16, 20, 16, emissive);

    auto run_pass = [&grid]() {
        BlockLightRegion region(grid.cells);
        std::vector<EmissiveSource> sources;
        region.collect_emissive_sources(sources);
        region.clear_block_light();
        region.propagate_additive(sources, /*already_cleared=*/true);
        return region.modified_mask();
    };

    // The first pass genuinely moves light, so it must report chunks.
    CHECK(run_pass() != 0u);

    // The second pass wipes exactly what the first produced and rebuilds it. On a
    // chunk arriving into an already-lit neighbourhood this is the whole pass: the
    // arrival writes one chunk and every other slot lands back where it started.
    // Reporting those neighbours is what queued a rebuild of 26 meshes that were
    // already correct, and it is why loading a large build cost more than building
    // it. Nothing moved, so nothing is reported.
    CHECK(run_pass() == 0u);
}

TEST_CASE("a light pass that does move light reports the chunks it wrote") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID emissive = find_emissive();
    if (emissive == BlockIDs::AIR) return;  // no emissive block registered

    Grid grid;
    grid.fill(/*sky_light=*/true, /*torch=*/false);
    grid.cells[1][1][1]->set_block(16, 20, 16, emissive);
    grid.cells[1][1][1]->set_light_rgb(16, 20, 16, 14, 14, 14);

    BlockLightRegion region(grid.cells);
    std::vector<EmissiveSource> sources;
    region.collect_emissive_sources(sources);
    CHECK(!sources.empty());
    region.clear_block_light();
    region.propagate_additive(sources, /*already_cleared=*/true);

    // The source chunk is reported (and the light it spread into anything it
    // reached). Under-reporting here would leave a chunk's mesh showing light that
    // is no longer in its data.
    CHECK((region.modified_mask() & BlockLightRegion::slot_bit(0, 0, 0)) != 0u);
}
