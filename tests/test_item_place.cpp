#include "doctest.h"
#include "core/item_registry.hpp"

using namespace VoxelEngine;

// An item may declare the block it places (items.json "place"); `resolve` is the
// only place the clicked face turns into a variant, so it is pinned here without
// needing a world, a registry load or a Godot runtime.
namespace {

// The torch is the shipped case: block = the ground torch, wall[] = the four
// variants keyed by the face the piece hugs (n = -Z, s = +Z, e = +X, w = -X).
ItemPlace torch_place() {
    ItemPlace p;
    p.block = 10;
    p.wall[0] = 11;  // n
    p.wall[1] = 12;  // s
    p.wall[2] = 13;  // e
    p.wall[3] = 14;  // w
    return p;
}

} // namespace

TEST_CASE("an item's placement picks the block its clicked face calls for") {
    const ItemPlace torch = torch_place();

    SUBCASE("a top face places the standing block") {
        CHECK(torch.resolve(0.0f, 1.0f, 0.0f) == 10);
    }

    SUBCASE("a bottom face places the standing block too") {
        // No ceiling-hanging variant exists, and refusing the click would be a
        // worse surprise than a standing piece in the cell below.
        CHECK(torch.resolve(0.0f, -1.0f, 0.0f) == 10);
    }

    SUBCASE("a wall face picks the variant that hugs that wall") {
        // Clicking the +Z face puts the piece on the new cell's -Z side: n.
        CHECK(torch.resolve(0.0f, 0.0f, 1.0f) == 11);
        // ...and -Z face -> the +Z side: s.
        CHECK(torch.resolve(0.0f, 0.0f, -1.0f) == 12);
        // +X face -> the -X side: w.
        CHECK(torch.resolve(1.0f, 0.0f, 0.0f) == 14);
        // -X face -> the +X side: e.
        CHECK(torch.resolve(-1.0f, 0.0f, 0.0f) == 13);
    }

    SUBCASE("a diagonal normal goes to the dominant horizontal axis") {
        // The raycast's normal is face-axis exact in principle; this only has to
        // not fall off an axis when it is not.
        CHECK(torch.resolve(0.1f, 0.0f, 0.9f) == 11);   // Z wins -> n
        CHECK(torch.resolve(0.9f, 0.0f, 0.1f) == 14);   // X wins -> w
        CHECK(torch.resolve(-0.9f, 0.0f, -0.1f) == 13); // X wins -> e
    }

    SUBCASE("a face with no variant falls back to the standing block") {
        ItemPlace only_ground;
        only_ground.block = 10;  // wall[] all AIR
        CHECK(only_ground.resolve(0.0f, 1.0f, 0.0f) == 10);
        CHECK(only_ground.resolve(0.0f, 0.0f, 1.0f) == 10);
        CHECK(only_ground.resolve(1.0f, 0.0f, 0.0f) == 10);
    }

    SUBCASE("a wall-only piece offers nothing on a horizontal face") {
        // block stays AIR, so a top-face click resolves to nothing and the
        // placement path refuses rather than guessing.
        ItemPlace wall_only;
        wall_only.wall[0] = 11;
        CHECK(wall_only.resolve(0.0f, 1.0f, 0.0f) == BlockIDs::AIR);
        CHECK(wall_only.resolve(0.0f, 0.0f, 1.0f) == 11);
    }

    SUBCASE("any() is true exactly when a block is named") {
        CHECK(torch.any());
        ItemPlace empty;
        CHECK_FALSE(empty.any());
        empty.wall[3] = 14;
        CHECK(empty.any());
    }
}
