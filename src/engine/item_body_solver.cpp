#include "engine/item_body_solver.hpp"

#include "engine/collision_resolver.hpp"

#include <godot_cpp/variant/quaternion.hpp>

#include <algorithm>
#include <cmath>

// A dropped item's substep, natively. Ported from scripts/dropped_items.gd's
// _substep / _settle / _solve_item_pairs (the pair half is now its own class):
// the comments that explain WHY each step is what it is are on that history and
// in item_body_solver.hpp. What is here is the order and the numbers.

namespace VoxelEngine {

using namespace godot;

namespace {

// The item physics, in blocks and seconds, exactly as the script had it. These
// are the GRAVITY / *_DAMPING / BOUNCE / FRICTION / REST_* constants: they live
// here now because here is where they act.
constexpr float kGravity = 16.0f;         // a touch under the player's
constexpr float kMaxSpeed = 60.0f;        // terminal speed, so a long fall cannot tunnel
constexpr float kLinearDamping = 0.25f;   // fraction of the velocity shed per second, in the air
constexpr float kAngularDamping = 0.30f;  // and of the spin
constexpr float kBounce = 0.25f;          // share of an approach speed a contact gives back
constexpr float kRestingApproach = 0.5f;  // below this a contact does not bounce at all
constexpr float kFriction = 0.6f;         // share of the tangential motion a contact takes
constexpr int32_t kIterations = 6;        // passes over the contact set per substep
constexpr float kPushFraction = 0.3f;     // share of a penetration corrected per substep
constexpr float kPushSlop = 0.001f;       // penetration shallow enough to leave alone
constexpr float kContactRadius = 0.02f;   // how far outside a cell a point still counts
constexpr float kRestSpeed = 0.12f;       // at rest: slow enough ...
constexpr float kRestSpin = 0.35f;        // ... and barely turning ...
constexpr float kRestTime = 0.3f;         // ... and staying that way for this long

} // namespace

void ItemBodySolver::step(const ItemShapeTable& shapes, ItemBodies& bodies, float delta) {
    if (bodies.count <= 0 || resolver_ == nullptr) {
        return;
    }
    // Every body's world half first, in order, and only then the pairs: stepping
    // one body to the end of the frame at a time would let it clear a body that
    // had not moved yet.
    supported_.assign(static_cast<size_t>(bodies.count), 0);
    for (int32_t i = 0; i < bodies.count; ++i) {
        if (bodies.asleep[i] == 0) {
            step_body(shapes, bodies, i, delta);
        }
    }
    pairs_.solve(shapes, bodies, delta);
    // What the pair half has to say about each body's rest.
    //
    // The two halves judge DIFFERENT bodies, and that is the point: a body the
    // world is holding up was stopped by the world, and the pair solve may hand
    // it the weight of whatever is stacked on it a moment later -- reading the
    // rest off THAT would call a body that is not moving "not at rest" every
    // substep, and a stack could never sleep. A body the world is NOT holding is
    // held by the pair, and there the pair's own answer is the right one.
    for (int32_t i = 0; i < bodies.count; ++i) {
        if (bodies.asleep[i] != 0) {
            continue;
        }
        // A woken body's rest timer is not the one it slept with, or what is
        // left of it would put the body straight back to sleep.
        if (bodies.woken[i] != 0) {
            bodies.rest[i] = 0.0f;
        }
        const bool held_by_pair = bodies.touched[i] != 0;
        bodies.grounded[i] = (supported_[i] != 0 || held_by_pair) ? 1 : 0;
        if (supported_[i] == 0) {
            if (held_by_pair) {
                if (at_rest(bodies, i)) {
                    bodies.rest[i] += delta;
                } else {
                    bodies.rest[i] = 0.0f;
                }
            } else {
                // Nothing is holding it at all: it is in the air, and whatever
                // rest it had is not a rest it is on now.
                bodies.rest[i] = 0.0f;
            }
        }
        if (bodies.rest[i] >= kRestTime) {
            bodies.asleep[i] = 1;
            bodies.velocities[i] = Vector3();
            bodies.spins[i] = Vector3();
        }
    }
}

void ItemBodySolver::step_body(const ItemShapeTable& shapes, ItemBodies& bodies, int32_t index,
                               float delta) {
    // Gravity and damping, then the turn and the move.
    Vector3 velocity = bodies.velocities[index];
    velocity.y -= kGravity * delta;
    velocity *= std::max(0.0f, 1.0f - kLinearDamping * delta);
    if (velocity.length() > kMaxSpeed) {
        velocity = velocity.normalized() * kMaxSpeed;
    }
    Vector3 spin = bodies.spins[index] * std::max(0.0f, 1.0f - kAngularDamping * delta);
    // The rotation is integrated BEFORE the contacts are found: done after, the
    // body turns into the world every substep with nothing solving it, which is
    // what a block resting on its edge clips through the floor with.
    if (spin.length_squared() > 0.000001f) {
        const Quaternion turn(spin.normalized(), spin.length() * delta);
        const Quaternion current = Quaternion(bodies.rotations[index].x, bodies.rotations[index].y,
                                              bodies.rotations[index].z, bodies.rotations[index].w);
        bodies.rotations[index] = item_rotation_of((turn * current).normalized());
    }
    bodies.velocities[index] = velocity;
    bodies.spins[index] = spin;
    bodies.positions[index] += velocity * delta;

    const Basis basis = item_basis_of(bodies.rotations[index]);
    const Basis transposed = basis.transposed();
    const Vector3 inertia = bodies.inertia[index];
    const int32_t shape = bodies.shapes[index];

    // The body's own points, in world space: what the world is asked about. The
    // shape decides where they are, so a stair is asked as its two boxes' points
    // and not as the corners of the block around them.
    points_.clear();
    for (int32_t p = 0; p < shapes.point_count[shape]; ++p) {
        points_.push_back(bodies.positions[index]
            + basis.xform(shapes.points[shapes.point_start[shape] + p]));
    }
    const std::vector<CollisionResolver::PointContact> contacts =
        resolver_->contacts_for_points(points_.data(), points_.size(), kContactRadius);

    // The exact convex guard, and the body's ONLY push-out: points can only
    // report the places they were put, so a block sitting against the MIDDLE of
    // an edge lies between two of them and the edge passes through the world.
    box_offsets_.clear();
    box_halves_.clear();
    for (int32_t b = 0; b < shapes.box_count[shape]; ++b) {
        box_offsets_.push_back(shapes.box_offsets[shapes.box_start[shape] + b]);
        box_halves_.push_back(shapes.box_halves[shapes.box_start[shape] + b]);
    }
    const CollisionResolver::TurnedContact guard = resolver_->turned_boxes_contact(
        bodies.positions[index], box_offsets_, box_halves_, basis);

    // The points answer MOTION and nothing else, several times over, so the ones
    // that touch agree on one motion instead of fighting. The penetration is not
    // pushed out here -- a face lying on the floor reports nine points at once,
    // and moving the body out of each would move it out nine times over. One
    // overlap, one correction: the guard is that correction.
    bool touching = false;
    for (int32_t iteration = 0; iteration < kIterations; ++iteration) {
        // The sweep changes ends every pass. One pass over a landing face turns the
        // body a little -- each impulse moves the body, and the ones after it are
        // answered against the motion they made -- and the contacts are reached in
        // the same order every substep, so that residue points the same way every
        // time: a settled block walked in the direction its corner contacts were
        // seen in. Reaching them from both ends leaves no such direction.
        const bool reverse = (iteration & 1) != 0;
        for (size_t pass = 0; pass < contacts.size(); ++pass) {
            const size_t ci = reverse ? contacts.size() - 1 - pass : pass;
            const CollisionResolver::PointContact& contact = contacts[ci];
            // A contact is only useful while it pushes the way the whole body has
            // to go: a point sunk past the middle of a thin cell reports the face
            // it is NEAREST, which can be the one it came in through, and an
            // impulse along that face would fight the push-out.
            if (guard.into && contact.normal.dot(guard.normal) < 0.0f) {
                continue;
            }
            const Vector3 r = contact.point - bodies.positions[index];
            const Vector3& normal = contact.normal;
            // Answer the motion this point actually sees, at the point itself:
            // the difference between a box that slides and a body that turns.
            const Vector3 point_velocity = bodies.velocities[index] + bodies.spins[index].cross(r);
            const float approach = point_velocity.dot(normal);
            if (approach >= 0.0f) {
                continue;
            }
            touching = true;
            // Bounce only a real impact: from below the threshold the motion is
            // absorbed, which is what lets a body lie still instead of riding
            // gravity's own tickle up and down forever.
            const float restitution = std::abs(approach) > kRestingApproach ? kBounce : 0.0f;
            const Vector3 rn = r.cross(normal);
            const float denom = 1.0f
                + rn.dot(item_inertia_xform(basis, transposed, inertia, rn));
            const float impulse = -(1.0f + restitution) * approach / std::max(denom, 0.0001f);
            bodies.velocities[index] += normal * impulse;
            bodies.spins[index] += item_inertia_xform(basis, transposed, inertia,
                                                     r.cross(normal * impulse));
            // Friction, the tangential part of the same contact, through the same
            // effective mass: what stops a body sliding, and what stops it
            // turning when it is set down on a face.
            const Vector3 tangent = point_velocity - normal * approach;
            if (tangent.length_squared() <= 0.000001f) {
                continue;
            }
            const Vector3 t = tangent.normalized();
            const Vector3 rt = r.cross(t);
            const float t_denom = 1.0f
                + rt.dot(item_inertia_xform(basis, transposed, inertia, rt));
            const float t_impulse = -std::min(tangent.length() * kFriction,
                                              kFriction * std::abs(impulse))
                / std::max(t_denom, 0.0001f);
            bodies.velocities[index] += t * t_impulse;
            bodies.spins[index] += item_inertia_xform(basis, transposed, inertia,
                                                     r.cross(t * t_impulse));
        }
    }

    if (guard.into) {
        touching = true;
        if (guard.depth > kPushSlop) {
            bodies.positions[index] += guard.normal * ((guard.depth - kPushSlop) * kPushFraction);
        }
        // Take the motion into the surface out too, or the body keeps its speed
        // into the world and is pushed out again every substep.
        const float closing = bodies.velocities[index].dot(guard.normal);
        if (closing < 0.0f) {
            bodies.velocities[index] -= guard.normal * closing;
        }
    }

    // Whether the WORLD is holding this body this substep, and how long it has
    // been still while the world holds it. The pair half may add motion on top
    // of that a moment later (the weight of a stack), which is why the rest of a
    // body the world holds is judged HERE and not after both halves (see step).
    supported_[index] = touching ? 1 : 0;
    if (touching) {
        if (at_rest(bodies, index)) {
            bodies.rest[index] += delta;
        } else {
            bodies.rest[index] = 0.0f;
        }
    }
}

bool ItemBodySolver::at_rest(const ItemBodies& bodies, int32_t index) const {
    return bodies.velocities[index].length() < kRestSpeed
        && bodies.spins[index].length() < kRestSpin;
}

} // namespace VoxelEngine
