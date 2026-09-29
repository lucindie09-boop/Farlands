// The resolver's public surface and the walk behind it: the cross-section constants
// the neighbour questions read, the two questions themselves (declared for the rules
// half in core/shape_resolver_internal.hpp), the walk both entry points share, the
// rule-name table, and the small box utilities. Every rule the walk asks about lives
// in core/shape_resolver_rules.cpp.

#include "core/shape_resolver.hpp"
#include "core/shape_resolver_internal.hpp"

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
} // namespace

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

namespace {

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
