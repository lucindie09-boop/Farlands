#ifndef FARLANDS_BLOCK_TYPES_HPP
#define FARLANDS_BLOCK_TYPES_HPP
#include <cstdint>
#include <array>
#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

namespace godot { class String; }

namespace VoxelEngine {

using BlockID = uint16_t;

// -----------------------------------------------------------------------------
// Block Properties
// -----------------------------------------------------------------------------
enum class BlockProperty : uint8_t {
    None           = 0,
    Solid          = 1 << 0,
    Transparent    = 1 << 1,
    Opaque         = 1 << 2,
    Liquid         = 1 << 3,
    RenderAllFaces = 1 << 4,
    NoOcclusion    = 1 << 5,
    Emissive       = 1 << 6
};

constexpr inline BlockProperty operator|(BlockProperty a, BlockProperty b) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult) - Valid bitfield operation on uint8_t enum
    return static_cast<BlockProperty>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr inline BlockProperty operator&(BlockProperty a, BlockProperty b) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult) - Valid bitfield operation on uint8_t enum
    return static_cast<BlockProperty>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr inline bool HasProperty(BlockProperty flags, BlockProperty prop) noexcept {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(prop)) != 0;
}

enum class LightEmissionPattern : uint8_t {
    Diamond = 0   // 6-connected falloff (classic Minecraft-style)
};

// -----------------------------------------------------------------------------
// Fluid state (the storage half; the behaviour is in src/fluids/)
// -----------------------------------------------------------------------------
// Which fluid a block IS, when it is one (block_definitions.json "fluid"). A
// block that says it is water at depth 2 is water that has flowed two steps
// from a source. This lives in core because it is part of a block definition,
// exactly like `drops` is; the rules that read it are src/fluids/fluid_rules.*
// and they never see a block id at all — they resolve states by name through
// the registry (see fluids/fluid_state_table.hpp).
enum class FluidKind : uint8_t {
    None = 0,   // not a fluid
    Water = 1,
    Lava = 2,   // declared but with no traits yet, so it does not flow
    Acid = 3
};

[[nodiscard]] const char* fluid_kind_name(FluidKind kind) noexcept;
[[nodiscard]] FluidKind fluid_kind_from_name(const char* name) noexcept;

// -----------------------------------------------------------------------------
// Block Collision AABB
// -----------------------------------------------------------------------------
struct BlockAABB {
    float min[3];
    float max[3];
};

// -----------------------------------------------------------------------------
// Block Shape (shared geometry from block_shapes.json)
// -----------------------------------------------------------------------------
struct BlockShape {
    std::vector<BlockAABB> selection_boxes;
    std::vector<BlockAABB> collision_boxes;
};

// -----------------------------------------------------------------------------
// Block Type Definition
// -----------------------------------------------------------------------------
struct BlockType {
    BlockID id = 0;
    const char* name = nullptr;
    BlockProperty properties = BlockProperty::None;
    std::array<bool, 6> visible_faces{};   // +X, -X, +Y, -Y, +Z, -Z
    std::array<int, 6> texture_indices{};    // +X, -X, +Y, -Y, +Z, -Z (resolved layer index)
    uint8_t light_level = 0;
    uint8_t light_r = 0;
    uint8_t light_g = 0;
    uint8_t light_b = 0;
    LightEmissionPattern light_pattern = LightEmissionPattern::Diamond;

    // Mesh behaviour: blocks visually lower than full height (water, mud, etc.)
    float top_face_offset = 0.0f;
    // If false, side faces are rendered even against same-type neighbours.
    bool cull_against_same = true;
    // Vanilla slipperiness (0.6 = stone, 0.6 = dirt, 0.98 = ice, 0.98 = packed_ice, etc.)
    float slipperiness = 0.6f;

    // Break time while holding LMB, in seconds (0.5 = dirt, 1.5 = stone).
    // -1.0 = unbreakable (bedrock, water) — never cracks or breaks.
    float hardness = 1.0f;

    // Tool class this block is mined fastest with ("pickaxe", "axe", "shovel",
    // ...). Empty = no preference, bare-hand speed. Parsed from
    // block_definitions.json.
    std::string preferred_tool;
    // Minimum tool tier needed to actually receive the preferred-tool speed
    // bonus (0 = any tier). A matching-class tool below this tier mines at
    // bare-hand speed instead.
    int32_t min_tier = 0;

