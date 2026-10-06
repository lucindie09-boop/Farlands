// The far-field surface tile builder (src/lod/lod_surface.cpp).
//
// This is the geometry the seed-grid mode draws instead of chunks, so the things
// worth pinning are the ones that would show up as broken terrain at distance:
// the winding (a backwards quad is invisible under cull_back), the sampling count
// (a tile that samples every cell corner four times is four times the cost), the
// water quad (an ocean drawn without it is a hole), the deep-floor skip (the
// saving over oceans), and the failure mode (an unsampled column is a hole, never
// invented terrain).
#include "doctest.h"
#include "lod/lod_surface.hpp"
#include "lod_surface_test_support.hpp"

// The shoreline case below casts rays at the built mesh, which is stated in the
// engine's own vector type.
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using VoxelEngine::lod::build_tile_mesh;
using VoxelEngine::lod::kNoWater;
using VoxelEngine::lod::kShadeTop;
using VoxelEngine::lod::SurfaceSample;
using VoxelEngine::lod::TileMesh;

using lod_surface_test::as_sampler;
using lod_surface_test::CountingSampler;

TEST_CASE("a flat tile is the cells it says it is") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 256, 32, as_sampler(sampler), 9);

    const int32_t cells = 256 / 32;
    CHECK(cells == 8);
    CHECK(mesh.terrain_quads == cells * cells);
    CHECK(mesh.water_quads == 0);
    CHECK(mesh.vertices.size() == static_cast<size_t>(cells) * cells * 6);
    CHECK(mesh.min_y == doctest::Approx(64.0f));
    CHECK(mesh.max_y == doctest::Approx(64.0f));
    // Every node is sampled once, not once per cell that touches it.
    CHECK(sampler.calls == (cells + 1) * (cells + 1));
    // A level quad takes the top constant.
    for (const auto& v : mesh.vertices) {
        CHECK(v.shade == doctest::Approx(kShadeTop));
        CHECK(v.water == doctest::Approx(0.0f));
        CHECK(v.layer == doctest::Approx(3.0f));
        CHECK(v.y == doctest::Approx(64.0f));
    }
}

TEST_CASE("quads are wound the way the world winds a top face") {
    // A flat tile at y = 0: the first triangle's corners must go
    // (x0,z0) -> (x1,z0) -> (x1,z1), which is MeshBuilder::kFaceVertices order for
    // a +Y face. Reversed here and every far quad is culled away.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 0.0f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    CHECK(mesh.vertices.size() >= 6);
    CHECK(mesh.vertices[0].x == doctest::Approx(0.0f));
    CHECK(mesh.vertices[0].z == doctest::Approx(0.0f));
    CHECK(mesh.vertices[1].x == doctest::Approx(32.0f));
    CHECK(mesh.vertices[1].z == doctest::Approx(0.0f));
    CHECK(mesh.vertices[2].x == doctest::Approx(32.0f));
    CHECK(mesh.vertices[2].z == doctest::Approx(32.0f));
    CHECK(mesh.vertices[3].x == doctest::Approx(0.0f));
    CHECK(mesh.vertices[3].z == doctest::Approx(0.0f));
    CHECK(mesh.vertices[4].x == doctest::Approx(32.0f));
    CHECK(mesh.vertices[4].z == doctest::Approx(32.0f));
    CHECK(mesh.vertices[5].x == doctest::Approx(0.0f));
    CHECK(mesh.vertices[5].z == doctest::Approx(32.0f));
}

