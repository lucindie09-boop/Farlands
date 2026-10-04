#include "engine/item_pair_solver.hpp"

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/quaternion.hpp>

#include <algorithm>
#include <cmath>

// The maths of the item-vs-item pass, moved out of GDScript. The text that
// explained it lives on the declarations (item_pair_solver.hpp) and, for the
// parts that are the same idea as the world's solve, on the GDScript it was
// ported from: scripts/dropped_items.gd, where the pair solve used to be.

namespace VoxelEngine {

using namespace godot;

namespace {

// The pair solve's own numbers, in blocks and seconds. They were the GDScript
// solver's constants; they live here now because this is where they act.
constexpr float kBounce = 0.25f;           // share of an approach speed a contact gives back
constexpr float kRestingApproach = 0.5f;   // below this a contact does not bounce at all
constexpr float kFriction = 0.6f;          // share of the tangential motion a contact takes
constexpr float kPushFraction = 0.3f;      // share of a penetration corrected per pass
constexpr float kPushSlop = 0.001f;        // penetration shallow enough to leave alone
constexpr float kContactRadius = 0.02f;    // how far outside a box a point still counts
constexpr float kWakeSpeed = 0.75f;        // how fast a body must move to wake a sleeping one
constexpr int32_t kIterations = 3;         // passes over a pair's contacts per substep

struct BoxOverlap {
    bool overlap = false;
    Vector3 normal;
    float depth = 0.0f;
};

// Two TURNED boxes, by the separating axis test: they overlap while no axis
// separates them, and the axis they overlap along LEAST is the penetration,
// whose direction is the way out. All fifteen axes have to be tried, not just
// the six box faces: two boxes resting corner to corner are separated by the
// cross product of two edges and by nothing else. The normal points from `a` to
// `b` (which is what the guard wants from it), and the first axis that
// separates them ends the test.
BoxOverlap box_pair_overlap(const Vector3& a_centre, const Vector3& a_half, const Basis& a_basis,
                            const Vector3& b_centre, const Vector3& b_half, const Basis& b_basis) {
    BoxOverlap out;
    const Vector3 delta = b_centre - a_centre;
    const Vector3 a_axes[3] = {a_basis.get_column(0), a_basis.get_column(1), a_basis.get_column(2)};
    const Vector3 b_axes[3] = {b_basis.get_column(0), b_basis.get_column(1), b_basis.get_column(2)};
    // The six faces first, then the nine edge cross products, in the same order
    // the GDScript tried them: a tie between two axes is broken by which came
    // first, and the axes' order is part of the answer.
    Vector3 axes[15];
    for (int i = 0; i < 3; ++i) {
        axes[i] = a_axes[i];
        axes[3 + i] = b_axes[i];
        for (int j = 0; j < 3; ++j) {
            axes[6 + i * 3 + j] = a_axes[i].cross(b_axes[j]);
        }
    }
    float best_depth = INFINITY;
    Vector3 best_normal(0.0f, 1.0f, 0.0f);
    for (int k = 0; k < 15; ++k) {
        const Vector3 axis = axes[k];
        if (axis.length_squared() < 0.000001f) {
            continue;
        }
        const Vector3 n = axis.normalized();
        const float ra = std::abs(n.dot(a_axes[0])) * a_half.x
                       + std::abs(n.dot(a_axes[1])) * a_half.y
                       + std::abs(n.dot(a_axes[2])) * a_half.z;
        const float rb = std::abs(n.dot(b_axes[0])) * b_half.x
                       + std::abs(n.dot(b_axes[1])) * b_half.y
                       + std::abs(n.dot(b_axes[2])) * b_half.z;
        const float overlap = ra + rb - std::abs(n.dot(delta));
        if (overlap <= 0.0f) {
            return out;  // a separating axis: not touching at all
        }
        if (overlap < best_depth) {
            best_depth = overlap;
            best_normal = n.dot(delta) >= 0.0f ? n : -n;
        }
    }
    out.overlap = true;
    out.normal = best_normal;
    out.depth = best_depth;
    return out;
}

} // namespace

void ItemPairSolver::solve(const ItemShapeTable& shapes, ItemBodies& bodies, float delta) {
    if (bodies.count < 2 || shapes.shape_count <= 0) {
        return;
    }
    if (bodies.touched != nullptr) {
        for (int32_t i = 0; i < bodies.count; ++i) {
            bodies.touched[i] = 0;
        }
    }
    if (bodies.woken != nullptr) {
        for (int32_t i = 0; i < bodies.count; ++i) {
            bodies.woken[i] = 0;
        }
    }
    for (int32_t a = 0; a < bodies.count; ++a) {
        for (int32_t b = a + 1; b < bodies.count; ++b) {
            // Two sleeping bodies are over and done with: neither can move the
            // other, and the pile they are part of has already settled.
            if (bodies.asleep[a] != 0 && bodies.asleep[b] != 0) {
                continue;
            }
            solve_pair(shapes, bodies, a, b);
        }
    }
}

// The pair solve moves bodies and answers motion; it does not age them. Whether
// a body has been still for long enough to sleep is the caller's `_settle`, on
// the same terms as the world's contacts.
void ItemPairSolver::solve_pair(const ItemShapeTable& shapes, ItemBodies& bodies, int32_t a, int32_t b) {
    // Broad phase: the bodies' own reaches, so the box math below only runs for
    // the pairs that could possibly touch. Squared, so no square root is spent
    // on the pairs that do not.
    const float reach_sum = bodies.reach[a] + bodies.reach[b];
    if ((bodies.positions[a] - bodies.positions[b]).length_squared() > reach_sum * reach_sum) {
        return;
    }
    const Guard guard = pair_guard(shapes, bodies, a, b);
    if (!guard.overlap) {
        return;
    }

    // A body that is moving has HIT the sleeping one; a body only leaning on it
    // has not, and the sleeping body stays an obstacle. Gravity's own tickle
    // between substeps is well under the threshold, which is what lets a stack
    // settle instead of being jogged awake by the weight above it.
    if (bodies.asleep[a] != 0 && bodies.velocities[b].length() > kWakeSpeed) {
        bodies.asleep[a] = 0;
        bodies.woken[a] = 1;
    }
    if (bodies.asleep[b] != 0 && bodies.velocities[a].length() > kWakeSpeed) {
        bodies.asleep[b] = 0;
        bodies.woken[b] = 1;
    }

    // Both directions, so the turning is carried by whichever body's points are
    // actually inside the other. Only the contacts that agree with the guard's
    // separation are kept: a point sunk past the middle of the other body
    // reports the face it is nearest, which can be the one it came in through,
    // and an impulse along that face would fight the push-out.
    contacts_.clear();
    points_into_boxes(shapes, bodies, a, b, -1.0f);
    points_into_boxes(shapes, bodies, b, a, 1.0f);
    size_t kept = 0;
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& contact = contacts_[i];
        if (contact.normal.dot(guard.normal) * contact.side >= 0.0f) {
            contacts_[kept++] = contact;
        }
    }
    contacts_.resize(kept);
    for (int32_t iteration = 0; iteration < kIterations; ++iteration) {
        for (const Contact& contact : contacts_) {
            apply_impulse(bodies, contact);
        }
    }

