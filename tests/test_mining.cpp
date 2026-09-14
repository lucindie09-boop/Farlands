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

// A block that a hammer crushes into `crush_result`.
BlockType make_crushable(const char* preferred, int32_t min_tier, BlockID crush_result) {
    BlockType b = make_block(preferred, min_tier);
    b.crush_result = crush_result;
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

TEST_CASE("mining: a hammer is fast on the blocks it can crush, and only those") {
    // Stone is the real shape of this: it prefers the pickaxe, and a hammer
    // still out-mines it because crushing it is what the hammer is for.
    const BlockType stone = make_crushable("pickaxe", 0, BlockIDs::GRAVEL);
    const ItemToolStats hammer = make_tool("hammer", 1, 3.0f);

    CHECK(is_crushable(stone));
    CHECK(is_hammer(hammer));
    CHECK(tool_mining_speed(hammer, stone) == doctest::Approx(3.0f));
    // ...and the block's own preferred tool keeps its bonus.
    CHECK(tool_mining_speed(make_tool("pickaxe", 1, 3.0f), stone) == doctest::Approx(3.0f));

    // An ordinary block is none of the hammer's business.
    const BlockType planks = make_block("axe", 0);
    CHECK_FALSE(is_crushable(planks));
    CHECK(tool_mining_speed(hammer, planks) == doctest::Approx(1.0f));
    CHECK(tool_mining_speed(make_tool("axe", 0, 1.25f), planks) == doctest::Approx(1.25f));

    // A block can also name the hammer outright, with nothing to crush.
    const BlockType named = make_block("hammer", 0);
    CHECK(tool_mining_speed(hammer, named) == doctest::Approx(3.0f));
    CHECK(tool_mining_speed(make_tool("pickaxe", 1, 3.0f), named) == doctest::Approx(1.0f));

    // The tier gate still applies to a hammer like any other tool class.
    const BlockType tough = make_crushable("pickaxe", 2, BlockIDs::SAND);
    CHECK(tool_mining_speed(make_tool("hammer", 1, 3.0f), tough) == doctest::Approx(1.0f));
    CHECK(tool_mining_speed(make_tool("hammer", 2, 4.0f), tough) == doctest::Approx(4.0f));
}

TEST_CASE("mining: only a hammer crushes, and only a block that names a result") {
    const BlockType stone = make_crushable("pickaxe", 0, BlockIDs::GRAVEL);
    const ItemToolStats hammer = make_tool("hammer", 1, 3.0f);
    const ItemToolStats pickaxe = make_tool("pickaxe", 9, 9.0f);

    CHECK(crushed_block(stone, &hammer) == BlockIDs::GRAVEL);
    // Bare hand, a non-tool, and any other tool class leave the block alone —
    // even a much stronger pickaxe.
    CHECK(crushed_block(stone, nullptr) == BlockIDs::AIR);
    CHECK(crushed_block(stone, &ItemToolStats{}) == BlockIDs::AIR);
    CHECK(crushed_block(stone, &pickaxe) == BlockIDs::AIR);
    // A hammer on a block with no crush_result has nothing to produce.
    const BlockType plain = make_block("pickaxe", 0);
    CHECK(crushed_block(plain, &hammer) == BlockIDs::AIR);
}

TEST_CASE("mining: a block's own drop stands, unless a hammer crushes it") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    // The real shape of this: stone yields cobblestone to anything, and
    // cobblestone yields gravel to a hammer.
    const BlockID stone = reg.register_block(make_block("pickaxe", 0));
    const BlockID cobble = reg.register_block(make_crushable("pickaxe", 0, BlockIDs::GRAVEL));
    CHECK(stone != BlockIDs::AIR);
    CHECK(cobble != BlockIDs::AIR);
    if (BlockType* stone_bt = reg.get_block_mutable(stone)) {
        stone_bt->drops = cobble;
    }

    const ItemToolStats hammer = make_tool("hammer", 1, 3.0f);
    const ItemToolStats pickaxe = make_tool("pickaxe", 1, 3.0f);

    // Stone yields cobblestone whatever is held — a hammer included, since
    // stone names no crush_result of its own.
    CHECK(block_drop(stone, nullptr).id == cobble);
    CHECK(block_drop(stone, &pickaxe).id == cobble);
    CHECK(block_drop(stone, &hammer).id == cobble);

    // Cobblestone yields itself, and gravel to a hammer.
    CHECK(block_drop(cobble, nullptr).id == cobble);
    CHECK(block_drop(cobble, &pickaxe).id == cobble);
    CHECK(block_drop(cobble, &hammer).id == BlockIDs::GRAVEL);

    // Substituting the drop keeps it one block: stone never yields 2 of them.
    CHECK(block_drop(stone, nullptr).count == 1);
    CHECK(block_drop(cobble, &hammer).count == 1);
}

TEST_CASE("mining: the crush decides what a break yields") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    // A real registered block carrying a crush_result, since the standalone
    // test build loads no items.json (so no held id can be a hammer here).
    const BlockID source = reg.register_block(make_crushable("pickaxe", 0, BlockIDs::GRAVEL));
    const ItemToolStats hammer = make_tool("hammer", 1, 3.0f);

    const BlockDrop crushed = block_drop(source, &hammer);
    CHECK(crushed.id == BlockIDs::GRAVEL);
    CHECK(crushed.count == 1);

    // Without a hammer the block yields itself, unchanged.
    const BlockDrop bare = block_drop(source, nullptr);
    CHECK(bare.id == source);
    CHECK(bare.count == 1);

    // The held-id wrapper is the same rule, and an empty hand never crushes.
    CHECK(resolve_block_drop(source, BlockIDs::AIR).id == source);
    CHECK(resolve_block_drop(source, ItemRegistry::FIRST_ITEM_ID).id == source);

    // Air never yields anything.
    const BlockDrop nothing = resolve_block_drop(BlockIDs::AIR, BlockIDs::AIR);
    CHECK(nothing.id == BlockIDs::AIR);
    CHECK(nothing.count == 0);
}