TEST_CASE("a corner height is shared, so slopes are continuous") {
    // One node raised: every cell that touches it uses the same height there, so
    // no crack opens along the cell edge. On a 2x2 tile the corner node appears
    // twice and the interior node six times, all at their own height.
    CountingSampler sampler;
    sampler.height = [](int32_t x, int32_t z) {
        return (x == 0 && z == 0) ? 100.0f : 50.0f;
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    int32_t at_origin = 0;
    int32_t at_interior = 0;
    for (const auto& v : mesh.vertices) {
        if (v.x == doctest::Approx(0.0f) && v.z == doctest::Approx(0.0f)) {
            CHECK(v.y == doctest::Approx(100.0f));
            ++at_origin;
        }
        if (v.x == doctest::Approx(32.0f) && v.z == doctest::Approx(32.0f)) {
            CHECK(v.y == doctest::Approx(50.0f));
            ++at_interior;
        }
    }
    CHECK(at_origin == 2);
    CHECK(at_interior == 6);
}

TEST_CASE("a finer edge takes its coarser neighbour's chord, so no crack opens") {
    // Two tiles at different spacings share an edge and agree only AT the nodes
    // they have in common: between them the fine surface follows the terrain and
    // the coarse one chords across, and the wedge between the two surfaces is the
    // crack visible along every spacing-level boundary. The finer tile's edge is
    // snapped onto that chord instead, which is what closes it.
    CountingSampler sampler;
    // A ridge along z = 32 that only the finer sampling sees.
    sampler.height = [](int32_t, int32_t z) { return (z % 64 == 32) ? 80.0f : 40.0f; };
    const std::array<int32_t, 4> neighbours{0, 64, 0, 0};  // coarser across +x only
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9, 8.0f, neighbours);

    int32_t on_snapped_edge = 0;
    int32_t on_open_edge = 0;
    for (const auto& v : mesh.vertices) {
        // The snapped edge is the chord between the two nodes the neighbour has.
        if (v.x == doctest::Approx(64.0f)) {
            CHECK(v.y == doctest::Approx(40.0f));
            ++on_snapped_edge;
        }
        // The edge with no coarser neighbour keeps the ridge.
        if (v.x == doctest::Approx(0.0f) && v.z == doctest::Approx(32.0f)) {
            CHECK(v.y == doctest::Approx(80.0f));
            ++on_open_edge;
        }
    }
    CHECK(on_snapped_edge > 0);
    CHECK(on_open_edge > 0);
    // Snapping moves heights, so the box is taken after it -- the ridge is still
    // inside the tile and the cull box has to say so.
    CHECK(mesh.max_y == doctest::Approx(80.0f));
}

TEST_CASE("water is drawn over the floor it covers") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 60.0f; };
    sampler.water = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);

    // Four cells, each with a floor quad (shallow enough to keep) and a water
    // quad at the water level.
    CHECK(mesh.terrain_quads == 4);
    CHECK(mesh.water_quads == 4);
    int32_t water_verts = 0;
    for (const auto& v : mesh.vertices) {
        if (v.water > 0.5f) {
            ++water_verts;
            CHECK(v.y == doctest::Approx(64.0f));
            CHECK(v.layer == doctest::Approx(9.0f));
        }
    }
    CHECK(water_verts == 24);
    // The water plane is in the cull box, or a lake beyond the horizon would be
    // culled with the terrain that is under it.
    CHECK(mesh.max_y == doctest::Approx(64.0f));
}

TEST_CASE("a deep floor is skipped, a shallow one is kept") {
    CountingSampler deep;
    deep.height = [](int32_t, int32_t) { return 20.0f; };
    deep.water = [](int32_t, int32_t) { return 90.0f; };
    const TileMesh deep_mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(deep), 9);
    CHECK(deep_mesh.terrain_quads == 0);
    CHECK(deep_mesh.water_quads == 4);

    CountingSampler shallow;
    shallow.height = [](int32_t, int32_t) { return 62.0f; };
    shallow.water = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh shallow_mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(shallow), 9);
    CHECK(shallow_mesh.terrain_quads == 4);
    CHECK(shallow_mesh.water_quads == 4);
}

TEST_CASE("an unsampled column is a hole, not invented terrain") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    sampler.valid_until_x = 64;  // the nodes at x = 0 and x = 32 sample
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    // A 2x2 tile: the two cells whose corners are all sampled are drawn, the two
    // that need the missing node at x = 64 are holes.
    CHECK(mesh.terrain_quads == 2);
    CHECK(mesh.vertices.size() == 12);
}

TEST_CASE("a tile that samples nothing at all is empty") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    sampler.valid_until_x = 0;
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    CHECK(mesh.terrain_quads == 0);
    CHECK(mesh.water_quads == 0);
    CHECK(mesh.vertices.empty());
    CHECK(mesh.min_y == doctest::Approx(0.0f));
    CHECK(mesh.max_y == doctest::Approx(0.0f));
}

