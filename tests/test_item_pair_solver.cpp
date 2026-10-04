#include "doctest.h"
#include "engine/item_pair_solver.hpp"

#include <godot_cpp/variant/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

// The dropped items' own pair solve (engine/item_pair_solver.cpp), away from
// both the world and the script that owns the items: two bodies, one pass, the
// numbers checked. The scenarios are the GDScript solver's -- a pair that
// overlaps is pushed apart, a closing pair trades momentum, a sleeping body is
// an obstacle, a moving one wakes what it hits -- plus the shape tests that say
// a body is its boxes and not the block around them.

using namespace VoxelEngine;
using namespace godot;

namespace {

// A body's shape as DroppedItems uploads it: the boxes in the body's own space
// and the surface points the impulses act at, flattened with a row per shape.
class Fixture {
public:
    Fixture() {
        // Shape 0: one unit cube.
        add_shape({Box{Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f)}});
        // Shape 1: a slab and a post above one half of it -- a stair, which is
        // two boxes and has a bite taken out of it.
        add_shape({Box{Vector3(0.0f, -0.25f, 0.0f), Vector3(0.5f, 0.25f, 0.5f)},
                   Box{Vector3(0.0f, 0.25f, -0.25f), Vector3(0.5f, 0.25f, 0.25f)}});
    }

    // One body, standing still where it is put. `shape` is a row above.
    void add_body(const Vector3& at, int32_t shape = 0, bool sleeping = false,
                  const Vector3& velocity = Vector3()) {
        positions.push_back(at);
        rotations.push_back(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        velocities.push_back(velocity);
        spins.push_back(Vector3());
        inertia.push_back(Vector3(0.166f, 0.166f, 0.166f));
        reach.push_back(shape_reach[shape]);
        shapes.push_back(shape);
        asleep.push_back(sleeping ? 1 : 0);
        rest.push_back(0.0f);
        grounded.push_back(0);
    }

    void turn(int32_t body, const Vector3& axis, float angle) {
        const Quaternion q(axis, angle);
        rotations[body] = Vector4(q.x, q.y, q.z, q.w);
    }

    // One substep, with the arrays' results left in place for the checks.
    void solve(float delta = 0.0083f) {
        touched.assign(positions.size(), 0);
        woken.assign(positions.size(), 0);
        ItemShapeTable table;
        table.box_offsets = box_offsets.data();
        table.box_halves = box_halves.data();
        table.box_start = box_start.data();
        table.box_count = box_count.data();
        table.points = points.data();
        table.point_start = point_start.data();
        table.point_count = point_count.data();
        table.shape_count = static_cast<int32_t>(box_count.size());

        ItemBodies bodies;
        bodies.positions = positions.data();
        bodies.rotations = rotations.data();
        bodies.velocities = velocities.data();
        bodies.spins = spins.data();
        bodies.inertia = inertia.data();
        bodies.reach = reach.data();
        bodies.shapes = shapes.data();
        bodies.asleep = asleep.data();
        bodies.rest = rest.data();
        bodies.touched = touched.data();
        bodies.woken = woken.data();
        bodies.grounded = grounded.data();
        bodies.count = static_cast<int32_t>(positions.size());
        solver.solve(table, bodies, delta);
    }

    std::vector<Vector3> positions;
    std::vector<Vector4> rotations;
    std::vector<Vector3> velocities;
    std::vector<Vector3> spins;
    std::vector<Vector3> inertia;
    std::vector<float> reach;
    std::vector<int32_t> shapes;
    std::vector<uint8_t> asleep;
    // The world step's own fields. The pair solve never touches them; they are
    // here because both halves of a substep work on the same body struct.
    std::vector<float> rest;
    std::vector<uint8_t> touched;
    std::vector<uint8_t> woken;
    std::vector<uint8_t> grounded;

private:
    struct Box {
        Vector3 centre;
        Vector3 half;
    };

    void add_shape(const std::vector<Box>& boxes) {
        box_start.push_back(static_cast<int32_t>(box_offsets.size()));
        box_count.push_back(static_cast<int32_t>(boxes.size()));
        point_start.push_back(static_cast<int32_t>(points.size()));
        int32_t count = 0;
        float reach = 0.0f;
        for (const Box& box : boxes) {
            box_offsets.push_back(box.centre);
            box_halves.push_back(box.half);
            reach = std::max(reach, box.centre.length() + box.half.length());
            // The box's shell, three points an axis, as DroppedItems lays it out.
            for (int ix = 0; ix < 3; ++ix) {
                for (int iy = 0; iy < 3; ++iy) {
                    for (int iz = 0; iz < 3; ++iz) {
                        if (ix != 0 && ix != 2 && iy != 0 && iy != 2 && iz != 0 && iz != 2) {
                            continue;
                        }
                        points.push_back(box.centre + Vector3(
                            box.half.x * (static_cast<float>(ix) - 1.0f),
                            box.half.y * (static_cast<float>(iy) - 1.0f),
                            box.half.z * (static_cast<float>(iz) - 1.0f)));
                        ++count;
                    }
                }
            }
        }
        point_count.push_back(count);
        // The body's own reach from its origin, which is what its broad phase is.
        shape_reach.push_back(reach);
    }

    ItemPairSolver solver;
    std::vector<Vector3> box_offsets;
    std::vector<Vector3> box_halves;
    std::vector<int32_t> box_start;
    std::vector<int32_t> box_count;
    std::vector<Vector3> points;
    std::vector<int32_t> point_start;
    std::vector<int32_t> point_count;
    std::vector<float> shape_reach;
};

// The GDScript solver's push-out: one overlap, one correction, shared by both
// bodies when both are free. A pair of unit cubes half a block into each other
// is 0.5 deep; 30% of that, less the slop, is corrected, half to each body.
constexpr float kHalfDepthPush = (0.5f - 0.001f) * 0.3f * 0.5f;

} // namespace

