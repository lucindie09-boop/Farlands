#include "core/mob_attributes.hpp"

#include "schematic/minimal_json.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace VoxelEngine {

namespace {

using schematic::JsonValue;

// A field the file may leave out: absent keeps the struct's default, present must be
// a number. That is what makes a new entry cheap (it states what it is) without
// letting a typo read as a zero.
bool read_float(const JsonValue& mob, const char* field, const std::string& where, float& out,
                std::string* error) {
    const JsonValue* value = mob.member(field);
    if (value == nullptr) return true;
    if (!value->is_number()) {
        if (error != nullptr) *error = where + ": \"" + field + "\" must be a number";
        return false;
    }
    out = static_cast<float>(value->as_number());
    return true;
}

// [min, max] or {"min": m, "max": n}: the two spellings a window takes here, the same
// ones the loot table's rolls accept, because a hand-written file wants the pair and
// an exported one wants the object.
bool read_window(const JsonValue& mob, const std::string& where, MobAttributes& out,
                 std::string* error) {
    const JsonValue* value = mob.member("anger_window");
    if (value == nullptr) return true;
    auto as_int = [](const JsonValue& number) {
        return static_cast<int32_t>(std::llround(number.as_number()));
    };
    if (value->is_array() && value->items().size() == 2) {
        out.anger_min_ticks = as_int(value->items()[0]);
        out.anger_max_ticks = as_int(value->items()[1]);
    } else if (value->is_object() && value->member("min") != nullptr &&
               value->member("max") != nullptr) {
        out.anger_min_ticks = as_int(*value->member("min"));
        out.anger_max_ticks = as_int(*value->member("max"));
    } else {
        if (error != nullptr) *error = where + ": \"anger_window\" must be a [min, max] pair";
        return false;
    }
    return true;
}

bool read_mob(const std::string& name, const JsonValue& value, MobAttributes& out,
              std::string* error) {
    const std::string where = "mob \"" + name + "\": ";
    if (!value.is_object()) {
        if (error != nullptr) *error = where + "expected an object of attributes";
        return false;
    }
    out.name = name;
    if (!read_float(value, "health", where, out.health, error)) return false;
    if (!read_float(value, "damage", where, out.damage, error)) return false;
    if (!read_float(value, "speed", where, out.speed, error)) return false;
    if (!read_float(value, "follow_range", where, out.follow_range, error)) return false;
    if (!read_float(value, "alert_box", where, out.alert_box, error)) return false;
    if (!read_float(value, "step_height", where, out.step_height, error)) return false;
    if (!read_float(value, "safe_fall_distance", where, out.safe_fall_distance, error)) {
        return false;
    }
    if (!read_float(value, "fall_damage_multiplier", where, out.fall_damage_multiplier, error)) {
        return false;
    }
    if (!read_float(value, "armor", where, out.armor, error)) return false;
    if (!read_float(value, "knockback_resistance", where, out.knockback_resistance, error)) {
        return false;
    }
    if (!read_window(value, where, out, error)) return false;

    // The ranges that make a body rather than a number: no health at all is a corpse
    // that was never alive, a negative blow heals, and a window that runs backwards is
    // a chase nobody can end.
    if (out.health <= 0.0f) {
        if (error != nullptr) *error = where + "\"health\" must be above zero";
        return false;
    }
    if (out.damage < 0.0f) {
        if (error != nullptr) *error = where + "\"damage\" cannot be negative";
        return false;
    }
    if (out.speed <= 0.0f) {
        if (error != nullptr) *error = where + "\"speed\" must be above zero";
        return false;
    }
    if (out.follow_range <= 0.0f) {
        if (error != nullptr) *error = where + "\"follow_range\" must be above zero";
        return false;
    }
    if (out.alert_box < 0.0f) {
        if (error != nullptr) *error = where + "\"alert_box\" cannot be negative";
        return false;
    }
    // A step height of zero is a body that cannot walk up a slab -- a legless one, and a
    // legal thing to write; a negative one is arithmetic that puts it through the floor.
    if (out.step_height < 0.0f) {
        if (error != nullptr) *error = where + "\"step_height\" cannot be negative";
        return false;
    }
    if (out.safe_fall_distance < 0.0f) {
        if (error != nullptr) *error = where + "\"safe_fall_distance\" cannot be negative";
        return false;
    }
    // A negative multiplier would heal a landing, and so would a negative damage.
    if (out.fall_damage_multiplier < 0.0f) {
        if (error != nullptr) *error = where + "\"fall_damage_multiplier\" cannot be negative";
        return false;
    }
    if (out.armor < 0.0f) {
        if (error != nullptr) *error = where + "\"armor\" cannot be negative";
        return false;
    }
    // A share: below zero would amplify a shove, above one would reverse it.
    if (out.knockback_resistance < 0.0f || out.knockback_resistance > 1.0f) {
        if (error != nullptr) *error = where + "\"knockback_resistance\" must be between 0 and 1";
        return false;
    }
    if (out.anger_min_ticks < 0 || out.anger_max_ticks < out.anger_min_ticks) {
        if (error != nullptr) {
            *error = where + "\"anger_window\" must run forwards and cannot be negative";
        }
        return false;
    }
    return true;
}

} // namespace

MobRegistry& MobRegistry::get_instance() {
    static MobRegistry registry;
    return registry;
}

bool MobRegistry::load_from_json(const std::string& text, std::string* error) {
    JsonValue root;
    std::string parse_error;
    if (!schematic::parse_json(text, root, &parse_error)) {
        if (error != nullptr) *error = "not JSON: " + parse_error;
        return false;
    }
    const JsonValue* mobs = root.is_object() ? root.member("mobs") : nullptr;
    if (mobs == nullptr || !mobs->is_object()) {
        if (error != nullptr) *error = "expected {\"mobs\": {...}}";
        return false;
    }
    std::vector<MobAttributes> parsed;
    parsed.reserve(mobs->members().size());
    for (const auto& [name, value] : mobs->members()) {
        if (!name.empty() && name[0] == '_') continue;  // a comment, like every data file here
        MobAttributes attributes;
        if (!read_mob(name, value, attributes, error)) return false;
        parsed.push_back(std::move(attributes));
    }
    // Only on success: a file that failed to parse is a mistake to fix, not a reason
    // for every body in the game to lose its numbers.
    mobs_ = std::move(parsed);
    return true;
}

const MobAttributes* MobRegistry::find(const std::string& name) const noexcept {
    for (const MobAttributes& mob : mobs_) {
        if (mob.name == name) return &mob;
    }
    return nullptr;
}

} // namespace VoxelEngine