TEST_CASE("degenerate tile specifications are refused") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    CHECK(build_tile_mesh(0, 0, 0, 32, as_sampler(sampler), 9).vertices.empty());
    CHECK(build_tile_mesh(0, 0, 64, 0, as_sampler(sampler), 9).vertices.empty());
    // A spacing that does not divide the tile would place nodes off the global
    // lattice, so neighbouring tiles would not share their edges.
    CHECK(build_tile_mesh(0, 0, 64, 48, as_sampler(sampler), 9).vertices.empty());
    CHECK(sampler.calls == 0);
}

TEST_CASE("nodes land on the global lattice, so two tiles share their edge") {
    // The reason origin/tile/spacing are constrained: the node at the shared edge
    // is the same world position in both tiles, so the faces coincide exactly.
    CountingSampler sampler;
    sampler.height = [](int32_t x, int32_t) { return static_cast<float>(x % 7); };
    const TileMesh left = build_tile_mesh(0, 0, 256, 32, as_sampler(sampler), 9);
    const TileMesh right = build_tile_mesh(256, 0, 256, 32, as_sampler(sampler), 9);
    CHECK(!left.vertices.empty());
    CHECK(!right.vertices.empty());

    float left_edge_max = -1.0e9f;
    for (const auto& v : left.vertices) {
        if (v.x == doctest::Approx(256.0f)) left_edge_max = std::max(left_edge_max, v.y);
    }
    float right_edge_min = 1.0e9f;
    for (const auto& v : right.vertices) {
        if (v.x == doctest::Approx(256.0f)) right_edge_min = std::min(right_edge_min, v.y);
    }
    CHECK(left_edge_max > -1.0e9f);
    // The same world column sampled in both tiles gives the same height.
    sampler.height = [](int32_t x, int32_t) { return static_cast<float>(x % 7); };
    const auto at_edge = sampler(256, 0);
    for (const auto& v : left.vertices) {
        if (v.x == doctest::Approx(256.0f)) CHECK(v.y == doctest::Approx(at_edge.height));
    }
    for (const auto& v : right.vertices) {
        if (v.x == doctest::Approx(256.0f)) CHECK(v.y == doctest::Approx(at_edge.height));
    }
    CHECK(right_edge_min < 1.0e9f);
}

TEST_CASE("the mesh is in world coordinates and says so in its bounds") {
    // The convention the engine layer depends on: tiled geometry is emitted where
    // it belongs in the world, and the instance transform stays identity. It was
    // mixed with the opposite convention once (local geometry plus an origin
    // transform), which drew every tile but the first at double its offset -- one
    // visible mesh out of a ring. Both halves are pinned here, and the engine takes
    // the instance's cull box from these bounds rather than from the tile index, so
    // the two cannot drift apart again.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh mesh = build_tile_mesh(1024, 2048, 256, 32, as_sampler(sampler), 9);
    CHECK(mesh.min_x == doctest::Approx(1024.0f));
    CHECK(mesh.max_x == doctest::Approx(1024.0f + 256.0f));
    CHECK(mesh.min_z == doctest::Approx(2048.0f));
    CHECK(mesh.max_z == doctest::Approx(2048.0f + 256.0f));
    for (const auto& v : mesh.vertices) {
        CHECK(v.x >= 1024.0f);
        CHECK(v.x <= 1280.0f);
        CHECK(v.z >= 2048.0f);
        CHECK(v.z <= 2304.0f);
    }
    // A negative tile is the interesting case for a mesh that must land where it
    // says: the bounds follow the origin's sign rather than an absolute value.
    const TileMesh negative = build_tile_mesh(-768, -256, 256, 32, as_sampler(sampler), 9);
    CHECK(negative.min_x == doctest::Approx(-768.0f));
    CHECK(negative.max_x == doctest::Approx(-512.0f));
}

TEST_CASE("the texture coordinates are world block coordinates") {
    // One block of world is one texture repeat, as a chunk face maps it, and a
    // quad advances the coordinate by its own width -- which is what makes the
    // sampler take a mip level instead of one stretched texel.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 0.0f; };
    const TileMesh mesh = build_tile_mesh(1024, 2048, 64, 32, as_sampler(sampler), 9);
    CHECK(!mesh.vertices.empty());
    float max_u = 0.0f;
    for (const auto& v : mesh.vertices) {
        CHECK(v.u == doctest::Approx(v.x));
        CHECK(v.v == doctest::Approx(v.z));
        max_u = std::max(max_u, v.u);
    }
    // A 32-block quad spans 32 texture repeats.
    CHECK(max_u == doctest::Approx(1024.0f + 32.0f * 2.0f));
}

