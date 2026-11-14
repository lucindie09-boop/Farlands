#include "doctest.h"

#include "fluids/generated_water_wake.hpp"

#include <cstdlib>
#include <vector>

using VoxelEngine::BlockID;
using VoxelEngine::fluids::WakeCell;

// The bounded wake's rule, without a world: generated water within the radius is
// reported, everything else is not, and an edit with none within one block costs
// the seven-cell pre-check and nothing more.

namespace {

constexpr BlockID AIR_BLOCK = 0;
constexpr BlockID GENERATED = 40;
constexpr BlockID LIVE = 41;
constexpr BlockID STONE = 42;

// A shore: stone where the player edited, a two-deep strip of generated water
// beside it, and one generated cell far outside the radius.
struct ShoreGrid {
    BlockID at(int32_t x, int32_t y, int32_t z) const {
        if (x == 0 && y == 0 && z == 0) return STONE;
        if (y == 0 && x >= 1 && x <= 2 && z >= -2 && z <= 2) return GENERATED;
        if (y == 0 && x == 6 && z == 0) return GENERATED;
        return AIR_BLOCK;
    }
};

} // namespace

TEST_CASE("water wake: only generated water inside the radius is reported") {
    ShoreGrid grid;
    std::vector<WakeCell> cells;
    auto at = [&](int32_t x, int32_t y, int32_t z) { return grid.at(x, y, z); };
    auto emit = [&](const WakeCell& cell) { cells.push_back(cell); };

    const int found = VoxelEngine::fluids::find_generated_water(at, emit, GENERATED, 0, 0, 0, 2);
    CHECK(found == 10);  // x = 1..2, z = -2..2
    CHECK(cells.size() == 10);
    for (const WakeCell& cell : cells) {
        CHECK(std::abs(cell.x) <= 2);
        CHECK(std::abs(cell.y) <= 2);
        CHECK(std::abs(cell.z) <= 2);
        CHECK(grid.at(cell.x, cell.y, cell.z) == GENERATED);
    }

    cells.clear();
    CHECK(VoxelEngine::fluids::find_generated_water(at, emit, GENERATED, 0, 0, 0, 1) == 3);  // x = 1, z = -1..1
}

TEST_CASE("water wake: an edit with no generated water nearby reports nothing") {
    auto air = [](int32_t, int32_t, int32_t) { return static_cast<BlockID>(AIR_BLOCK); };
    int emitted = 0;
    auto emit = [&](const WakeCell&) { ++emitted; };
    CHECK(VoxelEngine::fluids::find_generated_water(air, emit, GENERATED, 0, 0, 0, 2) == 0);
    CHECK(emitted == 0);

    // A stone wall is not generated water either, even though it is next door.
    auto stone = [](int32_t, int32_t, int32_t) { return STONE; };
    CHECK(VoxelEngine::fluids::find_generated_water(stone, emit, GENERATED, 0, 0, 0, 2) == 0);
}

TEST_CASE("water wake: a disabled radius and an invalid block are no-ops") {
    ShoreGrid grid;
    auto at = [&](int32_t x, int32_t y, int32_t z) { return grid.at(x, y, z); };
    auto emit = [](const WakeCell&) {};
    CHECK(VoxelEngine::fluids::find_generated_water(at, emit, GENERATED, 0, 0, 0, 0) == 0);
    CHECK(VoxelEngine::fluids::find_generated_water(at, emit, AIR_BLOCK, 0, 0, 0, 2) == 0);
}

TEST_CASE("water wake: the pre-check only looks one cell out") {
    // Generated water two cells away wakes nothing until it is in reach: this is
    // the property that keeps a build in the middle of a continent free.
    auto two_away = [](int32_t x, int32_t y, int32_t z) {
        return static_cast<BlockID>(x == 2 && y == 0 && z == 0 ? GENERATED : AIR_BLOCK);
    };
    int emitted = 0;
    auto emit = [&](const WakeCell&) { ++emitted; };
    CHECK(VoxelEngine::fluids::find_generated_water(two_away, emit, GENERATED, 0, 0, 0, 2) == 0);
    CHECK(emitted == 0);

    auto one_away = [](int32_t x, int32_t y, int32_t z) {
        return static_cast<BlockID>(x == 1 && y == 0 && z == 0 ? GENERATED : AIR_BLOCK);
    };
    CHECK(VoxelEngine::fluids::find_generated_water(one_away, emit, GENERATED, 0, 0, 0, 2) == 1);
}