TEST_CASE("ItemPairSolver separates two overlapping bodies") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f));
    f.add_body(Vector3(0.5f, 0.0f, 0.0f));
    f.solve();

    CHECK(f.positions[0].x == doctest::Approx(-kHalfDepthPush).epsilon(1e-4));
    CHECK(f.positions[1].x == doctest::Approx(0.5f + kHalfDepthPush).epsilon(1e-4));
    CHECK(f.positions[0].y == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(f.positions[1].z == doctest::Approx(0.0f).epsilon(1e-5));
    // Both are in contact with something, which is what lets each come to rest.
    CHECK(f.touched[0] == 1);
    CHECK(f.touched[1] == 1);
    CHECK(f.woken[0] == 0);
    CHECK(f.woken[1] == 0);
    // Nothing was closing, so nothing was answered with an impulse.
    CHECK(f.velocities[0].length() == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(f.velocities[1].length() == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("ItemPairSolver leaves a pair that is not touching alone") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f), 0, false, Vector3(2.0f, 0.0f, 0.0f));
    f.add_body(Vector3(1.5f, 0.0f, 0.0f));
    f.solve();

    CHECK(f.positions[1].x == doctest::Approx(1.5f).epsilon(1e-6));
    CHECK(f.velocities[0].x == doctest::Approx(2.0f).epsilon(1e-6));
    CHECK(f.velocities[1].length() == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(f.touched[0] == 0);
    CHECK(f.touched[1] == 0);
}

TEST_CASE("ItemPairSolver answers a closing pair with an impulse") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f), 0, false, Vector3(2.0f, 0.0f, 0.0f));
    f.add_body(Vector3(0.98f, 0.0f, 0.0f));
    f.solve();

    // The moving body slows and the still one is thrown forward: a throw hands
    // its momentum over instead of passing through.
    CHECK(f.velocities[0].x < 2.0f);
    CHECK(f.velocities[1].x > 0.0f);
    // Every impulse is answered equal and opposite, so the pair's momentum
    // along the contact normal is what it was.
    CHECK(f.velocities[0].x + f.velocities[1].x == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(f.touched[1] == 1);
}

