// The built-in blocks: ids, names, properties and shapes for every block the game
// has without reading a data file. The JSON readers can override any of it; this is
// what a world gets when there is no data directory at all.

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace VoxelEngine {

void BlockRegistry::initialize_default_blocks() noexcept {
    // Idempotent: repeated calls (e.g. once per test case) must not append
    // duplicate defaults, which would eventually overflow MAX_BLOCK_TYPES.
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    // Helper for solid, opaque, AO-generating blocks with all 6 faces visible.
    const auto solid = [&](const char* name) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    };

    // 0: Air
    {
        BlockType bt{};
        bt.name = "air";
        bt.properties = BlockProperty::Transparent | BlockProperty::NoOcclusion;
        bt.visible_faces = {false, false, false, false, false, false};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 1-4: Basic solids
    solid("stone");
    solid("dirt");

    // 3: Grass (bottom face hidden by dirt underneath)
    {
        BlockType bt{};
        bt.name = "grass";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, false, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    solid("sand");

    // 5: Surface water (lowered top face)
    {
        BlockType bt{};
        bt.name = "surface_water";
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, false, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.12f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        // Water's light cost (see light_opacity). The ocean is the big one: a
        // generated seabed is dark, not a glow-lit floor.
        bt.light_opacity = 3;
        // Deliberately NOT a fluid state. The flowing simulation is the dynamic
        // water a player pours, and generated ocean is not part of it: left out
        // of the state table, the ocean never ticks, so it neither spills runoff
        // along its shores when something nearby is edited nor drains through a
        // channel. It is still a wall to the flow (see blocks_fluid), so a poured
        // bucket pools against the sea instead of writing ocean cells away.
        // Giving this a `fluid` state is what would make oceans live — one line
        // in block_definitions.json — and the spill along every flat shore at sea
        // level is the cost of it.
        register_block(bt);
    }

    // 6: Water (lowered top face)
    {
        BlockType bt{};
        bt.name = "water";
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.12f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        // A source: this is the block a poured bucket places, and the block a
        // pool settles into.
        bt.fluid_kind = FluidKind::Water;
        bt.light_opacity = 3;
        register_block(bt);
    }

    // 7-8: Wood & Leaves
    solid("wood");

    // 8: Leaves
    {
        BlockType bt{};
        bt.name = "leaves";
        bt.properties = BlockProperty::Solid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 9: Bedrock
    solid("bedrock");

    // 10: Mud (lowered top face)
    {
        BlockType bt{};
        bt.name = "mud";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.0625f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 11: Wet sand (lowered top face)
    {
        BlockType bt{};
        bt.name = "wet_sand";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.0625f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 12-13: Full variants (no offset)
    solid("mud_full");
    solid("wet_sand_full");

    // 14-17: Light blocks (emissive)
    {
        BlockType bt{};
        bt.name = "light_block";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 15; bt.light_g = 15; bt.light_b = 15;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    {
        BlockType bt{};
        bt.name = "light_red";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 15; bt.light_g = 0; bt.light_b = 0;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    {
        BlockType bt{};
        bt.name = "light_green";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 0; bt.light_g = 15; bt.light_b = 0;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    {
        BlockType bt{};
        bt.name = "light_blue";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 0; bt.light_g = 0; bt.light_b = 15;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 18-20: Snow, Gravel, Cactus
    solid("snow");
    solid("gravel");
    solid("cactus");

    // Fluid states. APPENDED, never inserted: ids are positional, so a new
    // entry here would re-point every id after it. Nothing in the fluid system
    // depends on these particular values either — it resolves states by name
    // (fluids/fluid_state_table.hpp) — which is why the test registry and the
    // game registry can number them differently and still agree on behaviour.
    // The surface of a flowing cell drops with its depth — the reference's
    // liquid height, offset = (depth + 1) / 9 rounded to hundredths — which is
    // what makes a stream slope down instead of sitting at one flat height.
    // Depth 0 is not in this table: a source keeps the established 0.12 of the
    // `water` block above, and a falling cell is FULL height (offset 0), because
    // water in a column is only ever full strength.
    static constexpr float kRunoffOffset[8] = { 0.0f, 0.22f, 0.33f, 0.44f, 0.56f, 0.67f, 0.78f, 0.89f };
    const auto fluid_state = [&](const char* name, FluidKind kind, uint8_t depth, bool falling, uint8_t opacity) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset =
            (falling || depth >= 8) ? 0.0f : kRunoffOffset[depth];
        bt.slipperiness = 0.6f;
        bt.hardness = -1.0f;  // unbreakable, like water
        bt.full_cube_ = true;
        bt.fluid_kind = kind;
        bt.fluid_depth = depth;
        bt.fluid_falling = falling;
        bt.light_opacity = opacity;
        register_block(bt);
    };
    fluid_state("water_runoff_1", FluidKind::Water, 1, false, 3);
    fluid_state("water_runoff_2", FluidKind::Water, 2, false, 3);
    fluid_state("water_runoff_3", FluidKind::Water, 3, false, 3);
    fluid_state("water_runoff_4", FluidKind::Water, 4, false, 3);
    fluid_state("water_runoff_5", FluidKind::Water, 5, false, 3);
    fluid_state("water_runoff_6", FluidKind::Water, 6, false, 3);
    fluid_state("water_runoff_7", FluidKind::Water, 7, false, 3);
    fluid_state("water_fallen", FluidKind::Water, 0, true, 3);

    // The other two fluids, appended for the same reason as everything else
    // here. Their sources are registered next to their runoff because both
    // halves are the same block with a fluid state on it, and the state table
    // (fluids/fluid_state_table.hpp) finds every one of them by scanning this
    // registry — the same scan the game's JSON goes through.
    const auto fluid_source = [&](const char* name, FluidKind kind, uint8_t opacity) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.12f;
        bt.slipperiness = 0.6f;
        bt.hardness = -1.0f;
        bt.full_cube_ = true;
        bt.fluid_kind = kind;
        bt.light_opacity = opacity;
        register_block(bt);
    };
    // Lava stops at depth three (see fluid_rules.cpp), so it has no states
    // deeper than that to store — a depth nothing can reach needs no block.
    fluid_source("lava", FluidKind::Lava, 15);
    fluid_state("lava_runoff_1", FluidKind::Lava, 1, false, 15);
    fluid_state("lava_runoff_2", FluidKind::Lava, 2, false, 15);
    fluid_state("lava_runoff_3", FluidKind::Lava, 3, false, 15);
    fluid_state("lava_fallen", FluidKind::Lava, 0, true, 15);

    fluid_source("acid", FluidKind::Acid, 3);
    fluid_state("acid_runoff_1", FluidKind::Acid, 1, false, 3);
    fluid_state("acid_runoff_2", FluidKind::Acid, 2, false, 3);
    fluid_state("acid_runoff_3", FluidKind::Acid, 3, false, 3);
    fluid_state("acid_runoff_4", FluidKind::Acid, 4, false, 3);
    fluid_state("acid_runoff_5", FluidKind::Acid, 5, false, 3);
    fluid_state("acid_runoff_6", FluidKind::Acid, 6, false, 3);
    fluid_state("acid_runoff_7", FluidKind::Acid, 7, false, 3);
    fluid_state("acid_fallen", FluidKind::Acid, 0, true, 3);
}

} // namespace VoxelEngine
