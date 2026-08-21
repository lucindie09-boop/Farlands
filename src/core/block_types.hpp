#ifndef FUK_MINECRAFT_BLOCK_TYPES_HPP
#define FUK_MINECRAFT_BLOCK_TYPES_HPP
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

    // Cached flag: true when the block can participate in greedy meshing.
    // True for full cubes or bottom-anchored full-XZ columns whose height is
    // 1 - top_face_offset (water, mud, wet_sand). Slabs, walls, poles, and
    // multi-box stairs are excluded — they emit via per-AABB geometry.
    bool greedy_mergeable = true;

    [[nodiscard]] bool is_full_cube() const noexcept { return full_cube_; }

    // Returns collision_boxes if set, otherwise falls back to selection_boxes.
    [[nodiscard]] const std::vector<BlockAABB>& get_collision_boxes() const noexcept {
        return collision_boxes.empty() ? selection_boxes : collision_boxes;
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

    // Resolves a block name to its registered id (case-sensitive, matches
    // block_definitions.json "name" fields). Returns AIR (0) when unknown.
    [[nodiscard]] BlockID get_block_id_by_name(const char* name) const noexcept;

    void initialize_default_blocks() noexcept;
    bool load_shapes_from_json(const godot::String& json_path) noexcept;
    bool load_from_json(const godot::String& json_path) noexcept;

private:
    BlockRegistry() = default;

    std::array<BlockType, MAX_BLOCK_TYPES> block_types{};
    size_t count = 0;
    std::unordered_map<std::string, BlockShape> shapes{};

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

    constexpr bool is_oak_slab(BlockID id) {
        return id == OAK_SLAB || id == OAK_SLAB_TOP;
    }

    constexpr bool is_oak_stairs(BlockID id) {
        return (id >= OAK_STAIRS_N && id <= OAK_STAIRS_W_UP) && id != OAK_FENCE;
    }

    constexpr bool is_oak_wall(BlockID id) {
        return id == OAK_WALL_N || id == OAK_WALL_S || id == OAK_WALL_E || id == OAK_WALL_W || id == OAK_WALL_FULL;
    }
}

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_BLOCK_TYPES_HPP