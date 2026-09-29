#ifndef FARLANDS_BLOCK_SHAPE_TYPES_HPP
#define FARLANDS_BLOCK_SHAPE_TYPES_HPP
#include <cstdint>
#include <vector>

// The shape model: the boxes a variant is drawn and collided with, and the
// RULES that decide which parts of it are present. What a rule MEANS is
// core/shape_resolver.cpp, which is why only the data lives here. Split out of
// block_types.hpp so the registry file is about the registry.
namespace VoxelEngine {

// -----------------------------------------------------------------------------
// Block Collision AABB
// -----------------------------------------------------------------------------
struct BlockAABB {
    float min[3];
    float max[3];
};

// -----------------------------------------------------------------------------
// Block Shape (shared geometry from block_shapes.json)
//
// A variant is a list of PARTS. Most parts are unconditional; a part carrying a
// rule is present only while the cell faces its boxes reach have neighbours that
// rule accepts (a fence arm toward another fence, rather than toward air). The
// rule DATA lives here; what a rule MEANS is core/shape_resolver.cpp, so a
// family's semantics are in one place instead of spread through the data file.
//
// selection_boxes / collision_boxes stay the flattening of the parts under the
// canonical resolution (see shape_rule_canonical_faces). They are what every
// consumer that has no world to look at uses, and the loader derives them from
// the parts rather than trusting a second hand-written copy.
// -----------------------------------------------------------------------------
// `left` and `right` below are from the point of view of standing on the stair
// facing the way its step points, which is the way a turn reads on screen.
enum class ShapeRule : uint8_t {
    None = 0,              // unconditional part
    Fence = 1,             // arms appear toward another fence or any body-stopping block
    StairStep = 2,         // the step: stands whole unless a stair turns across it
    StairCutLeft = 3,      // the corner the step is cut back to, on the step's left
    StairCutRight = 4,     // ...and on its right
    StairCornerLeft = 5,   // a quarter filling the inside of a turn on the left
    StairCornerRight = 6,  // ...and on the right
    Pane = 7,              // a sheet reaching toward another pane or a whole face
    WallArm = 8,           // a reach meeting a neighbour, stopping short of the cell top
    WallBearing = 9,       // the same reach run to the top, while the cell above spans it
    WallPost = 10,         // the post itself, up while the cell's own layout calls for it
};

// "This block is not a stair at all", for BlockType::stair_step_face.
inline constexpr uint8_t kNoStairFace = 0xFF;

struct ShapePart {
    std::vector<BlockAABB> boxes;            // the part's own geometry
    std::vector<BlockAABB> collision_boxes;  // optional; empty = this part's boxes
    ShapeRule rule = ShapeRule::None;
    // Which cell faces the claim is tested on, in ShapeFace bits. Normally derived
    // from the geometry (shape_box_faces), so a rule cannot be authored against a
    // face its boxes do not meet — but a rule that asks about something else
    // supplies the faces itself (shape_rule_faces_for): the stair rules ask about
    // the step face and a guard side, neither of which is where their boxes are.
    // A file may also declare "faces" explicitly, which sets faces_declared.
    uint8_t faces = 0;
    bool faces_declared = false;
};

struct BlockShape {
    std::vector<BlockAABB> selection_boxes;
    std::vector<BlockAABB> collision_boxes;
    std::vector<ShapePart> parts;
};

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_SHAPE_TYPES_HPP
