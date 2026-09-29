#ifndef FARLANDS_BLOCK_PROPERTIES_HPP
#define FARLANDS_BLOCK_PROPERTIES_HPP
#include <cstdint>

// What a block IS, as opposed to what it looks like: the property bitfield the
// renderer, the collision resolver and the light propagator all test, plus the
// fluid identity a block definition may declare. Split out of block_types.hpp
// because these are the vocabulary the rest of the engine speaks, and the file
// they came from is the registry that fills them in.
namespace VoxelEngine {

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

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_PROPERTIES_HPP
