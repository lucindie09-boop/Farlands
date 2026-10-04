#ifndef FARLANDS_ITEM_PAIR_SOLVER_HPP
#define FARLANDS_ITEM_PAIR_SOLVER_HPP

#include "engine/item_body_state.hpp"

#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

// Item against item: the half of a dropped item's motion that is NOT the world.
//
// A body asks the collision resolver which of its own points are inside solid
// geometry, one body at a time; that answer is what turns it against the ground.
// This is the other half, and the one a pile of blocks spends its time in: every
// PAIR of bodies in the same substep. The maths is the maths the GDScript solver
// used to do here (git history of scripts/dropped_items.gd, now moved wholesale):
//
//   * a distance broad phase, from each body's own reach;
//   * a fifteen-axis separating axis test over the two bodies' boxes -- the
//     pair's guard, which is also the push-out, so one overlap is corrected
//     once;
//   * each body's own surface points, against the other body's boxes, as the
//     contacts that carry the impulses, the turning and the friction;
//   * a sleeping body is an OBSTACLE -- infinite mass -- until something moving
//     faster than a resting body touches it.
//
// What changed is only where it runs. In GDScript every step of that was
// dictionary lookups and a dictionary allocated per contact per substep, so a
// pile cost more the more blocks it held; here the same pass is arithmetic over
// flat arrays, which is what makes a pile of forty blocks affordable.
//
// The caller keeps the state: nothing survives a call but this object's scratch,
// so a pass is one substep and the arrays' contents afterwards are the substep's
// result.
namespace VoxelEngine {

class ItemPairSolver {
public:
    // One substep of item-vs-item motion over the bodies given: the shapes they
    // are (item_body_state.hpp) and the state they are in.
    void solve(const ItemShapeTable& shapes, ItemBodies& bodies, float delta);

private:
    // One point contact of a pair, as the body whose point it is sees it:
    // `mover` is pushed away from `other` along `normal`, and `side` is the sign
    // that normal must carry against the pair guard (a's contacts push a away
    // from b, b's the other way).
    struct Contact {
        int32_t mover = 0;
        int32_t other = 0;
        godot::Vector3 point;
        godot::Vector3 normal;
        float side = 0.0f;
    };

    // The shortest way two bodies overlap, over every pair of their boxes. The
    // normal points from `a` to `b`, so moving `a` along -normal and `b` along
    // +normal separates them.
    struct Guard {
        bool overlap = false;
        godot::Vector3 normal;
        float depth = 0.0f;
    };

    Guard pair_guard(const ItemShapeTable& shapes, const ItemBodies& bodies, int32_t a, int32_t b) const;
    // `mover`'s own points against `other`'s boxes, appended to contacts_.
    void points_into_boxes(const ItemShapeTable& shapes, const ItemBodies& bodies,
                           int32_t mover, int32_t other, float side);
    // One contact, answered for BOTH bodies: the same impulse, opposite ways,
    // each taking it at the point through its own lever arm.
    void apply_impulse(ItemBodies& bodies, const Contact& contact) const;
    void solve_pair(const ItemShapeTable& shapes, ItemBodies& bodies, int32_t a, int32_t b);

    // Reused across pairs and calls: a pile of blocks makes no allocation here
    // beyond the size of the biggest pair's contact set.
    std::vector<Contact> contacts_;
};

} // namespace VoxelEngine

#endif // FARLANDS_ITEM_PAIR_SOLVER_HPP