TEST_CASE("ItemPairSolver holds a sleeping body still as an obstacle") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f));
    f.add_body(Vector3(0.98f, 0.0f, 0.0f), 0, true);
    f.solve();

    // A slow body leaning on a sleeping one does not wake it (gravity's own
    // tickle is well under the wake speed), and the sleeping body is neither
    // moved nor pushed out of: the free body takes the whole correction.
    CHECK(f.asleep[1] == 1);
    CHECK(f.woken[1] == 0);
    CHECK(f.positions[1].x == doctest::Approx(0.98f).epsilon(1e-6));
    CHECK(f.positions[0].x == doctest::Approx(-(0.02f - 0.001f) * 0.3f).epsilon(1e-5));
}

TEST_CASE("ItemPairSolver wakes a sleeping body something fast hits") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f), 0, false, Vector3(3.0f, 0.0f, 0.0f));
    f.add_body(Vector3(0.98f, 0.0f, 0.0f), 0, true);
    f.solve();

    CHECK(f.asleep[1] == 0);
    CHECK(f.woken[1] == 1);
    CHECK(f.velocities[1].x > 0.0f);
}

TEST_CASE("ItemPairSolver ignores two sleeping bodies") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f), 0, true);
    f.add_body(Vector3(0.4f, 0.0f, 0.0f), 0, true);
    f.solve();

    CHECK(f.positions[0].x == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(f.positions[1].x == doctest::Approx(0.4f).epsilon(1e-6));
    CHECK(f.touched[0] == 0);
    CHECK(f.touched[1] == 0);
}

TEST_CASE("ItemPairSolver's broad phase gates the narrow one") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f));
    f.add_body(Vector3(0.5f, 0.0f, 0.0f));
    // A reach that cannot cover the pair: the boxes DO overlap, and are left
    // alone all the same, because the pair is never handed to the box maths.
    f.reach[0] = 0.1f;
    f.reach[1] = 0.1f;
    f.solve();

    CHECK(f.positions[0].x == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(f.touched[0] == 0);
}

TEST_CASE("ItemPairSolver separates a turned pair it can only find by an edge axis") {
    Fixture f;
    f.add_body(Vector3(0.0f, 0.0f, 0.0f));
    f.add_body(Vector3(0.62f, 0.0f, 0.62f));
    // Turned 45 degrees about Y, the second cube's edge points at the first:
    // their faces overlap, and the shortest way out is the one an edge cross
    // product names, not a face of either box.
    f.turn(1, Vector3(0.0f, 1.0f, 0.0f), 0.7853982f);
    f.solve();
    CHECK(f.touched[0] == 1);

    // Solved over several substeps the pair comes apart, and then STOPS: the
    // push-out is a correction toward no overlap, not a jitter that keeps
    // moving the pair every substep.
    for (int i = 0; i < 40; ++i) {
        f.solve();
    }
    const Vector3 settled = f.positions[1];
    for (int i = 0; i < 20; ++i) {
        f.solve();
    }
    CHECK(f.positions[1].x == doctest::Approx(settled.x).epsilon(1e-4));
    CHECK(f.positions[1].z == doctest::Approx(settled.z).epsilon(1e-4));
    CHECK((f.positions[1] - f.positions[0]).length() > 1.0f);
    // No velocity was ever involved, so the push-out is the only motion.
    CHECK(f.velocities[0].length() == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(f.velocities[1].length() == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("ItemPairSolver treats a body as its own boxes") {
    Fixture f;
    // A stair is a slab plus a post over half of it, and the other half of the
    // upper cell is the bite: a cube that shares the cell, and only the bite,
    // must not touch it.
    f.add_body(Vector3(0.0f, 0.0f, 0.0f), 1);
    f.add_body(Vector3(0.0f, 0.85f, 0.5f));
    f.solve();

    CHECK(f.touched[0] == 0);
    CHECK(f.positions[1].y == doctest::Approx(0.85f).epsilon(1e-6));

    // The same cube over the half the stair does have, and the pair solves.
    f.positions[1] = Vector3(0.0f, 0.85f, -0.5f);
    f.solve();
    CHECK(f.touched[0] == 1);
    CHECK(f.touched[1] == 1);
}
