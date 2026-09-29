// The connector rules themselves: what a wall, a stair, a fence, a pane and a
// pole do about the cell beside and the cell above, plus the stair geometry they
// are written in terms of. The walk that drives them is in core/shape_resolver.cpp,
// and the two neighbour questions it shares with this file are declared in
// core/shape_resolver_internal.hpp.

#include "core/shape_resolver.hpp"
#include "core/shape_resolver_internal.hpp"

namespace VoxelEngine {

// Which way a block's step points, and which way up the block is. False for
// anything that is not a stair at all — a slab, terrain, a fence.
[[nodiscard]] bool step_face_of(const BlockType& type, ShapeFace& out, bool& hanging) noexcept {
    if (type.stair_step_face == kNoStairFace) return false;
    const auto face = static_cast<ShapeFace>(type.stair_step_face);
    if (shape_side_ring_index(face) < 0) return false;
    out = face;
    hanging = type.stair_hanging;
    return true;
}

// A stair turned ACROSS this one's step: its step points at one of this cell's
// sides rather than along the step. That is the stair a step gets cut back
// against, and the reason a step is not simply always whole.
[[nodiscard]] bool stairs_cross(ShapeFace step, ShapeFace other) noexcept {
    return other != step && other != shape_opposite_face(step);
}

// A stair that is the same shape as this one: its step points the way ours does AND
// it is the same way up. Both halves of that matter. Two of these side by side are
// one flight, and the second one owns the geometry the first would otherwise grow —
// which is why both the cut and the corner rules ask about it before drawing
// anything.
//
// The way up is part of the question because the two families are MIRRORED about
// the floor: a stair that climbs and a stair that hangs meet along a side with a
// step-shaped gap between them, and a quarter or a remnant drawn there would be a
// box floating in that gap. So a hanging stair turns only with a hanging one and an
// upright only with an upright — which is also what leaves an upright stair's
// geometry exactly what it was before the hanging ones had rules of their own.
[[nodiscard]] bool same_step_stair(ShapeFace step, bool hanging, BlockID neighbor,
                                   const BlockRegistry& registry) noexcept {
    ShapeFace other = ShapeFace::Top;
    bool other_hanging = false;
    if (!step_face_of(registry.get_block_fast(neighbor), other, other_hanging)) return false;
    return other == step && other_hanging == hanging;
}

// Which half of the step survives a stair turned across it, if any.
enum class StairCut : uint8_t { None = 0, Left = 1, Right = 2 };

// Whether the step is cut back, and to which half.
//
// A stair in FRONT of the step that turns across it meets this cell along one of
// the step's sides, so the half it is stepping toward is the half the two steps run
// into each other on — and that half survives. The exception is what keeps a wide
// flight from developing a notch: when the half that would be cut away already
// belongs to a flight on this one's step (same way up, same way round), the two
// runs meet sideways and the step between them stays whole.
[[nodiscard]] StairCut cut_of(ShapeFace step, bool hanging, const ShapeNeighborFn& neighbors,
                              const BlockRegistry& registry) noexcept {
    ShapeFace front = ShapeFace::Top;
    bool front_hanging = false;
    if (!step_face_of(registry.get_block_fast(neighbors(step)), front, front_hanging)) {
        return StairCut::None;
    }
    // A stair the other way up in front of the step is not a stair the step runs
    // into: the two are mirrored about the floor, so what meets this cell along a
    // side is not a second half of the same shape.
    if (front_hanging != hanging) return StairCut::None;
    if (!stairs_cross(step, front)) return StairCut::None;

    if (front == shape_step_left_of(step)) {
        return same_step_stair(step, hanging, neighbors(shape_step_right_of(step)), registry)
                   ? StairCut::None
                   : StairCut::Left;
    }
    return same_step_stair(step, hanging, neighbors(shape_step_left_of(step)), registry)
               ? StairCut::None
               : StairCut::Right;
}

uint8_t shape_rule_faces_for(ShapeRule rule, const BlockType& self) noexcept {
    ShapeFace step = ShapeFace::Top;
    // The way up is not part of the answer here: a hanging stair asks the same two
    // questions about the same horizontal faces, and only the geometry it grows
    // differs, which the file's own boxes carry.
    bool hanging = false;
    if (!step_face_of(self, step, hanging)) return 0;  // not a stair: nothing to read

    switch (rule) {
        // The whole step, and both remnants, are decided by what stands in front of
        // them, so that one face is the entire claim in all three - written as one
        // case because a claim is a value and three identical ones are one claim.
        // The half of a remnant that is cut away is asked about inside the rule
        // instead, because what suppresses a cut is a stair standing there rather
        // than the face itself.
        case ShapeRule::StairStep:
        case ShapeRule::StairCutLeft:
        case ShapeRule::StairCutRight:
            return shape_face_bit(step);
        // A corner is read on three faces: the stair it turns with is BEHIND the
        // step and pointing at the quarter's side, nothing may be standing across
        // the step (a step cut back carries no corner), and the side the quarter
        // sits on must have no stair already on this one's step.
        case ShapeRule::StairCornerLeft:
            return static_cast<uint8_t>(shape_face_bit(shape_opposite_face(step)) |
                                        shape_face_bit(step) |
                                        shape_face_bit(shape_step_left_of(step)));
        case ShapeRule::StairCornerRight:
            return static_cast<uint8_t>(shape_face_bit(shape_opposite_face(step)) |
                                        shape_face_bit(step) |
                                        shape_face_bit(shape_step_right_of(step)));
        // Arms and sheets both read their claim off their own boxes, so the loader
        // derives it. A pane's is one direction per part, which is the whole reason
        // it can be a single block id where the reference needs a per-axis pair.
        case ShapeRule::Pane:
        case ShapeRule::WallArm:
        case ShapeRule::WallBearing:
        case ShapeRule::WallPost:  // reads no face: its part claims none (see below)
        case ShapeRule::Fence:
        case ShapeRule::None:
            break;
    }
    return 0;
}

bool shape_rule_is_connector(ShapeRule rule) noexcept {
    return rule == ShapeRule::Fence || rule == ShapeRule::Pane || rule == ShapeRule::WallArm;
}

bool shape_rule_self_decided(ShapeRule rule) noexcept {
    return rule == ShapeRule::WallPost;
}

bool shape_rule_canonical(ShapeRule rule, const BlockType& self, ShapeFace face) noexcept {
    (void)self;
    switch (rule) {
        // A fence, a pane and a wall are each held in the hand as a run along X -
        // post plus the two arms, or one flat sheet - rather than as a lone post or
        // as a four-armed cross, and for the same reasons in all three cases: it is
        // what the block is almost always about to become, and it is the only
        // canonical set that reads as that block at thumbnail size. For the pane it
        // is also the shape that reproduces the old single-plane block exactly, so
        // an inventory icon did not change when the world model grew arms. One
        // case, because the answer is one answer.
        case ShapeRule::Fence:
        case ShapeRule::Pane:
        case ShapeRule::WallArm:
            return face == ShapeFace::Right || face == ShapeFace::Left;
        // ...and the post is always in the icon, because at that size a wall without
        // its post looks like a wall standing in a hole — and because a wall held in
        // the hand is post plus a run through it, which is exactly the canonical set
        // this derives. Neither of the reaches is: both mean "something is beside me",
        // which a worldless resolution cannot know, and the icon draws the short one
        // through the post rather than the tall one, because nothing is over the cell
        // to carry it. (The walk does not ask this for the post — it answers for itself
        // — but the answer here is the same one, so the two cannot disagree.)
        case ShapeRule::WallPost:
            return true;
        case ShapeRule::WallBearing:
            return false;
        // The step is part of a lone stair, so it is what the icon, the hotbar cell
        // and the held viewmodel draw.
        case ShapeRule::StairStep:
            return true;
        // The remnants and the corners all mean "a stair is beside me", which a
        // worldless resolution cannot know, and a corner box on a lone stair would
        // be a box sticking out of nothing.
        case ShapeRule::StairCutLeft:
        case ShapeRule::StairCutRight:
        case ShapeRule::StairCornerLeft:
        case ShapeRule::StairCornerRight:
        case ShapeRule::None:
            return false;
    }
    return false;
}

bool shape_rule_connects(ShapeRule rule, const BlockType& self, ShapeFace face,
                         const ShapeNeighborFn& neighbors,
                         const BlockRegistry& registry) noexcept {
    if (rule == ShapeRule::None) return false;

    const BlockID neighbor = neighbors(face);
    const BlockType& type = registry.get_block_fast(neighbor);

    switch (rule) {
        case ShapeRule::Fence:
            if (neighbor == BlockIDs::AIR) return false;
            // A liquid is not something a rail can be attached to, and a
            // transparent neighbour is a window: arming into either would put a
            // rail in mid-air.
            if (type.is_liquid()) return false;
            if (HasProperty(type.properties, BlockProperty::Transparent)) return false;
            // Any wood: fences connect across materials, which is why this is one
            // rule and not four.
            if (type.connector == ShapeRule::Fence) return true;
            // Terrain, stone, planks, a wall, a slab, a stair — anything a body
            // cannot walk through. Deliberately not is_full_cube(): a fence lines
            // up with the side of a slab or a snow layer too, and the rail's own
            // height is fixed, so the neighbour's height does not matter.
            return type.stops_bodies();
        case ShapeRule::Pane:
            // A sheet reaches a face only toward something it can seal against, and
            // there are exactly two of those. Another pane, for the same reason a
            // rail reaches a rail: two sheets meeting on a shared plane are one
            // sheet, so a run and a corner are compositions rather than variants.
            if (type.connector == ShapeRule::Pane) return true;
            // ...or a block offering a WHOLE face to press against, which is what a
            // full cube means here. Deliberately not the fence's test: a rail is a
            // bar that only needs an end to meet, so it reaches anything a body
            // cannot walk through, while a sheet with no face behind it is glued to
            // nothing. So a pane sits flush against stone, planks, glass and terrain,
            // and stays short of the slabs, poles, stairs and fences it has no face
            // to meet.
            //
            // A window passes even though the fence rule turns it away, and it is
            // the clearest statement of the difference: a window is precisely what a
            // pane is for. There is no seam to hide either, because both sheets are
            // drawn. A liquid fails it however full the cell looks, so a pane never
            // reaches out over water.
            if (neighbor == BlockIDs::AIR || type.is_liquid()) return false;
            return type.is_full_cube();
        // The wall family's two hands, which are the SAME reach drawn at two heights.
        // They ask the same first question — does this side reach at all — and then
        // split on the cell above, so a side that reaches draws exactly one of them and
        // the pair can never both appear or both vanish.
        case ShapeRule::WallArm:
        case ShapeRule::WallBearing: {
            if (!wall_reaches(neighbor, type)) return false;
            const bool carried = above_covers_reach(neighbors, registry, face);
            return rule == ShapeRule::WallBearing ? carried : !carried;
        }
        case ShapeRule::WallPost:
            // Never reached: the post claims no cell face, so the walk resolves it
            // through shape_rule_present rather than one face at a time. What the
            // post depends on is not one neighbour — it is the four lateral ones at
            // once, and whether a fifth cell on top of them rests on it.
            (void)face;
            return false;
        case ShapeRule::StairStep:
        case ShapeRule::StairCutLeft:
        case ShapeRule::StairCutRight:
        case ShapeRule::StairCornerLeft:
        case ShapeRule::StairCornerRight: {
            // All five read the same three things: which way this stair's own step
            // points, which way up it is, and what is standing in front of it.
            ShapeFace step = ShapeFace::Top;
            bool hanging = false;
            if (!step_face_of(self, step, hanging)) return false;
            const StairCut cut = cut_of(step, hanging, neighbors, registry);

            switch (rule) {
                case ShapeRule::StairStep:
                    // The step is the raised half in full until a stair turns across
                    // it. Every other neighbour — air, stone, a fence, a stair
                    // pointing the same way or the opposite way — leaves it alone,
                    // because only a stair turned across it meets this cell along a
                    // side and so leaves the half below unsupported. When one does
                    // cut it the remnants take over, so the step and a remnant are
                    // never both drawn; that mutual exclusion is also what keeps a
                    // stair from ever resolving into a full cell.
                    return face == step && cut == StairCut::None;
                case ShapeRule::StairCutLeft:
                case ShapeRule::StairCutRight: {
                    const bool want_left = rule == ShapeRule::StairCutLeft;
                    if (face != step) return false;
                    return cut == (want_left ? StairCut::Left : StairCut::Right);
                }
                case ShapeRule::StairCornerLeft:
                case ShapeRule::StairCornerRight: {
                    const ShapeFace side = (rule == ShapeRule::StairCornerLeft)
                                               ? shape_step_left_of(step)
                                               : shape_step_right_of(step);
                    // 1. The stair this corner turns with is BEHIND the step, and
                    //    its own step has to point back into this cell at `side`.
                    //    That is why the quarter sits in the half opposite the step:
                    //    it is the region the two of them leave open between them.
                    if (face == shape_opposite_face(step)) {
                        // ...and it has to be the same way up, for the same reason
                        // the cut does: the region the two of them leave open
                        // between them only exists when both are laid out the same
                        // way round, and a stair behind a hanging one points into a
                        // gap that is not there.
                        ShapeFace other = ShapeFace::Top;
                        bool other_hanging = false;
                        return step_face_of(type, other, other_hanging) && other == side &&
                               other_hanging == hanging;
                    }
                    // 2. Nothing may cut the step: a stair turned across it cuts the
                    //    step back instead, and the cut is what the cell gets, so a
                    //    cut step carries no corner.
                    if (face == step) return cut == StairCut::None;
                    // 3. The quarter's own side: no stair already on this one's step
                    //    there, because that stair reaches into the very region the
                    //    quarter would fill.
                    if (face == side) return !same_step_stair(step, hanging, neighbor, registry);
                    return false;
                }
                default:
                    return false;
            }
        }
        case ShapeRule::None:
            break;
    }
    return false;
}

namespace {

// The 2/16 column a wall's post occupies, and the epsilon the coverage test below
// works to — the same one the box reader uses, so "reaches the top" means the same
// thing here as it does everywhere else.
constexpr float kPostColumnMin = 0.4375f;  // 7/16
constexpr float kPostColumnMax = 0.5625f;  // 9/16
constexpr float kPostEpsilon = 1e-4f;

// Whether the block above stands ON the post's footprint all the way up: its boxes
// have to span the 2/16 column and reach the top of its own cell. That is what
// "something rests on the post" means, and it is why a slab over a wall does not
// raise one (half a cell tall) while a plank, a window, a fence's post or another
// post does.
//
// The collision boxes rather than the visual ones, deliberately: the question is
// whether the thing above is carried by the post, and a fence's rail-shape reaches
// across the cell without being anything to carry. Growth, not a single box test,
// because a post may legitimately be modelled in more than one box stacked.
[[nodiscard]] bool covers_post_column(const BlockType& above) noexcept {
    if (above.is_full_cube()) return true;
    const std::vector<BlockAABB>& boxes = above.get_collision_boxes();
    float reach = 0.0f;
    for (bool grew = true; grew;) {
        grew = false;
        for (const BlockAABB& box : boxes) {
            if (box.min[0] > kPostColumnMin + kPostEpsilon) continue;
            if (box.max[0] < kPostColumnMax - kPostEpsilon) continue;
            if (box.min[2] > kPostColumnMin + kPostEpsilon) continue;
            if (box.max[2] < kPostColumnMax - kPostEpsilon) continue;
            if (box.min[1] > reach + kPostEpsilon) continue;
            if (box.max[1] <= reach) continue;
            reach = box.max[1];
            grew = true;
        }
    }
    return reach >= 1.0f - kPostEpsilon;
}

} // namespace

bool shape_rule_present(ShapeRule rule, const BlockType& self, const ShapeNeighborFn& neighbors,
                        const BlockRegistry& registry) noexcept {
    if (rule != ShapeRule::WallPost) return true;

    // The four sides, opposite pairs adjacent: Back/-Z against Front/+Z, and
    // Right/+X against Left/-X.
    constexpr ShapeFace kSides[4] = {ShapeFace::Back, ShapeFace::Right, ShapeFace::Front,
                                     ShapeFace::Left};
    bool reaches[4];
    bool bearing[4];
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        bearing[i] = shape_rule_connects(ShapeRule::WallBearing, self, kSides[i], neighbors, registry);
        // A side counts when either of the wall's two hands reaches it — and since the
        // two hands are one reach drawn at two heights, that is just "the side connects".
        reaches[i] = bearing[i] ||
                     shape_rule_connects(ShapeRule::WallArm, self, kSides[i], neighbors, registry);
        any = any || reaches[i];
    }

