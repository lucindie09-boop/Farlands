#include "core/shape_resolver.hpp"

#include <cstddef>

namespace VoxelEngine {
namespace {

// A box has to reach the cell boundary to be around a neighbour's corner. The
// authored numbers are exact 16ths (0.4375, 1.0), so this only absorbs the float
// round-trip through the JSON loader.
constexpr float kFaceTouchEpsilon = 1e-4f;

// How far in from the boundary a wall's reach is answerable for: it stands on the
// half of the cell nearest the side it points at, and then some. The region a reach
// is drawn over, grown out to the full width of the cell across it, is what the cell
// above has to span for that reach to be run to the cell's top.
constexpr float kReachStrip = 0.5625f;  // 9/16

// Whether the cell above spans the strip a reach on `face` occupies.
//
// This is the whole height rule of the wall family, and it is deliberately not a
// question about the neighbour: a wall beside a whole block and a wall beside another
// wall look the SAME, because what a reach buttresses against does not tell you how
// tall it should be. What does is whether there is anything over it. A reach has open
// sky above it and stops 2/16 short of the cell top; put a block, a slab or a step on
// the cell and the reach runs the full height, because the wall is now carrying what
// stands on it and a flank under a span should meet it.
//
// One box of the cell above has to do it, which is not a shortcut: every part that
// spans a whole strip is drawn as one box, and the things that are too thin to carry
// (a rail's post, a sheet, a fence's arm) miss it in the middle rather than the
// corners, so no union of them covers a strip either.
[[nodiscard]] bool above_covers_reach(const ShapeNeighborFn& neighbors,
                                     const BlockRegistry& registry, ShapeFace face) noexcept {
    float x0 = 0.0f;
    float x1 = 1.0f;
    float z0 = 0.0f;
    float z1 = 1.0f;
    switch (face) {
        case ShapeFace::Back:  z1 = kReachStrip; break;
        case ShapeFace::Front: z0 = 1.0f - kReachStrip; break;
        case ShapeFace::Left:  x1 = kReachStrip; break;
        case ShapeFace::Right: x0 = 1.0f - kReachStrip; break;
        default:               return false;
    }

    // Air is a full cube in this engine's cache (it carries no shape of its own, which
    // is exactly what "a whole cell" means), so it is turned away by name before the
    // flag is ever read — the same order every other rule here uses. A liquid is
    // turned away for the other reason: it has no collision to span anything with.
    const BlockID above_id = neighbors(ShapeFace::Top);
    if (above_id == BlockIDs::AIR) return false;
    const BlockType& above = registry.get_block_fast(above_id);
    if (above.is_liquid()) return false;
    // A whole cell spans every strip in it, and it is the one case the boxes cannot
    // answer: a full cube carries no explicit list, because being a whole cell is its
    // shape. Same shortcut the post's coverage test takes, for the same reason.
    if (above.is_full_cube()) return true;
    for (const BlockAABB& box : above.get_collision_boxes()) {
        if (box.min[0] > x0 + kFaceTouchEpsilon) continue;
        if (box.max[0] < x1 - kFaceTouchEpsilon) continue;
        if (box.min[2] > z0 + kFaceTouchEpsilon) continue;
        if (box.max[2] < z1 - kFaceTouchEpsilon) continue;
        return true;
    }
    return false;
}

// Whether a wall reaches this neighbour at all, and it is the same answer whatever
// height the reach ends up drawn at: what a wall meets is a separate question from
// how tall the thing it draws is.
[[nodiscard]] bool wall_reaches(BlockID neighbor, const BlockType& type) noexcept {
    if (neighbor == BlockIDs::AIR) return false;
    // Another wall, whatever it is made of: two materials meet here for the same
    // reason two fence woods do.
    if (type.connector == ShapeRule::WallArm) return true;
    // ...or a sheet, which a wall bites into. This is the family's one asymmetry, and
    // it is the pane rule's mirror image: a sheet presses against a whole face, and a
    // wall's flank is not a face, so a pane does not reach a wall — while a wall's
    // reach is a buttress meeting whatever stands in the cell beside it, and a sheet is
    // something to buttress against.
    if (type.connector == ShapeRule::Pane) return true;
    // ...or a neighbour offering a whole face, which is what a full cube means here.
    // Deliberately not the fence's reach into anything a body cannot walk through: a
    // rail only needs an end to meet, so it lines up with the side of a slab or a snow
    // layer, while a wall's reach is a buttress and there is nothing to buttress
    // against a half-height neighbour. So a slab, a stair, a pole, a fence and a torch
    // are all left alone — and so is water, however full the cell looks.
    if (type.is_liquid()) return false;
    return type.is_full_cube();
}

// The walk both entry points share. `canonical` swaps the per-face neighbour
// question for the rule's canonical face set, which is the only difference
// between "what is there now" and "what a thumbnail should show".
void resolve_impl(const BlockType& block, const BlockRegistry& registry,
                  const ShapeNeighborFn& neighbors, bool canonical, ShapeBoxKind kind,
                  ShapeBoxes& out) noexcept {
    // Only the counters, never the 768-byte box array: the mesher runs this per
    // block, and clearing the scratch would cost more than the resolution.
    out.borrowed = nullptr;
    out.borrowed_count = 0;
    out.resolved_count = 0;
    out.overflowed = false;

    if (block.parts.empty()) {
        const std::vector<BlockAABB>& boxes =
            (kind == ShapeBoxKind::Collision) ? block.get_collision_boxes() : block.selection_boxes;
        out.borrowed = boxes.data();
        out.borrowed_count = static_cast<uint8_t>(boxes.size() < 255 ? boxes.size() : 255);
        return;
    }

    for (const ShapePart& part : block.parts) {
        if (part.rule != ShapeRule::None) {
            bool claimed = true;
            if (shape_rule_self_decided(part.rule)) {
                // A rule that reads the cell as a whole has no face to be claimed on,
                // so it answers for itself. A worldless resolution draws it: the
                // canonical set is the block's fullest reading, and a wall's post is
                // in the item model exactly as it is in the reference's.
                claimed = canonical || shape_rule_present(part.rule, block, neighbors, registry);
            } else if (canonical) {
                // No world to ask, so every face of the claim has to be one the rule
                // says a lone block still has: a fence arm pointing the way the
                // canonical run does not is not in the item model either.
                for (uint8_t d = 0; d < 6; ++d) {
                    const uint8_t bit = static_cast<uint8_t>(1u << d);
                    if ((part.faces & bit) == 0) continue;
                    if (!shape_rule_canonical(part.rule, block, static_cast<ShapeFace>(d))) {
                        claimed = false;
                        break;
                    }
                }
            } else {
                // Every face the part reaches has to be claimed. A part that
                // reaches no face at all cannot be claimed by anything and is
                // unconditional here (the loader logs it).
                for (uint8_t d = 0; d < 6; ++d) {
                    const uint8_t bit = static_cast<uint8_t>(1u << d);
                    if ((part.faces & bit) == 0) continue;
                    if (!shape_rule_connects(part.rule, block, static_cast<ShapeFace>(d),
                                             neighbors, registry)) {
                        claimed = false;
                        break;
                    }
                }
            }
            if (!claimed) continue;
        }

        const std::vector<BlockAABB>& boxes =
            (kind == ShapeBoxKind::Collision && !part.collision_boxes.empty()) ? part.collision_boxes
                                                                               : part.boxes;
        for (const BlockAABB& box : boxes) {
            if (out.resolved_count >= ShapeBoxes::kCapacity) {
                out.overflowed = true;
                return;
            }
            out.resolved[out.resolved_count++] = box;
        }
    }
}

} // namespace

ShapeRule shape_rule_from_name(std::string_view name) noexcept {
    if (name == "fence") return ShapeRule::Fence;
    if (name == "stair_step") return ShapeRule::StairStep;
    if (name == "stair_cut_left") return ShapeRule::StairCutLeft;
    if (name == "stair_cut_right") return ShapeRule::StairCutRight;
    if (name == "stair_corner_left") return ShapeRule::StairCornerLeft;
    if (name == "stair_corner_right") return ShapeRule::StairCornerRight;
    if (name == "pane") return ShapeRule::Pane;
    if (name == "wall_arm") return ShapeRule::WallArm;
    if (name == "wall_bearing") return ShapeRule::WallBearing;
    if (name == "wall_post") return ShapeRule::WallPost;
    return ShapeRule::None;
}

const char* shape_rule_name(ShapeRule rule) noexcept {
    switch (rule) {
        case ShapeRule::Fence:            return "fence";
        case ShapeRule::StairStep:        return "stair_step";
        case ShapeRule::StairCutLeft:     return "stair_cut_left";
        case ShapeRule::StairCutRight:    return "stair_cut_right";
        case ShapeRule::StairCornerLeft:  return "stair_corner_left";
        case ShapeRule::StairCornerRight: return "stair_corner_right";
        case ShapeRule::Pane:             return "pane";
        case ShapeRule::WallArm:          return "wall_arm";
        case ShapeRule::WallBearing:      return "wall_bearing";
        case ShapeRule::WallPost:         return "wall_post";
        case ShapeRule::None:             break;
    }
    return "none";
}

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
        // The whole step is decided by what stands in front of it, so that one face
        // is the entire claim.
        case ShapeRule::StairStep:
            return shape_face_bit(step);
        // A remnant is the half of the step the stair in front is stepping toward,
        // so its claim is that one face. The half being cut away is asked about
        // inside the rule, because what suppresses a cut is a stair standing there
        // rather than the face itself.
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
        // A fence in the hand is the common fence run, not a lone post and not a
        // four-armed cross: post plus the two arms along X. Chosen because it is
        // what the item is almost always about to become, and because it is the
        // only canonical set that reads as a fence at thumbnail size.
        case ShapeRule::Fence:
            return face == ShapeFace::Right || face == ShapeFace::Left;
        // A pane likewise: post plus the two arms along X, which is one flat
        // sheet across the cell and the only canonical set that reads as glass at
        // thumbnail size. It is also the shape that reproduces the old
        // single-plane block exactly, so an inventory icon did not change when the
        // world model grew arms.
        case ShapeRule::Pane:
            return face == ShapeFace::Right || face == ShapeFace::Left;
        // A wall likewise, for the same two reasons: a run along X is what the block
        // is almost always about to become, and it is the only set that reads as a
        // wall at thumbnail size.
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

uint8_t shape_face_from_name(std::string_view name) noexcept {
    if (name == "n") return static_cast<uint8_t>(ShapeFace::Back);
    if (name == "s") return static_cast<uint8_t>(ShapeFace::Front);
    if (name == "e") return static_cast<uint8_t>(ShapeFace::Right);
    if (name == "w") return static_cast<uint8_t>(ShapeFace::Left);
    if (name == "up") return static_cast<uint8_t>(ShapeFace::Top);
    if (name == "down") return static_cast<uint8_t>(ShapeFace::Bottom);
    return 0xFF;
}

uint8_t shape_box_faces(const std::vector<BlockAABB>& boxes) noexcept {
    uint8_t mask = 0;
    for (const BlockAABB& box : boxes) {
        if (box.min[1] <= kFaceTouchEpsilon) mask |= shape_face_bit(ShapeFace::Bottom);
        if (box.max[1] >= 1.0f - kFaceTouchEpsilon) mask |= shape_face_bit(ShapeFace::Top);
        if (box.min[0] <= kFaceTouchEpsilon) mask |= shape_face_bit(ShapeFace::Left);
        if (box.max[0] >= 1.0f - kFaceTouchEpsilon) mask |= shape_face_bit(ShapeFace::Right);
        if (box.min[2] <= kFaceTouchEpsilon) mask |= shape_face_bit(ShapeFace::Back);
        if (box.max[2] >= 1.0f - kFaceTouchEpsilon) mask |= shape_face_bit(ShapeFace::Front);
    }
    return mask;
}

void resolve_shape_boxes(const BlockType& block, const BlockRegistry& registry,
                         const ShapeNeighborFn& neighbors, ShapeBoxKind kind,
                         ShapeBoxes& out) noexcept {
    resolve_impl(block, registry, neighbors, /*canonical=*/false, kind, out);
}

void resolve_canonical_boxes(const BlockType& block, ShapeBoxKind kind, ShapeBoxes& out) noexcept {
    // The registry is reached only through shape_rule_connects, which the
    // canonical branch never calls — so this is safe while the registry is still
    // being built, which is exactly when the loader needs it.
    resolve_impl(block, BlockRegistry::get_instance(), ShapeNeighborFn{}, /*canonical=*/true, kind,
                 out);
}

std::vector<BlockAABB> shape_boxes_copy(const BlockType& block, const BlockRegistry& registry,
                                        const ShapeNeighborFn& neighbors, ShapeBoxKind kind) {
    ShapeBoxes boxes;
    resolve_shape_boxes(block, registry, neighbors, kind, boxes);
    return std::vector<BlockAABB>(boxes.begin(), boxes.end());
}

} // namespace VoxelEngine