    // Block a HAMMER leaves behind instead of this one (block_definitions.json
    // "crush_result"; 0 = AIR = not crushable). This one field is the hammer's
    // whole contract: a block that names a crush_result is mined faster by a
    // hammer (see mining.hpp) and drops that block in its place, so cobblestone
    // becomes gravel and gravel becomes sand.
    BlockID crush_result = 0;

    // Block this one yields when broken, whatever the tool (block_definitions.json
    // "drops"; 0 = AIR = drop this block itself). Deliberately separate from
    // crush_result: `drops` is what ANY break yields (stone -> cobblestone),
    // while crush_result is what only a hammer makes of it. Like crush_result
    // it is resolved from its NAME after every block is registered, because the
    // target is normally declared later in the file than the block naming it.
    BlockID drops = 0;

    // Texture filename per face (populated by load_from_json, used by TextureArrayGenerator).
    // Placed last so existing aggregate initializers are unaffected.
    std::array<std::string, 6> texture_names{};

    // Emissive texture filename per face (empty = no emissive contribution).
    std::array<std::string, 6> emissive_texture_names{};
    std::array<int, 6> emissive_texture_indices{};  // resolved by TextureArrayGenerator

    // Collision / selection shape: one or more AABBs in block-local [0,1] space.
    // Empty vector = full cube (default). Used by collision, raycast, outline, and mesh builder.
    std::vector<BlockAABB> selection_boxes{};

    // Optional override for collision only. When non-empty, collision queries use this
    // instead of selection_boxes. Raycast, mesh, and outline still use selection_boxes.
    std::vector<BlockAABB> collision_boxes{};

    // Cached flag: true when selection_boxes is a single full cube [0,0,0,1,1,1].
    // Checked on hot paths (greedy meshing, collision, AO) for zero-overhead fast path.
    bool full_cube_ = true;

    // Fluid state (block_definitions.json "fluid": {"kind": "water", "depth":
    // 0, "falling": false}). `fluid_kind == FluidKind::None` for anything that
    // is not a fluid. depth 0 means full strength (a source, or a falling
    // cell); 1..7 is runoff that many steps from a source.
    FluidKind fluid_kind = FluidKind::None;
    uint8_t fluid_depth = 0;
    bool fluid_falling = false;

    // True when this block is a fluid at all.
    [[nodiscard]] bool is_fluid_state() const noexcept { return fluid_kind != FluidKind::None; }

    // True when fluid cannot occupy this cell or pass through it. Air, liquids
    // and anything without collision let fluid through; a solid fills at least
    // part of the cell, so fluid goes around it and sits on top rather than
    // inside. This is the single answer the flow rules ask about a cell
    // (FluidWorld::blocked), so the flow rules and the collider cannot disagree
    // about what is in the way.
    //
    // Deliberately NOT `is_full_cube()` on its own, and deliberately not a plain
    // "is it air" test either: in the built-in default registry (what the tests
    // and headless tools run on) `full_cube_` is TRUE for air, because it is only
    // recomputed from real shapes once block_definitions.json loads. Classifying
    // on that flag alone makes air solid, so nothing ever flows — the same trap
    // pathfinding/block_class.hpp exists to avoid. `Solid` is the property that
    // actually separates air and fluids from everything else.
    [[nodiscard]] bool blocks_fluid() const noexcept {
        // A liquid the simulation owns lets flow through and into it (its own
        // cells are read with FluidWorld::fluid_at instead) ...
        if (is_fluid_state()) return false;
        // ...but a liquid it does not own is a mass it may not displace. That is
        // generated ocean water: not a fluid state (see block_types.cpp), "liquid"
        // in every other sense, and a wall here. Without this a poured bucket at
        // the waterline would write runoff over ocean cells, which would then find
        // no supply of their own and delete themselves — holes in the sea.
        if (is_liquid()) return true;
        if (!HasProperty(properties, BlockProperty::Solid)) return false;
        if (full_cube_) return true;
        return !get_collision_boxes().empty();
    }

    // Cached flag: true when the block can participate in greedy meshing.
    // True for full cubes or bottom-anchored full-XZ columns whose height is
    // 1 - top_face_offset (water, mud, wet_sand). Slabs, walls, poles, and
    // multi-box stairs are excluded — they emit via per-AABB geometry.
    //
    // This describes the SHAPE, not who draws it: liquids are always drawn by the
    // fluid surface pass (mesh_fluid.hpp), whatever this says, because their
    // surface has four independent corner heights and a merged run has one. See
    // MeshBuilder::is_fluid_drawn, which is the exclusion every generic emitter
    // makes.
    bool greedy_mergeable = true;

