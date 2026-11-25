#include "doctest.h"

#include "core/loot_table.hpp"
#include "schematic/minimal_json.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace VoxelEngine;

namespace {

// A table written the way data/loot_tables.json writes one, so the parser is asked
// the same questions here that it is asked at startup: two rolls on a weighted pool,
// a fixed count next to a range, and a single entry behind a chance.
const char* kTable = R"({
  "tables": {
    "_comment": "a comment entry is skipped, as in every data file here",
    "test": {
      "pools": [
        { "rolls": 2, "entries": [
          { "item": "coin", "count": [0, 3], "weight": 1 },
          { "item": "rock", "count": 2, "weight": 3 }
        ] },
        { "entries": [ { "item": "ear", "chance": 0.5 } ] }
      ]
    }
  }
})";

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

int count_of(const std::vector<LootDrop>& drops, const std::string& item) {
    int total = 0;
    for (const LootDrop& drop : drops) {
        if (drop.item == item) total += drop.count;
    }
    return total;
}

} // namespace

TEST_CASE("A loot table keeps the shape its file gives it") {
    std::string error;
    CHECK(LootRegistry::get_instance().load_from_json(kTable, &error));
    CHECK(error.empty());

    const LootTable* table = LootRegistry::get_instance().find("test");
    CHECK(table != nullptr);
    if (table == nullptr) return;
    CHECK(table->pools.size() == 2);
    CHECK(LootRegistry::get_instance().find("_comment") == nullptr);

    const LootPool& coins = table->pools[0];
    CHECK(coins.rolls_min == 2);
    CHECK(coins.rolls_max == 2);
    CHECK(coins.entries.size() == 2);
    if (coins.entries.size() == 2) {
        CHECK(coins.entries[0].item == "coin");
        CHECK(coins.entries[0].count_min == 0);   // an empty pocket is an outcome
        CHECK(coins.entries[0].count_max == 3);
        CHECK(coins.entries[0].weight == doctest::Approx(1.0));
        CHECK(coins.entries[1].item == "rock");
        CHECK(coins.entries[1].count_min == 2);   // a plain number is a fixed count
        CHECK(coins.entries[1].count_max == 2);
        CHECK(coins.entries[1].weight == doctest::Approx(3.0));
    }

    const LootPool& ears = table->pools[1];
    CHECK(ears.rolls_min == 1);  // rolls defaults to one
    CHECK(ears.entries.size() == 1);
    if (ears.entries.size() == 1) {
        CHECK(ears.entries[0].chance == doctest::Approx(0.5));
        CHECK(ears.entries[0].count_min == 1);  // count defaults to one
    }
}

TEST_CASE("Rolling a loot table picks by weight, then by chance, then by count") {
    std::string error;
    CHECK(LootRegistry::get_instance().load_from_json(kTable, &error));

    std::mt19937 rng(1337);
    int tables = 0;
    int with_ear = 0;
    int coins = 0;
    int rocks = 0;
    for (int i = 0; i < 2000; ++i) {
        const std::vector<LootDrop> drops = LootRegistry::get_instance().roll("test", rng);
        ++tables;
        for (const LootDrop& drop : drops) {
            CHECK(drop.count > 0);  // an entry that rolled nothing is not emitted
            if (drop.item == "coin") {
                CHECK(drop.count <= 3);
                coins += drop.count;
            } else if (drop.item == "rock") {
                CHECK(drop.count == 2);
                rocks += drop.count;
            }
        }
        if (count_of(drops, "ear") > 0) ++with_ear;
    }
    // Both entries of the weighted pool come up (weight 3 against 1 is a share, not a
    // percentage, so the rock is the more common one but neither is impossible).
    CHECK(coins > 0);
    CHECK(rocks > 0);
    CHECK(rocks > coins);
    // ...and the chance is a rate over the tables, rolled after the pick: half of
    // them carry an ear.
    const double rate = static_cast<double>(with_ear) / static_cast<double>(tables);
    CHECK(rate > 0.42);
    CHECK(rate < 0.58);
}

TEST_CASE("A loot entry whose chance or count can come up empty drops nothing") {
    const char* table = R"({
      "tables": { "never": { "pools": [
        { "rolls": 1, "entries": [ { "item": "ear", "chance": 0.0 } ] },
        { "rolls": 1, "entries": [ { "item": "coin", "count": [0, 0] } ] }
      ] } }
    })";
    std::string error;
    CHECK(LootRegistry::get_instance().load_from_json(table, &error));

    std::mt19937 rng(7);
    for (int i = 0; i < 200; ++i) {
        CHECK(LootRegistry::get_instance().roll("never", rng).empty());
    }
    // An unknown table is empty rather than an error: a body with no table drops none.
    CHECK(LootRegistry::get_instance().roll("goblin", rng).empty());
}

