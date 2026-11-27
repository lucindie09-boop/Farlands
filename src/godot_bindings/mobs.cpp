#include "godot_bindings/mobs.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

#include <string>

#include "core/mob_attributes.hpp"

using namespace godot;
using namespace VoxelEngine;

bool Mobs::load(const String& path) {
    const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null()) {
        WARN_PRINT("mobs.json missing: every walker keeps its own constants");
        return false;
    }
    const String text = file->get_as_text();
    std::string error;
    if (!MobRegistry::get_instance().load_from_json(std::string(text.utf8().get_data()), &error)) {
        WARN_PRINT(String("mobs.json: ") + String(error.c_str()));
        return false;
    }
    return true;
}

Dictionary Mobs::attributes(const String& mob) {
    Dictionary out;
    const MobAttributes* attributes =
        MobRegistry::get_instance().find(std::string(mob.utf8().get_data()));
    if (attributes == nullptr) return out;
    out["health"] = attributes->health;
    out["damage"] = attributes->damage;
    out["speed"] = attributes->speed;
    out["follow_range"] = attributes->follow_range;
    out["alert_box"] = attributes->alert_box;
    out["step_height"] = attributes->step_height;
    out["safe_fall_distance"] = attributes->safe_fall_distance;
    out["fall_damage_multiplier"] = attributes->fall_damage_multiplier;
    out["armor"] = attributes->armor;
    out["knockback_resistance"] = attributes->knockback_resistance;
    // The window as a two-item array, in ticks, the order the file writes it in. It is
    // not an int range type: GDScript has none, and a pair reads the same at a glance.
    Array window;
    window.push_back(attributes->anger_min_ticks);
    window.push_back(attributes->anger_max_ticks);
    out["anger_window"] = window;
    return out;
}

void Mobs::_bind_methods() {
    ClassDB::bind_static_method("Mobs", D_METHOD("load", "path"), &Mobs::load);
    ClassDB::bind_static_method("Mobs", D_METHOD("attributes", "mob"), &Mobs::attributes);
}
