#include "godot_bindings/loot_tables.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <string>

#include "core/item_registry.hpp"
#include "core/loot_table.hpp"

using namespace godot;
using namespace VoxelEngine;

bool LootTables::load(const String& path) {
    const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null()) {
        WARN_PRINT("loot_tables.json missing: nothing will drop loot");
        return false;
    }
    const String text = file->get_as_text();
    std::string error;
    if (!LootRegistry::get_instance().load_from_json(std::string(text.utf8().get_data()), &error)) {
        WARN_PRINT(String("loot_tables.json: ") + String(error.c_str()));
        return false;
    }
    return true;
}

Array LootTables::roll(const String& table) {
    Array out;
    const std::vector<LootDrop> drops = LootRegistry::get_instance().roll(
        std::string(table.utf8().get_data()));
    for (const LootDrop& drop : drops) {
        // A table names its items the way items.json does, so a typo is caught here
        // once rather than turning into an empty drop nobody can explain.
        const BlockID id = ItemRegistry::get_instance().get_item_id_by_name(drop.item.c_str());
        if (id == BlockIDs::AIR) {
            WARN_PRINT(String("loot_tables.json: \"") + String(drop.item.c_str())
                       + "\" is not an item in items.json");
            continue;
        }
        Dictionary entry;
        entry["block_id"] = static_cast<int>(id);
        entry["count"] = static_cast<int>(drop.count);
        out.push_back(entry);
    }
    return out;
}

void LootTables::_bind_methods() {
    ClassDB::bind_static_method("LootTables", D_METHOD("load", "path"), &LootTables::load);
    ClassDB::bind_static_method("LootTables", D_METHOD("roll", "table"), &LootTables::roll);
}
