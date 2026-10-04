#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "core/block_types.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/item_body_solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// The world half of a dropped item's substep (engine/item_body_solver.cpp),
// against a real chunk map and the real collision resolver: a body falls, lands
// on what the world says is there, and comes to rest -- and a body lands on
// ANOTHER body, because the step is both halves at once. The pair maths itself
// is tests/test_item_pair_solver.cpp's.

using namespace VoxelEngine;
using namespace godot;

using chunktest::make_test_chunk;

namespace {

// One unit cube, and the bodies that fall: the shape half of the solver's input
// as DroppedItems uploads it (three surface points an axis, a box the size of
// the body).
class Drop {
public:
    Drop() {
        box_offsets.push_back(Vector3(0.0f, 0.0f, 0.0f));
        box_halves.push_back(Vector3(0.5f, 0.5f, 0.5f));
        box_start.push_back(0);
        box_count.push_back(1);
        point_start.push_back(0);
        int32_t count = 0;
        for (int ix = 0; ix < 3; ++ix) {
            for (int iy = 0; iy < 3; ++iy) {
                for (int iz = 0; iz < 3; ++iz) {
                    if (ix != 0 && ix != 2 && iy != 0 && iy != 2 && iz != 0 && iz != 2) {
                        continue;
                    }
                    points.push_back(Vector3((static_cast<float>(ix) - 1.0f) * 0.5f,
                                             (static_cast<float>(iy) - 1.0f) * 0.5f,
                                             (static_cast<float>(iz) - 1.0f) * 0.5f));
                    ++count;
                }
            }
        }
        point_count.push_back(count);
        tables.push_back(0);
    }

    void add(const Vector3& at) {
        positions.push_back(at);
        rotations.push_back(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        velocities.push_back(Vector3());
        spins.push_back(Vector3());
        inertia.push_back(Vector3(0.166f, 0.166f, 0.166f));
        reach.push_back(0.866f);
        shapes.push_back(0);
        asleep.push_back(0);
        rest.push_back(0.0f);
        touched.push_back(0);
        woken.push_back(0);
        grounded.push_back(0);
    }

    ItemShapeTable shape_table() const {
        ItemShapeTable table;
        table.box_offsets = box_offsets.data();
        table.box_halves = box_halves.data();
        table.box_start = box_start.data();
        table.box_count = box_count.data();
        table.points = points.data();
        table.point_start = point_start.data();
        table.point_count = point_count.data();
        table.shape_count = 1;
        return table;
    }

    ItemBodies bodies() {
        ItemBodies out;
        out.positions = positions.data();
        out.rotations = rotations.data();
        out.velocities = velocities.data();
        out.spins = spins.data();
        out.inertia = inertia.data();
        out.reach = reach.data();
        out.shapes = shapes.data();
        out.asleep = asleep.data();
        out.rest = rest.data();
        out.touched = touched.data();
        out.woken = woken.data();
        out.grounded = grounded.data();
        out.count = static_cast<int32_t>(positions.size());
        return out;
    }

    void step(ItemBodySolver& solver, int32_t steps) {
        for (int32_t i = 0; i < steps; ++i) {
            ItemShapeTable table = shape_table();
            ItemBodies state = bodies();
            solver.step(table, state, 1.0f / 120.0f);
        }
    }

    std::vector<Vector3> positions;
    std::vector<Vector4> rotations;
    std::vector<Vector3> velocities;
    std::vector<Vector3> spins;
    std::vector<Vector3> inertia;
    std::vector<float> reach;
    std::vector<int32_t> shapes;
    std::vector<uint8_t> asleep;
    std::vector<float> rest;
    std::vector<uint8_t> touched;
    std::vector<uint8_t> woken;
    std::vector<uint8_t> grounded;

private:
    std::vector<Vector3> box_offsets;
    std::vector<Vector3> box_halves;
    std::vector<int32_t> box_start;
    std::vector<int32_t> box_count;
    std::vector<Vector3> points;
    std::vector<int32_t> point_start;
    std::vector<int32_t> point_count;
    std::vector<int32_t> tables;
};

// A chunk with a stone floor at y=0, top at y=1.0, over 4x4 cells.
void make_floor(ChunkMap& cm) {
    cm.insert(cm.get_chunk_key(0, 0, 0),
              make_test_chunk([] {
                  auto d = std::make_unique<ChunkData>();
                  for (int x = 0; x < 4; ++x)
                      for (int z = 0; z < 4; ++z)
                          d->set_block(x, 0, z, BlockIDs::STONE);
                  return d;
              }()));
}

} // namespace

TEST_CASE("ItemBodySolver drops a body onto the world's floor") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    make_floor(cm);
    CollisionResolver cr(&cm);
    ItemBodySolver solver(&cr);

    Drop drop;
    drop.add(Vector3(1.5f, 3.5f, 1.5f));
    drop.step(solver, 300);

