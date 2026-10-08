// The far field's biome boundaries (src/lod/lod_surface.cpp, layer_mix / layer_pair).
//
// The near world mixes its biomes in worldgen, so a boundary there is a gradient of
// surface materials before any geometry exists. A far cell is one flat quad wearing
// one texture array layer, so the same boundary arrived as a straight line as long as
// the cell is wide -- sand against grass with a razor edge on it, which is what the
// blend exists to remove. The claims worth pinning are the ones that would show as a
// seam: the mix is a SHARE of a five-node neighbourhood, it is read from the node
// rather than from the cell (so two cells sharing a node agree there), and a tile that
// is one biome through and through blends nothing at all.
#include "doctest.h"
#include "lod/lod_surface.hpp"
#include "lod_surface_test_support.hpp"

#include <cstdint>

using VoxelEngine::lod::build_tile_mesh;
using VoxelEngine::lod::layer_mix;
using VoxelEngine::lod::layer_pair;
using VoxelEngine::lod::NodeSurface;
using VoxelEngine::lod::NodeSurfaceFn;
using VoxelEngine::lod::TileMesh;

using lod_surface_test::as_sampler;
using lod_surface_test::CountingSampler;

namespace {

// The engine's own node callback in miniature (lod_grid_build.cpp): the ring is read
// from the sampler and the mix is the same pure helper the worker calls, so what is
// measured here is the arithmetic the far field is built with.
NodeSurfaceFn ring_surface(CountingSampler& s, int32_t spacing) {
    return [&s, spacing](int32_t x, int32_t z) {
        NodeSurface out;
        auto layer_at = [&s](int32_t px, int32_t pz) { return s(px, pz).layer; };
        out.mix = layer_mix(layer_at(x, z), layer_at(x, z - spacing), layer_at(x, z + spacing),
                            layer_at(x + spacing, z), layer_at(x - spacing, z));
        return out;
    };
}

// A world whose surface is layer 3 west of x = 32 and layer 5 at and east of it: a
// biome boundary running through a 64-block tile, down the middle.
constexpr uint8_t kWest = 3;
constexpr uint8_t kEast = 5;

} // namespace

TEST_CASE("a uniform neighbourhood has nothing to blend with") {
    const auto mix = layer_mix(7, 7, 7, 7, 7);
    CHECK(mix.other == 7);
    CHECK(mix.share == doctest::Approx(0.0f));
}

TEST_CASE("the mix is the other biome's share of the five-node neighbourhood") {
    // One node of another layer in the ring: a fifth of the neighbourhood, because the
    // node itself counts and it is not the other layer.
    const auto one = layer_mix(kWest, kEast, kWest, kWest, kWest);
    CHECK(one.other == kEast);
    CHECK(one.share == doctest::Approx(0.2f));
    // Three of the four: a boundary the node is sitting on.
    const auto three = layer_mix(kWest, kEast, kEast, kEast, kWest);
    CHECK(three.other == kEast);
    CHECK(three.share == doctest::Approx(0.6f));
    // All four, which is the most a ring can differ.
    const auto four = layer_mix(kWest, kEast, kEast, kEast, kEast);
    CHECK(four.other == kEast);
    CHECK(four.share == doctest::Approx(0.8f));
}

TEST_CASE("the mix follows the commoner of the layers around it") {
    // Two neighbours of one layer and one of another: the named other is the pair of
    // them, not the single one, so a three-way corner still blends toward the side it
    // mostly faces.
    const auto mix = layer_mix(kWest, kEast, kEast, 9, kWest);
    CHECK(mix.other == kEast);
    CHECK(mix.share == doctest::Approx(0.4f));
}

TEST_CASE("a cell blends between the layer most of its corners wear and the one crossing it") {
    const auto uniform = layer_pair(4, 4, 4, 4, {4, 4, 4, 4});
    CHECK(uniform.base == 4);
    CHECK(uniform.other == uniform.base);
    const auto crossed = layer_pair(kWest, kEast, kWest, kWest, {kEast, kEast, kEast, kEast});
    CHECK(crossed.base == kWest);
    CHECK(crossed.other == kEast);
}