namespace {

godot::Vector3 mesh_vertex(const TileMesh& mesh, size_t index) {
    const VoxelEngine::lod::LodVertex& v = mesh.vertices[index];
    return godot::Vector3(v.x, v.y, v.z);
}

// Moller-Trumbore, two-sided: the far field is drawn with cull_disabled, so a hit
// on either winding is the frame being stopped.
bool triangle_hit(const godot::Vector3& o, const godot::Vector3& d, const godot::Vector3& a,
                  const godot::Vector3& b, const godot::Vector3& c) {
    const godot::Vector3 e1 = b - a;
    const godot::Vector3 e2 = c - a;
    const godot::Vector3 p = d.cross(e2);
    const float det = e1.dot(p);
    if (std::abs(det) < 1.0e-6f) return false;
    const float inv = 1.0f / det;
    const godot::Vector3 s = o - a;
    const float u = s.dot(p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const godot::Vector3 q = s.cross(e1);
    const float v = d.dot(q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    return e2.dot(q) * inv > 0.0f;
}

// A ray travelling +x at height `y`, the ray a player looks along when a far
// shoreline opens up. It starts outside the tile and crosses all of it.
bool ray_hits(const TileMesh& mesh, float y) {
    const godot::Vector3 origin(-512.0f, y, 144.0f);
    const godot::Vector3 dir(1.0f, 0.0f, 0.0f);
    for (size_t i = 0; i + 5 < mesh.vertices.size(); i += 6) {
        for (int32_t t = 0; t < 2; ++t) {
            const size_t base = i + static_cast<size_t>(t) * 3;
            if (triangle_hit(origin, dir, mesh_vertex(mesh, base), mesh_vertex(mesh, base + 1),
                             mesh_vertex(mesh, base + 2))) {
                return true;
            }
        }
    }
    return false;
}

// A shoreline tile, 8x8 cells at 32 blocks: two columns of deep ocean, the cell
// where the water runs onto the beach, and the land behind it. The measured
// heights are 40 under the water and 70 on land, with the water level at 64.
constexpr float kFloor = 40.0f;
constexpr float kBeach = 70.0f;
constexpr float kSea = 64.0f;

SurfaceSample shore_sample(int32_t x, int32_t z) {
    (void)z;
    SurfaceSample s;
    s.valid = true;
    if (x <= 64) {
        s.height = kFloor;
        s.water = kSea;
    } else {
        s.height = kBeach;
        s.water = kNoWater;
    }
    s.layer = 3;
    return s;
}

} // namespace

TEST_CASE("a shoreline cell keeps its terrain, so the water and the land meet") {
    VoxelEngine::lod::SurfaceSampler sampler = [](int32_t x, int32_t z) {
        return shore_sample(x, z);
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 256, 32, sampler, 9);

    // Only the cells that are UNDER the water are skipped: two columns of ocean.
    // The cell the water runs onto is drawn, terrain and all, even though its
    // deepest corner is 24 blocks below the water level -- which is what the skip
    // used to test, and why the beach disappeared with the floor.
    CHECK(mesh.terrain_quads == 6 * 8);   // the shoreline column and the five land ones
    CHECK(mesh.water_quads == 3 * 8);     // the two ocean columns and the shoreline one

    // The frame, not the counts: every ray a player would look along, from just
    // above the shelf up to just below the land, has to be STOPPED by the surface
    // that rises out of the water. Before the fix each of these crossed the whole
    // tile without touching anything -- the beach was gone and the land's own edge
    // hung above the water sheet with nothing between them, which is what showed
    // as the inside of the hill through a gap. The last rays are the control: above
    // the land there is nothing to hit, so a probe that "hits" everywhere is a
    // probe that measures nothing.
    for (float y = kFloor + 2.0f; y <= kSea + 4.0f; y += 2.0f) {
        CHECK_MESSAGE(ray_hits(mesh, y), "a ray at height " << y << " crossed the shoreline");
    }
    CHECK_FALSE(ray_hits(mesh, kBeach + 4.0f));
    CHECK_FALSE(ray_hits(mesh, kBeach + 40.0f));
}
