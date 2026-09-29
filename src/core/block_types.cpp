// The registry itself: the fluid kind name table (the one place the loader, the
// state table and any diagnostic agree on a spelling), the name lookup, and the
// slab/stair/wall family accessors. The JSON readers are in
// core/block_types_shapes.cpp and core/block_types_load.cpp, the built-in blocks in
// core/block_types_defaults.cpp.

#include "core/block_types.hpp"

#include <cstring>

namespace VoxelEngine {

// Fluid kind names, as block_definitions.json spells them. Deliberately a
// string table in one place: the loader, the state table and any diagnostic all
// have to agree, and a new fluid is one line here plus its traits in
// src/fluids/fluid_rules.cpp.
const char* fluid_kind_name(FluidKind kind) noexcept {
    switch (kind) {
        case FluidKind::Water: return "water";
        case FluidKind::Lava:  return "lava";
        case FluidKind::Acid:  return "acid";
        case FluidKind::None:  break;
    }
    return "none";
}

FluidKind fluid_kind_from_name(const char* name) noexcept {
    if (name == nullptr) return FluidKind::None;
    const std::string_view s(name);
    if (s == "water") return FluidKind::Water;
    if (s == "lava")  return FluidKind::Lava;
    if (s == "acid")  return FluidKind::Acid;
    return FluidKind::None;
}

BlockID BlockRegistry::get_block_id_by_name(const char* name) const noexcept {
    if (name == nullptr) {
        return BlockIDs::AIR;
    }
    for (size_t i = 0; i < count; ++i) {
        const BlockType& bt = block_types[i];
        if (bt.name != nullptr && std::strcmp(bt.name, name) == 0) {
            return bt.id;
        }
    }
    return BlockIDs::AIR;
}

const BlockRegistry::SlabFamily* BlockRegistry::get_slab_family(BlockID id) const noexcept {
    if (id >= MAX_BLOCK_TYPES) return nullptr;
    const BlockID fi = slab_family_map_[id];
    if (fi == 0) return nullptr;
    return &families_[static_cast<size_t>(fi - 1)];
}

const BlockRegistry::StairFamily* BlockRegistry::get_stair_family(BlockID id) const noexcept {
    if (id >= MAX_BLOCK_TYPES) return nullptr;
    const BlockID fi = stair_family_map_[id];
    if (fi == 0) return nullptr;
    return &stair_families_[static_cast<size_t>(fi - 1)];
}

const BlockRegistry::WallFamily* BlockRegistry::get_wall_family(BlockID id) const noexcept {
    if (id >= MAX_BLOCK_TYPES) return nullptr;
    const BlockID fi = wall_family_map_[id];
    if (fi == 0) return nullptr;
    return &wall_families_[static_cast<size_t>(fi - 1)];
}

} // namespace VoxelEngine