TEST_CASE("the pair is chosen from the corners, so both cells at a boundary name it") {
    // A cell of grass with one sand corner, and the sand cell beside it: the grass cell
    // names sand as its other because its corners call it, and the sand cell names
    // grass for the same reason. Neither of them has to be told where the boundary is.
    const auto grass_side = layer_pair(4, 4, 4, 9, {9, 9, 9, 9});
    CHECK(grass_side.base == 4);
    CHECK(grass_side.other == 9);
    const auto sand_side = layer_pair(9, 9, 9, 9, {4, 4, 4, 9});
    CHECK(sand_side.base == 9);
    CHECK(sand_side.other == 4);
}

TEST_CASE("the blend is a ramp across the boundary cell, not a line along its edge") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    sampler.layer = [](int32_t x, int32_t) { return x >= 32 ? kEast : kWest; };
    constexpr int32_t spacing = 32;
    const TileMesh mesh =
        build_tile_mesh(0, 0, 64, spacing, as_sampler(sampler), 9, {},
                        ring_surface(sampler, spacing));
    CHECK(mesh.terrain_quads == 4);

    // The first cell's corners in the world's own order: (0,0), (32,0), (32,32), then
    // the same pair again for the second triangle. The two corners on the boundary are
    // 80% the layer they are next to; the two on the west side are 20% the east layer.
    CHECK(mesh.vertices[0].layer2 == doctest::Approx(kEast));
    CHECK(mesh.vertices[0].mix == doctest::Approx(0.2f));
    CHECK(mesh.vertices[1].mix == doctest::Approx(0.8f));
    CHECK(mesh.vertices[2].mix == doctest::Approx(0.8f));
    CHECK(mesh.vertices[5].mix == doctest::Approx(0.2f));
    // The cell's own layer is the one most of its corners wear, and it names the east
    // layer as the one to blend toward, because that is the layer its two west corners
    // are next to.
    CHECK(mesh.vertices[0].layer == doctest::Approx(kWest));
    CHECK(mesh.vertices[0].layer2 == doctest::Approx(kEast));
    // The cell east of it is all one layer -- and it names the WEST layer, because that
    // is the one ITS corners are next to. The named layer is the node's answer rather
    // than a direction, which is what makes the two cells agree along the edge between
    // them instead of each blending on its own side of it.
    CHECK(mesh.vertices[6].layer == doctest::Approx(kEast));
    CHECK(mesh.vertices[6].layer2 == doctest::Approx(kWest));
    // Every cell's pair is constant across its six vertices: a layer index interpolated
    // between two different layers would sample a texture that is neither of them.
    for (size_t cell = 0; cell + 6 <= mesh.vertices.size(); cell += 6) {
        for (size_t v = 1; v < 6; ++v) {
            CHECK(mesh.vertices[cell + v].layer2 == mesh.vertices[cell].layer2);
        }
    }
}

TEST_CASE("a tile that is one biome through and through blends nothing at all") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    sampler.layer = [](int32_t, int32_t) { return kWest; };
    constexpr int32_t spacing = 32;
    const TileMesh mesh =
        build_tile_mesh(0, 0, 64, spacing, as_sampler(sampler), 9, {},
                        ring_surface(sampler, spacing));
    CHECK(mesh.terrain_quads == 4);
    for (const auto& v : mesh.vertices) {
        CHECK(v.layer == doctest::Approx(kWest));
        // ...and the second layer names no other texture, so the shader skips the
        // second sample rather than blending a layer with itself.
        CHECK(v.layer2 == doctest::Approx(kWest));
        CHECK(v.mix == doctest::Approx(0.0f));
    }
}

TEST_CASE("the water plane is one layer and does not blend") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 60.0f; };
    sampler.water = [](int32_t, int32_t) { return 64.0f; };
    sampler.layer = [](int32_t x, int32_t) { return x >= 32 ? kEast : kWest; };
    constexpr int32_t spacing = 32;
    const TileMesh mesh =
        build_tile_mesh(0, 0, 64, spacing, as_sampler(sampler), 9, {},
                        ring_surface(sampler, spacing));
    CHECK(mesh.water_quads == 4);
    for (const auto& v : mesh.vertices) {
        if (v.water < 0.5f) continue;
        CHECK(v.layer == doctest::Approx(9));
        CHECK(v.layer2 == doctest::Approx(9));
        CHECK(v.mix == doctest::Approx(0.0f));
    }
}
