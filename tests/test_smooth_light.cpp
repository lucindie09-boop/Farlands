#include "doctest.h"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/light_packing.hpp"
#include <algorithm>
#include <cmath>

using namespace VoxelEngine;

namespace {
// Vertices are stored in 1/64 mesh-space increments; snap positions so exact
// integer coordinates can be compared.
float snap(float v) { return std::round(v * 64.0f) / 64.0f; }
} // namespace

TEST_CASE("smooth lighting ignores occluding samples at face corners") {
    BlockRegistry::get_instance().initialize_default_blocks();

    // Ground y=0..10. Two pillars DIAGONALLY adjacent: A at (16,16), B at
    // (17,17), both y=11..20. B's solid blocks sit inside the ±1 sampling
    // window of A's Right/Front face corners, so a blind average of the 4
    // packed-light samples would pull A's z=17 corner edge down to level ~8
    // (sky=40) for the pillar's whole height while the opposite edge stayed
    // full bright (sky=105) — the reported seam.
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 0; y <= 10; y++)
        for (int z = 0; z < CHUNK_DEPTH; z++)
            for (int x = 0; x < CHUNK_WIDTH; x++)
                chunk.set_block(x, y, z, BlockIDs::STONE);
    for (int y = 11; y <= 20; y++) {
        chunk.set_block(16, y, 16, BlockIDs::STONE);
        chunk.set_block(17, y, 17, BlockIDs::STONE);
    }
    chunk.compute_section_flags();
    chunk.compute_fully_solid();
    chunk.propagate_sky_light(nullptr);

    // Precondition: opaque solids store no propagated light, air carries full
    // skylight — the stale solid value is what used to leak into the average.
    CHECK(chunk.get_sky_light(16, 15, 16) == 0);
    CHECK(chunk.get_sky_light(16, 20, 17) == 15);

    MeshBuilder mb;
    mb.set_smooth_lighting(true);
    mb.build_mesh(chunk);

    // Full skylight (level 15) scales to ~105 in vertex space. Every mid-body
    // vertex of pillar A's Right face (x=17) must carry it: the edge adjacent
    // to B must be identical to the opposite edge (no seam).
    float min_sky = 1e9f, max_sky = -1e9f;
    int count = 0;
    for (const auto& v : mb.get_vertices()) {
        const float px = snap(v.x);
        const float py = snap(v.y);
        const float pz = snap(v.z);
        if (px == 17.0f && py >= 12.0f && py <= 20.0f && pz >= 16.0f && pz <= 17.0f) {
            const float sky = static_cast<float>(v.sky_light);
            min_sky = std::min(min_sky, sky);
            max_sky = std::max(max_sky, sky);
            count++;
        }
    }
    CHECK(count > 0);
    CHECK(min_sky == doctest::Approx(105.0f).epsilon(0.01f));
    CHECK(max_sky == doctest::Approx(105.0f).epsilon(0.01f));
    CHECK(max_sky == min_sky);
}