    // A plain through-run is the layout with no post, and it is the whole reason the
    // post is conditional: a run is a rail, and a rail studded with posts at every
    // cell no longer reads as the wall it is. Everything else carries one — a wall on
    // its own, the end of a run, a corner, a T junction — because those are the cells
    // where a column reads as the thing holding the run up. A four-way cross is the
    // one other layout without a post: four arms already meet in the middle.
    const bool through_run = any && reaches[0] == reaches[2] && reaches[1] == reaches[3];
    if (!through_run) return true;

    // A run whose reaches on both sides of an axis already run to the cell top is a
    // column already, so it takes no post however much is piled on it — and that is
    // exactly the run under a whole block, where the two reaches meet the span above
    // and a post between them would only be a thicker middle.
    if ((bearing[0] && bearing[2]) || (bearing[1] && bearing[3])) return false;

    // Otherwise the post is up exactly while something rests on its footprint.
    const BlockID above = neighbors(ShapeFace::Top);
    if (above == BlockIDs::AIR) return false;
    const BlockType& above_type = registry.get_block_fast(above);
    if (above_type.is_liquid()) return false;
    // ...and a wall above is inert, which is the one place this departs from the
    // reference on purpose. There the post below comes up when the wall above stands
    // WITH a post of its own — a question about that cell's neighbours, which no rule
    // can ask from here without reading two cells out. Leaving it out is the quieter
    // half of the difference: a wall two high stays the uniform rail one high is,
    // where asking only "is there a wall above" would give every stacked run a solid
    // base and a thin top.
    if (above_type.connector == ShapeRule::WallArm) return false;
    return covers_post_column(above_type);
}

} // namespace VoxelEngine
