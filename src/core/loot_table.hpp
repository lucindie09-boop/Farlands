#ifndef FARLANDS_LOOT_TABLE_HPP
#define FARLANDS_LOOT_TABLE_HPP

// -----------------------------------------------------------------------------
// Loot tables: what a dead body leaves behind.
//
// The shape is the one loot tables take wherever they are written as data — a
// table is a list of POOLS, a pool is rolled a number of times, and each roll
// picks one ENTRY out of its pool in proportion to that entry's weight. The
// picked entry may then fail its own `chance`, and only then does it choose how
// many to give from its count range:
//
//     pools -> rolls -> weighted pick -> chance -> count
//
// Those are deliberately separate steps, because they are three different
// questions (which item, whether at all, how many) and one percentage cannot
// answer all three: "a third drop an ear" and "an ear is one of three items"
// are different tables, and writing the second as a chance silently reprices it
// whenever a fourth item is added to the pool.
//
// Godot-free. The document is parsed with `schematic::parse_json`, so the test
// binary reads a table straight out of a string, and the roll takes the caller's
// generator, so a test can pin a seed and count what came out.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace VoxelEngine {

// One thing a pool can give: the item's name as written in the file (`items.json`
// spells the id), how many, how likely it is to win its pool's pick, and how
// likely it is to survive after winning it.
struct LootEntry {
    std::string item;
    int32_t count_min = 1;
    int32_t count_max = 1;
    double weight = 1.0;  // a share of its own pool, not a percentage
    double chance = 1.0;  // a percentage, rolled after the pick
};

// One independent group of rolls. Pools never see each other: an ear does not
// cost a coin, and a reroll of the pebbles cannot take coins away.
struct LootPool {
    int32_t rolls_min = 1;
    int32_t rolls_max = 1;
    std::vector<LootEntry> entries;
};

struct LootTable {
    std::string name;
    std::vector<LootPool> pools;
};

// One thing that came out, resolved to a name and a count. A count of zero is
// never emitted: an entry whose range landed on nothing gives nothing, the same
// way a stack of nothing is not an item.
struct LootDrop {
    std::string item;
    int32_t count = 0;
};

// Every table the game loaded, by name. Loaded once at startup from
// res://data/loot_tables.json (see LootTables::load in the Godot bindings, which
// owns the file read; this half owns the parsing and the dice).
class LootRegistry {
public:
    static LootRegistry& get_instance();

    // Parses a whole {"tables": {...}} document. Returns false and leaves the
    // current tables untouched when the text is not that document; `error`, when
    // given, names the pool or entry that made it false.
    [[nodiscard]] bool load_from_json(const std::string& text, std::string* error = nullptr);

    // nullptr when no table has that name.
    [[nodiscard]] const LootTable* find(const std::string& name) const noexcept;

    // One table's drops, rolled with `rng` — the caller's generator, so a test
    // can pin a seed and a run can be repeated. Unknown table: empty.
    [[nodiscard]] std::vector<LootDrop> roll(const std::string& name, std::mt19937& rng) const;

    // The same, on the registry's own generator, for a caller that has no reason
    // to keep one (the game's own drops).
    [[nodiscard]] std::vector<LootDrop> roll(const std::string& name);

    [[nodiscard]] size_t table_count() const noexcept { return tables_.size(); }

private:
    std::vector<LootTable> tables_;
    std::mt19937 rng_;
};

} // namespace VoxelEngine

#endif // FARLANDS_LOOT_TABLE_HPP