    // The pair's push-out, split between the bodies: both can be moved, so each
    // takes half, and an obstacle takes none of it.
    if (guard.depth > kPushSlop) {
        const float correction = (guard.depth - kPushSlop) * kPushFraction;
        const bool a_free = bodies.asleep[a] == 0;
        const bool b_free = bodies.asleep[b] == 0;
        if (a_free && b_free) {
            bodies.positions[a] -= guard.normal * (correction * 0.5f);
            bodies.positions[b] += guard.normal * (correction * 0.5f);
        } else if (a_free) {
            bodies.positions[a] -= guard.normal * correction;
        } else if (b_free) {
            bodies.positions[b] += guard.normal * correction;
        }
    }

    // Both bodies are touching something, which is half of what coming to rest
    // takes: the caller settles them on the same terms as the world's contacts.
    bodies.touched[a] = 1;
    bodies.touched[b] = 1;
}

ItemPairSolver::Guard ItemPairSolver::pair_guard(const ItemShapeTable& shapes, const ItemBodies& bodies,
                                                 int32_t a, int32_t b) const {
    const Basis a_basis = item_basis_of(bodies.rotations[a]);
    const Basis b_basis = item_basis_of(bodies.rotations[b]);
    const int32_t a_shape = bodies.shapes[a];
    const int32_t b_shape = bodies.shapes[b];
    const Vector3 a_position = bodies.positions[a];
    const Vector3 b_position = bodies.positions[b];
    // The deepest overlap over every pair of the bodies' boxes. A body is a SET
    // of boxes -- a stair is two -- so the pair is not one box each but all of
    // their boxes against all of the other's.
    Guard best;
    for (int32_t i = 0; i < shapes.box_count[a_shape]; ++i) {
        const int32_t a_box = shapes.box_start[a_shape] + i;
        const Vector3 a_centre = a_position + a_basis.xform(shapes.box_offsets[a_box]);
        for (int32_t j = 0; j < shapes.box_count[b_shape]; ++j) {
            const int32_t b_box = shapes.box_start[b_shape] + j;
            const Vector3 b_centre = b_position + b_basis.xform(shapes.box_offsets[b_box]);
            const BoxOverlap hit = box_pair_overlap(a_centre, shapes.box_halves[a_box], a_basis,
                                                    b_centre, shapes.box_halves[b_box], b_basis);
            if (hit.overlap && hit.depth > best.depth) {
                best.overlap = true;
                best.normal = hit.normal;
                best.depth = hit.depth;
            }
        }
    }
    return best;
}

