#ifndef FARLANDS_SHAPE_RESOLVER_HPP
#define FARLANDS_SHAPE_RESOLVER_HPP

// -----------------------------------------------------------------------------
// Neighbour-dependent shape resolution.
//
// A shape part may be CLAIMED: present only while the cell faces its boxes reach
// have neighbours the part's rule accepts. That is how a fence arms toward
// another fence but not toward open air, and it is resolved AT MESH TIME rather
// than stored per cell, because there is no neighbour-update pass in this engine
// — a cell has no state beyond its block id, so a variant-per-state scheme
// (fence_straight, fence_corner_nw, stair_inner_right, ...) would need one, and
// would have to run again after worldgen, paste and load or the shapes would be
// wrong exactly where nobody looks.
//
// Resolving at mesh time instead means the geometry of a cell is a pure function
// of that cell and its six neighbours, so a chunk meshed at load, at paste, and
// after an edit all agree — which is the property an update pass would have had
// to maintain, and the one tests/test_shape_resolver.cpp pins.
//
// What lives where:
//   - data/block_shapes.json  the boxes, and which RULE claims a part (if any)
//   - this header/.cpp        what a rule means: which neighbours satisfy it,
//                             which faces a canonical resolution assumes, and
//                             the single walk every consumer shares
//   - block_types.cpp         parses parts and derives the static (canonical)
//                             lists from them, so the JSON cannot drift
//
// Consumers, and the neighbour source each one supplies:
//   mesher      ChunkNeighborAccessor (already the mesher's culling source)
//   collision   the chunk map, under the same lock the query already holds
//   raycast     the chunk map under the DDA's lock (expanded by one block)
//   outline     the chunk map, one locked lookup per face
//   icons       resolution with no world at all (canonical)
//   pathfinding classify_block() stays id-only on purpose: it has no position,
//               and counting every fence as a full-height obstacle is the
//               conservative answer for a planner.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace VoxelEngine {

// Which boxes a consumer wants. A part carries visual boxes always, and collision
// boxes only when its collision differs from its shape (the fence is the case:
// the post is 1/4 wide, its collision is 1.5 tall and as wide as the arm's side).
enum class ShapeBoxKind : uint8_t {
    Selection = 0,
    Collision = 1,
};

// The six cell faces, in the mesher's FaceDirection order and with the same
// values (Top, Bottom, Right, Left, Front, Back; Right is +X, Front is +Z).
//
// Re-declared rather than included from mesh/mesh_types.hpp because core/ must
// not depend on the mesh layer — and pinned there by a static_assert, so the two
// enumerations cannot drift apart silently.
enum class ShapeFace : uint8_t {
    Top = 0,
    Bottom = 1,
    Right = 2,
    Left = 3,
    Front = 4,
    Back = 5,
};

[[nodiscard]] constexpr uint8_t shape_face_bit(ShapeFace face) noexcept {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(face));
}

// A neighbour lookup a resolution runs against. Every consumer answers from its
// own safe source: the mesher reads its accessor, collision reads the chunk map
// under the lock the caller already holds for the query box (padded by a block,
// so a +-1 neighbour is always in the locked set), the outline takes one locked
// lookup per face, and the canonical (inventory) resolution uses no world at all.
struct ShapeNeighborFn {
    BlockID (*fn)(void* ctx, ShapeFace face) = nullptr;
    void* ctx = nullptr;

    [[nodiscard]] BlockID operator()(ShapeFace face) const noexcept {
        return fn != nullptr ? fn(ctx, face) : BlockIDs::AIR;
    }
};

// The boxes a part contributes, resolved. Small enough to live on the stack and
// read in a hot loop: the largest shape in the game is the fence at 9 boxes, and
// a capacity of 32 leaves room for the corner-stair sets without an allocation
// in the mesher. A block with no parts BORROWS its static list, so the common
// path costs a pointer assignment rather than a copy.
struct ShapeBoxes {
    static constexpr uint8_t kCapacity = 32;

    const BlockAABB* borrowed = nullptr;  // non-null: the block's own static list
    uint8_t borrowed_count = 0;
    // Deliberately uninitialized: the mesher builds one of these per block, so
    // clearing 768 bytes of scratch would cost more than the resolution. Only the
    // first resolved_count entries are ever read.
    BlockAABB resolved[kCapacity];
    uint8_t resolved_count = 0;
    bool overflowed = false;              // more boxes than kCapacity (see the loader)

    [[nodiscard]] uint8_t count() const noexcept {
        return borrowed != nullptr ? borrowed_count : resolved_count;
    }
    [[nodiscard]] const BlockAABB& operator[](uint8_t i) const noexcept {
        return borrowed != nullptr ? borrowed[i] : resolved[i];
    }
    [[nodiscard]] bool empty() const noexcept { return count() == 0; }
    [[nodiscard]] const BlockAABB* begin() const noexcept {
        return borrowed != nullptr ? borrowed : resolved;
    }
    [[nodiscard]] const BlockAABB* end() const noexcept { return begin() + count(); }
};

// Rule table. A rule name that does not resolve is a load error (logged) and
// leaves the part unconditional, so a typo draws the model rather than deleting it.
[[nodiscard]] ShapeRule shape_rule_from_name(std::string_view name) noexcept;
[[nodiscard]] const char* shape_rule_name(ShapeRule rule) noexcept;

// Which faces a canonical resolution assumes connected. This is what an inventory
// icon, a hotbar model or a tooltip draws with no world to look at, and what the
// loader derives a shape's static box lists from.
[[nodiscard]] uint8_t shape_rule_canonical_faces(ShapeRule rule) noexcept;

// True when a neighbour satisfies the rule. The single place a family's
// connection semantics live, so the mesher, collision and the outline cannot
// disagree about whether a fence has an arm.
[[nodiscard]] bool shape_rule_connects(ShapeRule rule, BlockID neighbor,
                                      const BlockRegistry& registry) noexcept;

// Which cell faces a set of boxes reaches (within 1/16). The loader calls this
// once per part so the claim follows the geometry instead of a second naming
// scheme that could contradict it.
[[nodiscard]] uint8_t shape_box_faces(const std::vector<BlockAABB>& boxes) noexcept;

// Resolve a block's parts against real neighbours. Never allocates. The registry
// is what the rules ask about the neighbours themselves (is this one a fence, a
// liquid, a window), so a caller passes the one it already holds.
void resolve_shape_boxes(const BlockType& block, const BlockRegistry& registry,
                         const ShapeNeighborFn& neighbors, ShapeBoxKind kind,
                         ShapeBoxes& out) noexcept;

// The same walk with every claim satisfied on the rule's canonical faces. Needs
// no world, which is what makes it usable for an inventory icon and for deriving
// a shape's static lists while data/block_shapes.json is still loading.
void resolve_canonical_boxes(const BlockType& block, ShapeBoxKind kind,
                             ShapeBoxes& out) noexcept;

// Cold-path convenience (tools, tests, diagnostics).
[[nodiscard]] std::vector<BlockAABB> shape_boxes_copy(const BlockType& block,
                                                      const BlockRegistry& registry,
                                                      const ShapeNeighborFn& neighbors,
                                                      ShapeBoxKind kind);

} // namespace VoxelEngine

#endif // FARLANDS_SHAPE_RESOLVER_HPP
