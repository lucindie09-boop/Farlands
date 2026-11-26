#ifndef FARLANDS_GODOT_BINDINGS_MOBS_HPP
#define FARLANDS_GODOT_BINDINGS_MOBS_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

// The mob attributes, for GDScript: `Mobs.attributes("goblin")` answers what a
// goblin is, and `scripts/goblin.gd` reads its own number out of that instead of a
// constant. Static methods only, like BlockTextures and LootTables, so a script
// calls the class and nothing is instantiated.
//
// The parsing and the rules are src/core/mob_attributes.cpp; this class is the file
// read and the struct -> Dictionary translation, because those are the parts that
// need Godot.
class Mobs : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(Mobs, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // Reads res://data/mobs.json into the registry. False (with a warning) when the
    // file is missing or malformed; the last good entries stay in force.
    static bool load(const godot::String& path);

    // One mob's attributes as a Dictionary: health, damage, speed, follow_range,
    // alert_box and anger_window ([min_ticks, max_ticks]). An empty Dictionary when
    // no entry has that name -- a walker with no entry keeps its own constants.
    static godot::Dictionary attributes(const godot::String& mob);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_MOBS_HPP