void ItemPairSolver::points_into_boxes(const ItemShapeTable& shapes, const ItemBodies& bodies,
                                       int32_t mover, int32_t other, float side) {
    const Basis mover_basis = item_basis_of(bodies.rotations[mover]);
    const Basis other_basis = item_basis_of(bodies.rotations[other]);
    const Basis other_transposed = other_basis.transposed();
    const Vector3 mover_position = bodies.positions[mover];
    const Vector3 other_position = bodies.positions[other];
    const int32_t mover_shape = bodies.shapes[mover];
    const int32_t other_shape = bodies.shapes[other];
    const int32_t mover_points = shapes.point_count[mover_shape];
    const int32_t other_boxes = shapes.box_count[other_shape];
    for (int32_t pi = 0; pi < mover_points; ++pi) {
        const Vector3 point = mover_position
            + mover_basis.xform(shapes.points[shapes.point_start[mover_shape] + pi]);
        for (int32_t bi = 0; bi < other_boxes; ++bi) {
            const int32_t box = shapes.box_start[other_shape] + bi;
            const Vector3 centre = other_position + other_basis.xform(shapes.box_offsets[box]);
            const Vector3 half = shapes.box_halves[box];
            const Vector3 local = other_transposed.xform(point - centre);
            if (std::abs(local.x) > half.x + kContactRadius
                || std::abs(local.y) > half.y + kContactRadius
                || std::abs(local.z) > half.z + kContactRadius) {
                continue;
            }
            // Pushed out of the face the point is nearest, in `other`'s own
            // space and then turned back out into the world.
            const float dx = half.x - std::abs(local.x);
            const float dy = half.y - std::abs(local.y);
            const float dz = half.z - std::abs(local.z);
            Vector3 normal;
            if (dx <= dy && dx <= dz) {
                normal = other_basis.get_column(0) * (local.x >= 0.0f ? 1.0f : -1.0f);
            } else if (dz <= dy) {
                normal = other_basis.get_column(2) * (local.z >= 0.0f ? 1.0f : -1.0f);
            } else {
                normal = other_basis.get_column(1) * (local.y >= 0.0f ? 1.0f : -1.0f);
            }
            Contact contact;
            contact.mover = mover;
            contact.other = other;
            contact.point = point;
            contact.normal = normal;
            contact.side = side;
            contacts_.push_back(contact);
        }
    }
}

