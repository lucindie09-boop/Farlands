#ifndef FARLANDS_FLUID_STATE_TABLE_HPP
#define FARLANDS_FLUID_STATE_TABLE_HPP

// -----------------------------------------------------------------------------
// The bridge between a fluid state and the block that stores it.
//
// The rules in fluid_rules.hpp never see a block id: they think in
// (kind, depth, falling). Storage is the opposite — a block id IS the state, so
// depth lives in the id and nothing else does. This table is the only place the
// two meet, and it is built by SCANNING the registry for blocks that declare a
// fluid state (block_definitions.json "fluid"), never from a hardcoded id list.
//
// That matters because this project has two registries: the JSON the game loads
// and the C++ defaults the tests run on. Their ids for the same state differ,
// so any code that hardcoded one would be wrong in the other. Scanning by
// declaration means the tests exercise the same rules the game does.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "fluids/fluid_rules.hpp"

#include <array>
#include <cstdint>

namespace VoxelEngine {
namespace fluids {

class FluidStateTable {
public:
    // Depth 0..7, the maximum a fluid's traits may use.
    static constexpr int kDepths = 8;
    static constexpr int kKinds = 4;   // FluidKind::None..Acid

    // Drop everything. A registry reload must call build_from again.
    void clear() noexcept;

    // Scan a registry and record every block that declares a fluid state.
    // Returns how many states were found. Logs what it found, because a typo'd
    // kind name in the JSON leaves a block that looks like water and never
    // flows, which is impossible to diagnose from the symptom.
    int build_from(const BlockRegistry& registry) noexcept;

    [[nodiscard]] bool any() const noexcept { return state_count_ > 0; }
    [[nodiscard]] int state_count() const noexcept { return state_count_; }

    // The block that stores this state, or AIR when the loaded registry has no
    // such state — a state with no block to store it in simply cannot be
    // written, which is why callers must check.
    [[nodiscard]] BlockID block_for(const FluidCell& state) const noexcept;

    // The state a block represents. A kind-None cell for anything that is not a
    // fluid state (air, stone, a slab).
    [[nodiscard]] FluidCell state_of(BlockID id) const noexcept;

    // Whether this block is any fluid state at all.
    [[nodiscard]] bool is_fluid(BlockID id) const noexcept {
        return id < BlockRegistry::MAX_BLOCK_TYPES && state_by_id_[id].kind != FluidKind::None;
    }

private:
    [[nodiscard]] static int kind_index(FluidKind kind) noexcept {
        return static_cast<int>(kind);
    }

    // [kind][falling][depth] -> block id, 0 = that state does not exist here.
    std::array<std::array<std::array<BlockID, kDepths>, 2>, kKinds> id_for_{};
    std::array<FluidCell, BlockRegistry::MAX_BLOCK_TYPES> state_by_id_{};
    int state_count_ = 0;
};

} // namespace fluids
} // namespace VoxelEngine

#endif // FARLANDS_FLUID_STATE_TABLE_HPP
