#include "doctest.h"
#include "core/mining.hpp"

using namespace VoxelEngine;

namespace {

ItemToolStats make_tool(const char* tool_class, int32_t tier, float speed) {
    ItemToolStats t;
    t.tool_class = tool_class;
    t.tier = tier;
    t.speed = speed;
    return t;
}

BlockType make_block(const char* preferred, int32_t min_tier) {
    BlockType b{};
    b.preferred_tool = preferred;
    b.min_tier = min_tier;
    return b;
}

} // namespace

TEST_CASE("mining: defaults are neutral") {
    const ItemToolStats plain{};
    CHECK_FALSE(plain.is_tool());
    CHECK(plain.tier == 0);
    CHECK(plain.speed == doctest::Approx(1.0f));

    const BlockType block{};
    CHECK(block.preferred_tool.empty());
    CHECK(block.min_tier == 0);

    // A plain item against a plain block never amplifies.
    CHECK(tool_mining_speed(plain, block) == doctest::Approx(1.0f));
}

TEST_CASE("mining: only a matching tool class speeds up a block") {
    const BlockType stone = make_block("pickaxe", 0);
    const ItemToolStats pickaxe = make_tool("pickaxe", 1, 3.0f);
    const ItemToolStats axe = make_tool("axe", 1, 3.0f);

    CHECK(tool_mining_speed(pickaxe, stone) == doctest::Approx(3.0f));
    CHECK(tool_mining_speed(axe, stone) == doctest::Approx(1.0f));
    // A plain (non-tool) item is always bare-hand speed.
    CHECK(tool_mining_speed(ItemToolStats{}, stone) == doctest::Approx(1.0f));
    // A block that names no preferred tool ignores any tool.
    CHECK(tool_mining_speed(pickaxe, make_block("", 0)) == doctest::Approx(1.0f));
}

TEST_CASE("mining: tier gates the bonus but never blocks the break") {
    const BlockType ore = make_block("pickaxe", 2);

    CHECK(tool_mining_speed(make_tool("pickaxe", 1, 3.0f), ore) == doctest::Approx(1.0f));
    CHECK(tool_mining_speed(make_tool("pickaxe", 2, 3.0f), ore) == doctest::Approx(3.0f));
    CHECK(tool_mining_speed(make_tool("pickaxe", 5, 4.5f), ore) == doctest::Approx(4.5f));
    // Wrong class is unaffected by tier.
    CHECK(tool_mining_speed(make_tool("axe", 9, 4.0f), ore) == doctest::Approx(1.0f));
}

TEST_CASE("mining: speed is read from the tool, not assumed") {
    const BlockType planks = make_block("axe", 0);
    CHECK(tool_mining_speed(make_tool("axe", 0, 1.25f), planks) == doctest::Approx(1.25f));
    CHECK(tool_mining_speed(make_tool("axe", 0, 1.0f), planks) == doctest::Approx(1.0f));
}

TEST_CASE("mining: registry lookup only rewards real item tools") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    BlockType target{};
    target.name = "test_tool_target";
    target.properties = BlockProperty::Solid | BlockProperty::Opaque;
    target.visible_faces = {true, true, true, true, true, true};
    target.preferred_tool = "pickaxe";
    const BlockID id = reg.register_block(target);
    CHECK(id != BlockIDs::AIR);

    // Empty hand and unknown blocks are neutral.
    CHECK(mining_speed_multiplier(id, BlockIDs::AIR) == doctest::Approx(1.0f));
    CHECK(mining_speed_multiplier(BlockIDs::AIR, 4095) == doctest::Approx(1.0f));

    // Holding a *block* id never grants tool speed, even if its name matches a
    // class. The standalone test build loads no items, so an item id resolves
    // to no tool stats here too.
    CHECK(mining_speed_multiplier(id, BlockIDs::STONE) == doctest::Approx(1.0f));
    CHECK(mining_speed_multiplier(id, ItemRegistry::FIRST_ITEM_ID) == doctest::Approx(1.0f));

    // A registered block with no preferred tool ignores whatever is held.
    BlockType neutral{};
    neutral.name = "test_tool_neutral";
    const BlockID neutral_id = reg.register_block(neutral);
    CHECK(mining_speed_multiplier(neutral_id, ItemRegistry::FIRST_ITEM_ID) == doctest::Approx(1.0f));
}