    [[nodiscard]] bool is_full_cube() const noexcept { return full_cube_; }

    // Returns collision_boxes if set, otherwise falls back to selection_boxes.
    [[nodiscard]] const std::vector<BlockAABB>& get_collision_boxes() const noexcept {
        return collision_boxes.empty() ? selection_boxes : collision_boxes;
    }

    // A liquid cell, whatever shape it is drawn with.
    [[nodiscard]] bool is_liquid() const noexcept {
        return HasProperty(properties, BlockProperty::Liquid);
    }

    // True when a body is STOPPED by this block. A liquid's shape is a surface,
    // not a wall: water is drawn 14/16 tall so the surface has a height, but a
    // body swims through it, so collision must not treat that shape as solid.
    //
    // This is the single place that answer lives, because three callers have to
    // agree: the collision resolver, the pathfinder's cell classification and
    // the sneak edge-guard. Before it existed the collider called a liquid box
    // solid while the pathfinder called the same cell passable, and the visible
    // symptom is a poured bucket becoming a walkable step (see AGENTS.md).
    [[nodiscard]] bool stops_bodies() const noexcept {
        return !is_liquid();
    }
};

// -----------------------------------------------------------------------------
// Block Registry (Singleton)
// -----------------------------------------------------------------------------
class BlockRegistry {
public:
    static constexpr size_t MAX_BLOCK_TYPES = 256;

    [[nodiscard]] static BlockRegistry& get_instance() noexcept {
        static BlockRegistry instance;
        return instance;
    }

    BlockRegistry(const BlockRegistry&) = delete;
    BlockRegistry& operator=(const BlockRegistry&) = delete;

    // NOTE: not nodiscard — called for side-effects; return value is optional.
    BlockID register_block(const BlockType& block_type) noexcept {
        if (count >= MAX_BLOCK_TYPES) {
            return 0;
        }
        block_types[count] = block_type;
        block_types[count].id = static_cast<BlockID>(count);
        ++count;
        return block_types[count - 1].id;
    }

    // Unchecked fast access for hot paths where id is guaranteed valid.
    // In debug builds, asserts id is in range; in release builds, no overhead.
    [[nodiscard]] const BlockType& get_block_fast(BlockID id) const noexcept {
        assert(id < MAX_BLOCK_TYPES && "get_block_fast: id out of range");
        return block_types[id];
    }

    // Safe access with bounds checking.
    [[nodiscard]] const BlockType& get_block(BlockID id) const noexcept {
        if (id >= count) {
            return empty_block;
        }
        return block_types[id];
    }

    // Mutable access for post-registration patching (e.g. texture indices).
    [[nodiscard]] BlockType* get_block_mutable(BlockID id) noexcept {
        if (id >= count) {
            return nullptr;
        }
        return &block_types[id];
    }

    [[nodiscard]] size_t get_count() const noexcept {
        return count;
    }

    // Data-driven slab family: each family groups a bottom half, top half, and
    // merged full block so the placement code can orient and merge generically
    // without hardcoding IDs.  Built from "slab_family" fields in the JSON.
    struct SlabFamily {
        BlockID bottom = 0;
        BlockID top    = 0;
        BlockID full   = 0;
    };
    struct StairFamily {
        BlockID base = 0;  // stair/n (inventory variant)
        BlockID s = 0, e = 0, w = 0;
        BlockID n_up = 0, s_up = 0, e_up = 0, w_up = 0;
    };
    struct WallFamily {
        BlockID base = 0;  // wall/n (inventory variant)
        BlockID s = 0, e = 0, w = 0;
        BlockID full = 0;
    };
    [[nodiscard]] const SlabFamily* get_slab_family(BlockID id) const noexcept;
    [[nodiscard]] const StairFamily* get_stair_family(BlockID id) const noexcept;
    [[nodiscard]] const WallFamily* get_wall_family(BlockID id) const noexcept;

    // Resolves a block name to its registered id (case-sensitive, matches
    // block_definitions.json "name" fields). Returns AIR (0) when unknown.
    [[nodiscard]] BlockID get_block_id_by_name(const char* name) const noexcept;

    // Whether the block is flagged "hidden" in block_definitions.json
    // (placement-only variants like stair orientations are hidden from the
    // inventory /give list; their ids still exist in the positional save format).
    [[nodiscard]] bool is_hidden(BlockID id) const noexcept {
        return id < MAX_BLOCK_TYPES && hidden_[id];
    }

