#include "doctest.h"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/light_packing.hpp"
#include <algorithm>
#include <cmath>
#include <map>

using namespace VoxelEngine;

namespace {
// Vertices are stored in 1/64 mesh-space increments; snap positions so exact
// integer coordinates can be compared.
float snap(float v) { return std::round(v * 64.0f) / 64.0f; }
} // namespace

TEST_CASE("greedy merged side faces keep smooth gradients at shadow boundaries") {
    BlockRegistry::get_instance().initialize_default_blocks();

    // A tall solid wall (x=0..8, y=11..30) with a manually-placed sharp sky
    // light boundary on the air beside its +X face: bright (15) at y=20..30,
    // a short ramp (13, 9) at y=18..19, and dark (0) at y=11..17. This mimics
    // a cave mouth / overhang shadow line on a cliff. The vertical greedy
    // mesher merges the uniform bright and dark stretches into single side
    // faces; previously each merged face carried ONE flat light value, so the
    // two faces disagreed at their shared grid lines (105 vs 0 with no
    // gradient) — a hard border between lit and shadowed blocks. Per-corner
    // light on merged faces makes every shared grid corner agree.
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 11; y <= 30; y++)
        for (int z = 0; z < CHUNK_DEPTH; z++)
            for (int x = 0; x <= 8; x++)
                chunk.set_block(x, y, z, BlockIDs::STONE);
    chunk.compute_section_flags();
    chunk.compute_fully_solid();

    // Air cells beside the wall's +X face: bright above, dark below, ramp in between.
    for (int y = 11; y <= 30; y++)
        for (int z = 1; z < CHUNK_DEPTH - 1; z++) {
            uint8_t level;
            if (y <= 17) level = 0;
            else if (y == 18) level = 13;
            else if (y == 19) level = 9;
            else level = 15;
            chunk.set_sky_light(9, y, z, level);
        }
    CHECK(chunk.get_sky_light(9, 12, 16) == 0);
    CHECK(chunk.get_sky_light(9, 25, 16) == 15);

    MeshBuilder mb;
    mb.set_smooth_lighting(true);  // this alone also disables greedy
    mb.set_greedy_enabled(true);   // production path has both enabled
    mb.build_mesh(chunk);

    // Group every vertex of the wall's +X face by its grid corner (y, z). Any
    // two faces sharing a corner must produce the SAME light.
    std::map<std::pair<int, int>, std::pair<float, float>> per_corner;
    for (const auto& v : mb.get_vertices()) {
        // Convert fixed-point positions back to float for comparison
        const float px = snap(static_cast<float>(v.x));
        const float pz = snap(static_cast<float>(v.z));
        if (px != 9.0f || v.nx <= 100) continue;
        if (pz < 1.0f || pz >= 31.0f) continue;  // chunk-border nulls
        const int gy = static_cast<int>(std::lround(snap(static_cast<float>(v.y) / 256.0f)));  // Q8.8 to float
        const int gz = static_cast<int>(std::lround(pz));
        const float sky = static_cast<float>(v.sky_light);
        auto it = per_corner.find({gy, gz});
        if (it == per_corner.end()) {
            per_corner[{gy, gz}] = {sky, sky};
        } else {
            it->second.first = std::min(it->second.first, sky);
            it->second.second = std::max(it->second.second, sky);
        }
    }
    CHECK_FALSE(per_corner.empty());

    float max_spread = 0.0f;
    for (const auto& kv : per_corner) {
        max_spread = std::max(max_spread, kv.second.second - kv.second.first);
    }
    // Shared grid corners must agree exactly. Flat merged faces put e.g. 105
    // (bright merged run) next to a smooth per-block face's ~28 at the same
    // corner — a visible hard border.
    CHECK(max_spread == 0.0f);

    // The shadow boundary must be bridged by a gradient: dark at the bottom
    // of the wall, brighter at the top, with intermediate corner values at the
    // transition (merged faces only store corner vertices, so the brightest
    // interior reads via interpolation, not as a vertex value).
    float bright = -1e9f, dark = 1e9f;
    bool seen_intermediate = false;
    for (const auto& kv : per_corner) {
        const float v = kv.second.first;
        bright = std::max(bright, v);
        dark = std::min(dark, v);
        if (v > 10.0f && v < 60.0f) seen_intermediate = true;
    }
    CHECK(dark <= 10.0f);
    CHECK(bright >= 60.0f);
    CHECK(seen_intermediate);
    CHECK(bright > dark + 40.0f);
}
