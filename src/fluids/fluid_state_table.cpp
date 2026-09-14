#include "fluids/fluid_state_table.hpp"

#include <cstdio>

namespace VoxelEngine {
namespace fluids {

void FluidStateTable::clear() noexcept {
    for (auto& by_falling : id_for_) {
        for (auto& by_depth : by_falling) by_depth.fill(BlockIDs::AIR);
    }
    state_by_id_.fill(FluidCell{});
    state_count_ = 0;
}

int FluidStateTable::build_from(const BlockRegistry& registry) noexcept {
    clear();
    int counts[kKinds] = { 0, 0, 0, 0 };
    int per_kind_source[kKinds] = { 0, 0, 0, 0 };
    int per_kind_falling[kKinds] = { 0, 0, 0, 0 };
    int zero_depth_ambiguous = 0;

    for (BlockID id = 1; id < BlockRegistry::MAX_BLOCK_TYPES; ++id) {
        const BlockType& type = registry.get_block(id);
        if (!type.is_fluid_state()) continue;
        if (id >= registry.get_count()) break;

        const int kind = kind_index(type.fluid_kind);
        if (kind <= 0 || kind >= kKinds) continue;
        if (type.fluid_depth >= kDepths) continue;

        const int falling = type.fluid_falling ? 1 : 0;
        // A depth-0 non-falling cell is a source; a depth-0 falling cell is a
        // falling column. Any other depth is runoff. The FIRST block found for a
        // state wins (surface_water and water are both the water source), so
        // water is written back as whichever of the two was declared first.
        if (falling == 0 && type.fluid_depth == 0) ++per_kind_source[kind];
        if (falling == 1) {
            ++per_kind_falling[kind];
            if (type.fluid_depth != 0) ++zero_depth_ambiguous;
        }

        BlockID& slot = id_for_[kind][falling][type.fluid_depth];
        if (slot == BlockIDs::AIR) slot = id;
        state_by_id_[id] = FluidCell{ type.fluid_kind, type.fluid_depth, type.fluid_falling };
        ++counts[kind];
        ++state_count_;
    }

    // Report the shape of what was found. A kind with a falling state but no
    // source, or the other way round, cannot flow: the rules have nowhere to put
    // the state they computed, so the fluid simply looks inert — which is
    // impossible to diagnose from the symptom. Deliberately fprintf rather than
    // Godot's WARN_PRINT: this module has no Godot dependency.  
    for (int kind = 1; kind < kKinds; ++kind) {
        if (counts[kind] == 0) continue;
        if (per_kind_source[kind] == 0) {
            std::fprintf(stderr, "FluidStateTable: fluid kind \"%s\" has no source (depth 0) state — it cannot flow\n",
                         fluid_kind_name(static_cast<FluidKind>(kind)));
        }
        if (per_kind_falling[kind] == 0) {
            std::fprintf(stderr, "FluidStateTable: fluid kind \"%s\" has no falling state — columns will not form\n",
                         fluid_kind_name(static_cast<FluidKind>(kind)));
        }
    }
    (void)zero_depth_ambiguous;
    return state_count_;
}

BlockID FluidStateTable::block_for(const FluidCell& state) const noexcept {
    if (state.kind == FluidKind::None) return BlockIDs::AIR;
    const int kind = kind_index(state.kind);
    if (kind <= 0 || kind >= kKinds) return BlockIDs::AIR;
    const int depth = state.depth < kDepths ? state.depth : (kDepths - 1);
    return id_for_[kind][state.falling ? 1 : 0][depth];
}

FluidCell FluidStateTable::state_of(BlockID id) const noexcept {
    if (id >= BlockRegistry::MAX_BLOCK_TYPES) return FluidCell{};
    return state_by_id_[id];
}

} // namespace fluids
} // namespace VoxelEngine