void ItemPairSolver::apply_impulse(ItemBodies& bodies, const Contact& contact) const {
    const int32_t mover = contact.mover;
    const int32_t other = contact.other;
    // A sleeping body is static here -- infinite mass -- so a resting item is
    // not pushed by a body that is only leaning on it, and does not have to be
    // woken to hold it up.
    const float mover_free = bodies.asleep[mover] != 0 ? 0.0f : 1.0f;
    const float other_free = bodies.asleep[other] != 0 ? 0.0f : 1.0f;
    if (mover_free == 0.0f && other_free == 0.0f) {
        return;
    }
    const Basis mover_basis = item_basis_of(bodies.rotations[mover]);
    const Basis other_basis = item_basis_of(bodies.rotations[other]);
    const Basis mover_transposed = mover_basis.transposed();
    const Basis other_transposed = other_basis.transposed();
    const Vector3 r_mover = contact.point - bodies.positions[mover];
    const Vector3 r_other = contact.point - bodies.positions[other];
    const Vector3 mover_inertia = bodies.inertia[mover];
    const Vector3 other_inertia = bodies.inertia[other];
    // Where the point is moving NOW, at the point itself and not at the body's
    // centre: this is the difference between a box that slides and a body that
    // turns, and it is what makes a corner catch and topple instead of being
    // ignored.
    const Vector3 mover_velocity = bodies.velocities[mover] + bodies.spins[mover].cross(r_mover);
    const Vector3 other_velocity = bodies.velocities[other] + bodies.spins[other].cross(r_other);
    const Vector3 relative = mover_velocity - other_velocity;
    const float approach = relative.dot(contact.normal);
    if (approach >= 0.0f) {
        return;  // separating, or already answered
    }
    const Vector3 mover_rn = r_mover.cross(contact.normal);
    const Vector3 other_rn = r_other.cross(contact.normal);
    const float denom = mover_free + other_free
        + mover_rn.dot(item_inertia_xform(mover_basis, mover_transposed, mover_inertia, mover_rn))
        + other_rn.dot(item_inertia_xform(other_basis, other_transposed, other_inertia, other_rn));
    // Bounce only a real impact: below the threshold the motion is absorbed,
    // which is what lets two blocks lie on each other instead of trampolining on
    // gravity's own tickle.
    const float restitution = std::abs(approach) > kRestingApproach ? kBounce : 0.0f;
    const float impulse = -(1.0f + restitution) * approach / std::max(denom, 0.0001f);
    bodies.velocities[mover] += contact.normal * (impulse * mover_free);
    bodies.spins[mover] += item_inertia_xform(mover_basis, mover_transposed, mover_inertia,
                                        r_mover.cross(contact.normal * impulse)) * mover_free;
    bodies.velocities[other] -= contact.normal * (impulse * other_free);
    bodies.spins[other] -= item_inertia_xform(other_basis, other_transposed, other_inertia,
                                        r_other.cross(contact.normal * impulse)) * other_free;

    // Friction, the tangential share of the same contact, through the same
    // effective mass: what stops two stacked blocks sliding across each other.
    const Vector3 tangent = relative - contact.normal * approach;
    if (tangent.length_squared() <= 0.000001f) {
        return;
    }
    const Vector3 t = tangent.normalized();
    const Vector3 mover_rt = r_mover.cross(t);
    const Vector3 other_rt = r_other.cross(t);
    const float t_denom = mover_free + other_free
        + mover_rt.dot(item_inertia_xform(mover_basis, mover_transposed, mover_inertia, mover_rt))
        + other_rt.dot(item_inertia_xform(other_basis, other_transposed, other_inertia, other_rt));
    const float t_impulse = -std::min(tangent.length() * kFriction, kFriction * std::abs(impulse))
        / std::max(t_denom, 0.0001f);
    bodies.velocities[mover] += t * (t_impulse * mover_free);
    bodies.spins[mover] += item_inertia_xform(mover_basis, mover_transposed, mover_inertia,
                                        r_mover.cross(t * t_impulse)) * mover_free;
    bodies.velocities[other] -= t * (t_impulse * other_free);
    bodies.spins[other] -= item_inertia_xform(other_basis, other_transposed, other_inertia,
                                        r_other.cross(t * t_impulse)) * other_free;
}

} // namespace VoxelEngine
