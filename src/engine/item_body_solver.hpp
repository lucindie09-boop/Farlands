#ifndef FARLANDS_ITEM_BODY_SOLVER_HPP
#define FARLANDS_ITEM_BODY_SOLVER_HPP

#include "engine/item_body_state.hpp"
#include "engine/item_pair_solver.hpp"

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

// A dropped item's whole substep, natively: everything scripts/dropped_items.gd
// used to do per body per substep, in one pass over every body of the pile.
//
// The world half is the collision resolver's answer -- which of the body's own
// points are inside solid geometry, and how deep the body's turned boxes are --
// and the impulse solve that answers it: the body turns because the contact's
// torque comes from WHERE it touched, and it comes to rest because its contacts
// actually stopped it. The pair half is ItemPairSolver (bodies against each
// other). Between them they are the entire physics of a dropped item; what is
// left in the script is where the item is drawn and when it is picked up.
//
// It was GDScript because the physics is small and the shapes are odd, and it
// stayed GDScript until a pile of blocks made the cost visible: a per-body
// substep was two native queries wrapped in a dictionary-heavy impulse loop, and
// the loop was the expensive part -- for a short time. The maths is unchanged;
// see the GDScript in the git history for the text that explained it.
namespace VoxelEngine {

class CollisionResolver;

class ItemBodySolver {
public:
    explicit ItemBodySolver(const CollisionResolver* resolver) : resolver_(resolver) {}

    // One substep of every body given: gravity and damping, the turn and the
    // move, the world's contacts and its exact guard, the impulses, the push-out,
    // then the pair solve, then the ONE rest decision per body -- where a body
    // is because of BOTH halves, so it is made once and not twice.
    //
    // A sleeping body is not integrated and not asked about: it is an obstacle
    // to whatever moves, until something moving wakes it.
    void step(const ItemShapeTable& shapes, ItemBodies& bodies, float delta);

private:
    // One body's world half, from its integration to its settle.
    void step_body(const ItemShapeTable& shapes, ItemBodies& bodies, int32_t index, float delta);

    // Slow enough, and barely turning: half of what coming to rest takes -- the
    // other half is something holding the body up, which each half of the step
    // answers for the bodies it holds (see step).
    bool at_rest(const ItemBodies& bodies, int32_t index) const;

    const CollisionResolver* resolver_ = nullptr;
    ItemPairSolver pairs_;

    // Per-body scratch, reused across bodies and substeps so a pile allocates
    // nothing a body at a time. `supported_` is the world half's answer to "is
    // this body touching something", kept until the pair half has answered too.
    std::vector<uint8_t> supported_;
    std::vector<godot::Vector3> points_;
    std::vector<godot::Vector3> box_offsets_;
    std::vector<godot::Vector3> box_halves_;
};

} // namespace VoxelEngine

#endif // FARLANDS_ITEM_BODY_SOLVER_HPP
