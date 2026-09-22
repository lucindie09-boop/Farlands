#include "core/shape_resolver.hpp"

#include <cstddef>

namespace VoxelEngine {
namespace {

// A box has to reach the cell boundary to be around a neighbour's corner. The
// authored numbers are exact 16ths (0.4375, 1.0), so this only absorbs the float
// round-trip through the JSON loader.
constexpr float kFaceTouchEpsilon = 1e-4f;

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
            if (canonical) {
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
        case ShapeRule::None:             break;
    }
    return "none";
}

// The face a block's step points at. False for anything that is not an upright
// stair — a hanging stair, a slab, terrain — because none of those turn a corner
// or cut one.
[[nodiscard]] bool step_face_of(const BlockType& type, ShapeFace& out) noexcept {
    if (type.stair_step_face == kNoStairFace) return false;
    const auto face = static_cast<ShapeFace>(type.stair_step_face);
    if (shape_side_ring_index(face) < 0) return false;
    out = face;
    return true;
}

// A stair turned ACROSS this one's step: its step points at one of this cell's
// sides rather than along the step. That is the stair a step gets cut back
// against, and the reason a step is not simply always whole.
[[nodiscard]] bool stairs_cross(ShapeFace step, ShapeFace other) noexcept {
    return other != step && other != shape_opposite_face(step);
}

// A stair standing on the SAME step as this one: same way up, step pointing the
// way ours does. Two of those side by side are one flight, and the second one owns
// the geometry the first would otherwise grow — which is why both the cut and the
// corner rules ask about it before drawing anything.
[[nodiscard]] bool same_step_stair(ShapeFace step, BlockID neighbor,
                                   const BlockRegistry& registry) noexcept {
    ShapeFace other = ShapeFace::Top;
    if (!step_face_of(registry.get_block_fast(neighbor), other)) return false;
    return other == step;
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
[[nodiscard]] StairCut cut_of(ShapeFace step, const ShapeNeighborFn& neighbors,
                              const BlockRegistry& registry) noexcept {
    ShapeFace front = ShapeFace::Top;
    if (!step_face_of(registry.get_block_fast(neighbors(step)), front)) return StairCut::None;
    if (!stairs_cross(step, front)) return StairCut::None;

    if (front == shape_step_left_of(step)) {
        return same_step_stair(step, neighbors(shape_step_right_of(step)), registry)
                   ? StairCut::None
                   : StairCut::Left;
    }
    return same_step_stair(step, neighbors(shape_step_left_of(step)), registry) ? StairCut::None
                                                                               : StairCut::Right;
}

uint8_t shape_rule_faces_for(ShapeRule rule, const BlockType& self) noexcept {
    ShapeFace step = ShapeFace::Top;
    if (!step_face_of(self, step)) return 0;  // not an upright stair: nothing to read

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
        // Fence arms read their claim off their own boxes, so the loader derives it.
        case ShapeRule::Fence:
        case ShapeRule::None:
            break;
    }
    return 0;
}

bool shape_rule_is_connector(ShapeRule rule) noexcept { return rule == ShapeRule::Fence; }

bool shape_rule_canonical(ShapeRule rule, const BlockType& self, ShapeFace face) noexcept {
    (void)self;
    switch (rule) {
        // A fence in the hand is the common fence run, not a lone post and not a
        // four-armed cross: post plus the two arms along X. Chosen because it is
        // what the item is almost always about to become, and because it is the
        // only canonical set that reads as a fence at thumbnail size.
        case ShapeRule::Fence:
            return face == ShapeFace::Right || face == ShapeFace::Left;
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
        case ShapeRule::StairStep:
        case ShapeRule::StairCutLeft:
        case ShapeRule::StairCutRight:
        case ShapeRule::StairCornerLeft:
        case ShapeRule::StairCornerRight: {
            // All five read the same two things: which way this stair's own step
            // points, and what is standing in front of it.
            ShapeFace step = ShapeFace::Top;
            if (!step_face_of(self, step)) return false;
            const StairCut cut = cut_of(step, neighbors, registry);

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
                        ShapeFace other = ShapeFace::Top;
                        return step_face_of(type, other) && other == side;
                    }
                    // 2. Nothing may cut the step: a stair turned across it cuts the
                    //    step back instead, and the cut is what the cell gets, so a
                    //    cut step carries no corner.
                    if (face == step) return cut == StairCut::None;
                    // 3. The quarter's own side: no stair already on this one's step
                    //    there, because that stair reaches into the very region the
                    //    quarter would fill.
                    if (face == side) return !same_step_stair(step, neighbor, registry);
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
