#ifndef FARLANDS_GODOT_BINDINGS_LOOT_TABLES_HPP
#define FARLANDS_GODOT_BINDINGS_LOOT_TABLES_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

// The loot tables, for GDScript: `LootTables.roll("goblin")` answers what a body
// leaves behind, resolved to inventory ids. Static methods only, like
// BlockTextures, so a script calls the class and nothing is instantiated.
//
// The parsing and the dice are src/core/loot_table.cpp; this class is the file read
// and the name -> id resolution, because those two are the parts that need Godot.
class LootTables : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(LootTables, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // Reads res://data/loot_tables.json into the registry. False (with a warning)
    // when the file is missing or malformed; the last good tables stay in force.
    static bool load(const godot::String& path);

    // One table's drops as [{"block_id": int, "count": int}, ...]. An entry naming
    // something items.json does not have is warned about and skipped rather than
    // dropped silently.
    static godot::Array roll(const godot::String& table);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_LOOT_TABLES_HPP