TEST_CASE("A malformed loot table is refused and leaves the last good one in force") {
    std::string error;
    CHECK(LootRegistry::get_instance().load_from_json(kTable, &error));

    CHECK_FALSE(LootRegistry::get_instance().load_from_json("{ \"tables\": ", &error));
    CHECK_FALSE(error.empty());
    CHECK(LootRegistry::get_instance().find("test") != nullptr);

    // A pool with no entries, a negative weight and a chance outside 0..1 are the
    // three shapes worth catching: each is a typo that would otherwise roll silently.
    error.clear();
    CHECK_FALSE(LootRegistry::get_instance().load_from_json(
        R"({"tables": {"t": {"pools": [{"entries": []}]}}})", &error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(LootRegistry::get_instance().load_from_json(
        R"({"tables": {"t": {"pools": [{"entries": [{"item": "a", "weight": 0}] }]}}})",
        &error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(LootRegistry::get_instance().load_from_json(
        R"({"tables": {"t": {"pools": [{"entries": [{"item": "a", "chance": 1.5}] }]}}})",
        &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("The goblin's own loot table is the one the game drops") {
    // The shipped file, parsed the way the game parses it at startup: three
    // independent pools, and the counts and chance read off the file rather than
    // restated here.
    std::string error;
    CHECK(LootRegistry::get_instance().load_from_json(read_file("data/loot_tables.json"), &error));
    const LootTable* goblin = LootRegistry::get_instance().find("goblin");
    CHECK(goblin != nullptr);
    if (goblin == nullptr) return;
    CHECK(goblin->pools.size() == 3);
    if (goblin->pools.size() != 3) return;
    CHECK(goblin->pools[0].entries.size() == 1);
    CHECK(goblin->pools[0].entries[0].item == "silver_coin");
    CHECK(goblin->pools[0].entries[0].count_min == 0);
    CHECK(goblin->pools[0].entries[0].count_max == 2);
    CHECK(goblin->pools[1].entries[0].item == "pebble");
    CHECK(goblin->pools[1].entries[0].count_min == 1);
    CHECK(goblin->pools[1].entries[0].count_max == 4);
    CHECK(goblin->pools[2].entries[0].item == "goblin_ear");
    CHECK(goblin->pools[2].entries[0].chance == doctest::Approx(0.33));

    // Every name the table uses has to be an item, or the binding skips the drop and
    // the goblin leaves nothing for a typo nobody would see.
    schematic::JsonValue items;
    std::string items_error;
    CHECK(schematic::parse_json(read_file("data/items.json"), items, &items_error));
    const schematic::JsonValue* list = items.member("items");
    CHECK(list != nullptr);
    if (list == nullptr) return;
    CHECK(list->is_array());
    for (const LootPool& pool : goblin->pools) {
        for (const LootEntry& entry : pool.entries) {
            bool found = false;
            for (const schematic::JsonValue& item : list->items()) {
                const schematic::JsonValue* name = item.member("name");
                if (name != nullptr && name->as_string() == entry.item) found = true;
            }
            if (!found) MESSAGE("loot_tables.json names " << entry.item << ", which is not in data/items.json");
            CHECK(found);
        }
    }

    // The pools roll independently: over many goblins every range is respected, the
    // ear is about a third, and a coin's zero really happens.
    std::mt19937 rng(99);
    int with_coin = 0;
    int with_ear = 0;
    int goblins = 0;
    for (int i = 0; i < 3000; ++i) {
        const std::vector<LootDrop> drops = LootRegistry::get_instance().roll("goblin", rng);
        ++goblins;
        const int coins = count_of(drops, "silver_coin");
        const int pebbles = count_of(drops, "pebble");
        const int ears = count_of(drops, "goblin_ear");
        CHECK(coins <= 2);
        CHECK(pebbles >= 1);
        CHECK(pebbles <= 4);
        CHECK(ears <= 1);
        if (coins > 0) ++with_coin;
        if (ears > 0) ++with_ear;
    }
    CHECK(with_coin > goblins / 3);  // 0..2 means most of them, not all
    CHECK(with_coin < goblins);
    const double ear_rate = static_cast<double>(with_ear) / static_cast<double>(goblins);
    CHECK(ear_rate > 0.28);
    CHECK(ear_rate < 0.38);
}