    void initialize_default_blocks() noexcept;
    bool load_shapes_from_json(const godot::String& json_path) noexcept;
    bool load_from_json(const godot::String& json_path) noexcept;

private:
    BlockRegistry() = default;

    std::array<BlockType, MAX_BLOCK_TYPES> block_types{};
    size_t count = 0;
    std::unordered_map<std::string, BlockShape> shapes{};

    // Slab family lookup: maps block id → 1-indexed family (0 = not in any).
    std::vector<SlabFamily> families_;
    std::vector<std::string> family_names_;
    std::unordered_map<std::string, size_t> family_index_;
    std::array<BlockID, MAX_BLOCK_TYPES> slab_family_map_{};

    // "hidden" flag per block id, parsed from block_definitions.json.
    std::array<bool, MAX_BLOCK_TYPES> hidden_{};

    // Stair family lookup: maps block id → 1-indexed family (0 = not in any).
    std::vector<StairFamily> stair_families_;
    std::vector<std::string> stair_family_names_;
    std::unordered_map<std::string, size_t> stair_family_index_;
    std::array<BlockID, MAX_BLOCK_TYPES> stair_family_map_{};

    // Wall family lookup: maps block id → 1-indexed family (0 = not in any).
    std::vector<WallFamily> wall_families_;
    std::vector<std::string> wall_family_names_;
    std::unordered_map<std::string, size_t> wall_family_index_;
    std::array<BlockID, MAX_BLOCK_TYPES> wall_family_map_{};

    // Pre-constructed empty block for out-of-bounds queries.
    static inline BlockType empty_block = []() {
        BlockType bt{};
        bt.id = 0;
        bt.name = "air";
        bt.properties = BlockProperty::None;
        bt.visible_faces = {false, false, false, false, false, false};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        return bt;
    }();
};

// -----------------------------------------------------------------------------
// Common Block IDs
// -----------------------------------------------------------------------------
namespace BlockIDs {
    constexpr BlockID AIR            = 0;
    constexpr BlockID STONE          = 1;
    constexpr BlockID DIRT           = 2;
    constexpr BlockID GRASS          = 3;
    constexpr BlockID SAND           = 4;
    constexpr BlockID SURFACE_WATER  = 5;
    constexpr BlockID WATER          = 6;
    constexpr BlockID WOOD           = 7;
    constexpr BlockID LEAVES         = 8;
    constexpr BlockID BEDROCK        = 9;
    constexpr BlockID MUD            = 10;
    constexpr BlockID WET_SAND       = 11;
    constexpr BlockID MUD_FULL       = 12;
    constexpr BlockID WET_SAND_FULL  = 13;
    constexpr BlockID LIGHT_BLOCK    = 14;
    constexpr BlockID LIGHT_RED      = 15;
    constexpr BlockID LIGHT_GREEN    = 16;
    constexpr BlockID LIGHT_BLUE     = 17;
    constexpr BlockID SNOW           = 18;
    constexpr BlockID GRAVEL         = 19;
    constexpr BlockID CACTUS         = 20;
    constexpr BlockID OAK_SLAB       = 21;
    constexpr BlockID OAK_SLAB_TOP   = 22;
    constexpr BlockID OAK_STAIRS_N     = 23;
    constexpr BlockID OAK_FENCE      = 24;
    constexpr BlockID OAK_WALL_N     = 25;
    constexpr BlockID OAK_DOUBLE_SLAB = 26;
    constexpr BlockID OAK_STAIRS_S     = 27;
    constexpr BlockID OAK_STAIRS_E     = 28;
    constexpr BlockID OAK_STAIRS_W     = 29;
    constexpr BlockID OAK_STAIRS_N_UP  = 30;
    constexpr BlockID OAK_STAIRS_S_UP  = 31;
    constexpr BlockID OAK_STAIRS_E_UP  = 32;
    constexpr BlockID OAK_STAIRS_W_UP  = 33;
    constexpr BlockID OAK_WALL_S     = 34;
    constexpr BlockID OAK_WALL_E     = 35;
    constexpr BlockID OAK_WALL_W     = 36;
    constexpr BlockID OAK_WALL_FULL  = 37;
    constexpr BlockID COAL_ORE       = 39;
    constexpr BlockID GOLD_ORE       = 40;
    constexpr BlockID ICE            = 41;
    constexpr BlockID CRAFTING_TABLE = 42;
    constexpr BlockID OAK_STUMP      = 43;
    constexpr BlockID OAK_STUMP_TOP   = 44;
    constexpr BlockID OAK_STUMP_DOUBLE = 45;
}

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_TYPES_HPP