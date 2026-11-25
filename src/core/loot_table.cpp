#include "core/loot_table.hpp"

#include "schematic/minimal_json.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace VoxelEngine {

namespace {

using schematic::JsonValue;

// A number, a [min, max] pair, or {"min": m, "max": n}: the spellings a count and a
// roll count take. A pair is what a hand-written table wants (one line per drop) and
// the object is what a data pack exports, so both are read here.
bool read_range(const JsonValue& value, int32_t& lo, int32_t& hi, const std::string& where,
                std::string* error) {
    auto as_int = [](const JsonValue& number) {
        return static_cast<int32_t>(std::llround(number.as_number()));
    };
    if (value.is_number()) {
        lo = as_int(value);
        hi = lo;
        return true;
    }
    if (value.is_array() && value.items().size() == 2) {
        lo = as_int(value.items()[0]);
        hi = as_int(value.items()[1]);
    } else if (value.is_object() && value.member("min") != nullptr && value.member("max") != nullptr) {
        lo = as_int(*value.member("min"));
        hi = as_int(*value.member("max"));
    } else {
        if (error != nullptr) *error = where + ": expected a number or a [min, max] pair";
        return false;
    }
    if (lo > hi) {
        if (error != nullptr) *error = where + ": min is above max";
        return false;
    }
    return true;
}

bool read_pool(const JsonValue& value, size_t index, LootPool& pool, std::string* error) {
    const std::string where = "pool " + std::to_string(index);
    if (const JsonValue* rolls = value.member("rolls"); rolls != nullptr) {
        if (!read_range(*rolls, pool.rolls_min, pool.rolls_max, where + " rolls", error)) {
            return false;
        }
    }
    if (pool.rolls_min < 0) {
        if (error != nullptr) *error = where + ": rolls cannot be negative";
        return false;
    }
    const JsonValue* entries = value.member("entries");
    if (entries == nullptr || !entries->is_array() || entries->items().empty()) {
        if (error != nullptr) *error = where + ": needs a non-empty \"entries\" array";
        return false;
    }
    for (size_t i = 0; i < entries->items().size(); ++i) {
        const JsonValue& entry = entries->items()[i];
        const std::string at = where + " entry " + std::to_string(i);
        const JsonValue* item = entry.member("item");
        if (item == nullptr || !item->is_string() || item->as_string().empty()) {
            if (error != nullptr) *error = at + ": needs an \"item\" name";
            return false;
        }
        LootEntry out;
        out.item = item->as_string();
        if (const JsonValue* count = entry.member("count"); count != nullptr) {
            if (!read_range(*count, out.count_min, out.count_max, at + " count", error)) {
                return false;
            }
        }
        if (const JsonValue* weight = entry.member("weight"); weight != nullptr) {
            out.weight = weight->as_number();
        }
        if (const JsonValue* chance = entry.member("chance"); chance != nullptr) {
            out.chance = chance->as_number();
        }
        if (out.weight <= 0.0) {
            if (error != nullptr) *error = at + ": weight must be above zero";
            return false;
        }
        if (out.chance < 0.0 || out.chance > 1.0) {
            if (error != nullptr) *error = at + ": chance must be between 0 and 1";
            return false;
        }
        pool.entries.push_back(std::move(out));
    }
    return true;
}

bool read_table(const std::string& name, const JsonValue& value, LootTable& table,
                std::string* error) {
    table.name = name;
    const JsonValue* pools = value.member("pools");
    if (pools == nullptr || !pools->is_array() || pools->items().empty()) {
        if (error != nullptr) {
            *error = "table \"" + name + "\": needs a non-empty \"pools\" array";
        }
        return false;
    }
    for (size_t i = 0; i < pools->items().size(); ++i) {
        LootPool pool;
        if (!read_pool(pools->items()[i], i, pool, error)) return false;
        table.pools.push_back(std::move(pool));
    }
    return true;
}

} // namespace

LootRegistry& LootRegistry::get_instance() {
    static LootRegistry registry;
    return registry;
}

bool LootRegistry::load_from_json(const std::string& text, std::string* error) {
    JsonValue root;
    std::string parse_error;
    if (!schematic::parse_json(text, root, &parse_error)) {
        if (error != nullptr) *error = "not JSON: " + parse_error;
        return false;
    }
    const JsonValue* tables = root.is_object() ? root.member("tables") : nullptr;
    if (tables == nullptr || !tables->is_object()) {
        if (error != nullptr) *error = "expected {\"tables\": {...}}";
        return false;
    }
    std::vector<LootTable> parsed;
    parsed.reserve(tables->members().size());
    for (const auto& [name, value] : tables->members()) {
        if (!name.empty() && name[0] == '_') continue;  // a comment, like every data file here
        LootTable table;
        if (!read_table(name, value, table, error)) return false;
        parsed.push_back(std::move(table));
    }
    // Only on success: a file that failed to parse is a mistake to fix, not a reason
    // for every body in the game to stop dropping anything.
    tables_ = std::move(parsed);
    return true;
}

const LootTable* LootRegistry::find(const std::string& name) const noexcept {
    for (const LootTable& table : tables_) {
        if (table.name == name) return &table;
    }
    return nullptr;
}

std::vector<LootDrop> LootRegistry::roll(const std::string& name, std::mt19937& rng) const {
    std::vector<LootDrop> out;
    const LootTable* table = find(name);
    if (table == nullptr) return out;
    for (const LootPool& pool : table->pools) {
        if (pool.entries.empty()) continue;
        double total = 0.0;
        for (const LootEntry& entry : pool.entries) total += entry.weight;
        std::uniform_int_distribution<int32_t> roll_count(pool.rolls_min, pool.rolls_max);
        const int32_t rolls = roll_count(rng);
        for (int32_t i = 0; i < rolls; ++i) {
            // The pick is a share of the pool's total weight, not a percentage: a
            // point is taken in [0, total) and the entries are walked until it is
            // used up. That is what makes adding an entry reprice the others.
            std::uniform_real_distribution<double> share(0.0, total);
            double at = share(rng);
            const LootEntry* chosen = &pool.entries.back();
            for (const LootEntry& entry : pool.entries) {
                if (at < entry.weight) {
                    chosen = &entry;
                    break;
                }
                at -= entry.weight;
            }
            // ...and only then its own chance, which is a real percentage and is
            // independent of how many entries the pool has.
            if (chosen->chance < 1.0) {
                std::uniform_real_distribution<double> chance(0.0, 1.0);
                if (chance(rng) >= chosen->chance) continue;
            }
            std::uniform_int_distribution<int32_t> count(chosen->count_min, chosen->count_max);
            const int32_t amount = count(rng);
            if (amount > 0) out.push_back(LootDrop{chosen->item, amount});
        }
    }
    return out;
}

std::vector<LootDrop> LootRegistry::roll(const std::string& name) {
    return roll(name, rng_);
}

} // namespace VoxelEngine
