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
                // The whole claim has to be inside the canonical set: a part
                // standing on a face the canonical set says nothing about is not
                // part of a fence run, so it is not in the item model either.
                const uint8_t canon = shape_rule_canonical_faces(part.rule);
                claimed = (part.faces | canon) == canon;
            } else {
                // Every face the part reaches has to be claimed. A part that
                // reaches no face at all cannot be claimed by anything and is
                // unconditional here (the loader logs it).
                for (uint8_t d = 0; d < 6; ++d) {
                    const uint8_t bit = static_cast<uint8_t>(1u << d);
                    if ((part.faces & bit) == 0) continue;
                    if (!shape_rule_connects(part.rule, neighbors(static_cast<ShapeFace>(d)),
                                             registry)) {
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
    return ShapeRule::None;
}

const char* shape_rule_name(ShapeRule rule) noexcept {
    switch (rule) {
        case ShapeRule::Fence: return "fence";
        case ShapeRule::None:  break;
    }
    return "none";
}

uint8_t shape_rule_canonical_faces(ShapeRule rule) noexcept {
    switch (rule) {
        // A fence in the hand is the common fence run, not a lone post and not a
        // four-armed cross: post plus the two arms along X. Chosen because it is
        // what the item is almost always about to become, and because it is the
        // only canonical set that reads as a fence at thumbnail size.
        case ShapeRule::Fence:
            return static_cast<uint8_t>(shape_face_bit(ShapeFace::Right) |
                                        shape_face_bit(ShapeFace::Left));
        case ShapeRule::None:
            break;
    }
    return 0;
}

bool shape_rule_connects(ShapeRule rule, BlockID neighbor, const BlockRegistry& registry) noexcept {
    if (rule == ShapeRule::None) return false;
    if (neighbor == BlockIDs::AIR) return false;

    const BlockType& type = registry.get_block_fast(neighbor);

    switch (rule) {
        case ShapeRule::Fence:
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
        case ShapeRule::None:
            break;
    }
    return false;
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