    // It fell, it did not pass through, and it is lying on the floor's top: the
    // body is a cube, so its centre is half a block above the surface.
    CHECK(drop.asleep[0] == 1);
    CHECK(drop.grounded[0] == 1);
    CHECK(drop.positions[0].y == doctest::Approx(1.5f).epsilon(0.02f));
    CHECK(drop.positions[0].x == doctest::Approx(1.5f).epsilon(0.05f));
    CHECK(drop.velocities[0].length() == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("ItemBodySolver falls through what the world does not stop") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    cm.insert(cm.get_chunk_key(0, 0, 0),
              make_test_chunk([] {
                  auto d = std::make_unique<ChunkData>();
                  for (int x = 0; x < 4; ++x)
                      for (int z = 0; z < 4; ++z) {
                          d->set_block(x, 0, z, BlockIDs::STONE);
                          // Water is not solid: the body is drawn through it, and
                          // collides with it no more than with the air.
                          d->set_block(x, 1, z, BlockIDs::WATER);
                          d->set_block(x, 2, z, BlockIDs::WATER);
                      }
                  return d;
              }()));
    CollisionResolver cr(&cm);
    ItemBodySolver solver(&cr);

    Drop drop;
    drop.add(Vector3(1.5f, 4.5f, 1.5f));
    drop.step(solver, 300);

    CHECK(drop.asleep[0] == 1);
    CHECK(drop.positions[0].y == doctest::Approx(1.5f).epsilon(0.02f));
}

// A body dropped with the seams of the cells under it running through its own
// landing points: it has to settle in its own column. A point on a seam is within
// the contact radius of both cells, and answered wrong -- by the face of a cell it
// is merely beside instead of the top face holding it up -- the body is pushed
// off the spot it landed on, one side only, and walks.
//
// The bound is loose on purpose. A drop lands a couple of centimetres off centre
// even so, because the landing impulse is delivered through the first contact the
// sweep reaches and a corner answers harder than a face; tightening it is a
// change to the impulse solve (distributing the bounce over the whole landing
// face), not to this rule. What this holds is that a drop stays in its own column
// instead of leaving it.
TEST_CASE("ItemBodySolver lands a seam drop in its own column") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    make_floor(cm);
    CollisionResolver cr(&cm);
    ItemBodySolver solver(&cr);

    Drop drop;
    drop.add(Vector3(1.5f, 3.5f, 1.5f));  // the seams at x=1,2 and z=1,2 run under it
    drop.step(solver, 600);

    CHECK(drop.asleep[0] == 1);
    CHECK(std::abs(drop.positions[0].x - 1.5f) < 0.1f);
    CHECK(std::abs(drop.positions[0].z - 1.5f) < 0.1f);
}

// The landing zone: a dense lattice of blocks dropped as one mass, which lands in
// contact with itself and with the floor. What settles it is the friction of the
// resting contacts, and a landing point that is answered with a wall instead of
// the floor's own top face has none to give. A pile that comes apart after it has
// settled is the signature: bodies awake and sliding a whole second after the
// last of them landed.
TEST_CASE("ItemBodySolver settles a dense pile in place") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    // A floor wide enough that the pile's own spreading has somewhere to go: a
    // body that slides off the edge falls for good, and then nothing about its
    // landing is being measured any more.
    cm.insert(cm.get_chunk_key(0, 0, 0),
              make_test_chunk([] {
                  auto d = std::make_unique<ChunkData>();
                  for (int x = 0; x < 12; ++x)
                      for (int z = 0; z < 12; ++z)
                          d->set_block(x, 0, z, BlockIDs::STONE);
                  return d;
              }()));
    CollisionResolver cr(&cm);
    ItemBodySolver solver(&cr);

    const Vector3 centre(6.0f, 4.5f, 6.0f);
    Drop drop;
    for (int i = 0; i < 9; ++i) {
        const float gx = static_cast<float>(i % 3) - 1.0f;
        const float gz = static_cast<float>(i / 3) - 1.0f;
        drop.add(centre + Vector3(gx * 1.02f, 0.0f, gz * 1.02f));
    }
    drop.step(solver, 900);
    const std::vector<Vector3> settled = drop.positions;
    drop.step(solver, 120);  // one more second of solving

    float worst = 0.0f;
    int32_t awake = 0;
    int32_t stray = 0;
    for (size_t i = 0; i < drop.positions.size(); ++i) {
        const Vector3 at = drop.positions[i];
        // Off the floor's own top, or outside its footprint: it did not land.
        if (at.y < 1.0f - 0.05f || at.x < 0.0f || at.x > 12.0f || at.z < 0.0f || at.z > 12.0f) {
            ++stray;
            continue;
        }
        worst = std::max(worst, (at - settled[i]).length());
        if (drop.asleep[i] == 0) {
            ++awake;
        }
    }
    CHECK(stray == 0);
    CHECK(awake == 0);
    CHECK(worst < 0.2f);
}

TEST_CASE("ItemBodySolver stacks one body on another") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    make_floor(cm);
    CollisionResolver cr(&cm);
    ItemBodySolver solver(&cr);

    Drop drop;
    drop.add(Vector3(1.5f, 1.5f, 1.5f));  // resting on the floor already
    drop.add(Vector3(1.5f, 2.9f, 1.5f));  // a drop above it
    drop.step(solver, 400);



    // Both bodies come to rest, and the upper one rests ON the lower: a stack is
    // a stack because the pair solve holds the top one up, not because it sank
    // into it.
    CHECK(drop.velocities[0].length() < 0.02f);
    CHECK(drop.velocities[1].length() < 0.02f);
    CHECK(drop.spins[0].length() < 0.02f);
    CHECK(drop.spins[1].length() < 0.02f);
    CHECK(drop.grounded[0] == 1);
    CHECK(drop.grounded[1] == 1);
    CHECK(drop.rest[0] > 0.0f);
    CHECK(drop.rest[1] > 0.0f);
    CHECK(drop.asleep[0] == 1);
    CHECK(drop.asleep[1] == 1);
    CHECK(drop.positions[0].y == doctest::Approx(1.5f).epsilon(0.05f));
    CHECK(drop.positions[1].y - drop.positions[0].y == doctest::Approx(1.0f).epsilon(0.1f));
    CHECK(drop.positions[1].y > 2.3f);
}
