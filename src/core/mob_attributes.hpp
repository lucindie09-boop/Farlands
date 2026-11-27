#ifndef FARLANDS_MOB_ATTRIBUTES_HPP
#define FARLANDS_MOB_ATTRIBUTES_HPP

// -----------------------------------------------------------------------------
// Mob attributes as data.
//
// The numbers that make a goblin a goblin — how much health it has, what its blow
// costs, how fast it walks, how far it will follow, how far its shout carries, how
// long its anger lasts, how high a ledge it steps over, how far it may fall and what a
// landing costs, the armor it starts with and how much of a shove its weight shrugs
// off — were constants in scripts/goblin.gd, so tuning one was a code change and the
// second mob would copy a sheet of them. They live in
// res://data/mobs.json now, the way blocks, biomes, items and shaders do: the
// walker reads the entry named by its own `mob`, and the next mob is a data entry
// with a model.
//
// Three units are easy to mix up and every field says which it is: health and
// damage are in HALF-HEARTS (the unit `PlayerController.get_health()` counts in),
// speed is vanilla's movement value (the acceleration a body adds per tick — see
// MOVEMENT_SPEED in scripts/goblin.gd for the arithmetic that turns it into blocks
// a second), and every distance is in blocks — the follow and alert ranges, the step
// height, and the distance a fall stays harmless. `armor` is a count of defense points
// (the same points a worn chestplate is worth) and `knockback_resistance` is a share
// from 0 to 1, not a distance.
//
// Godot-free: the document is parsed with `schematic::parse_json`, so the test
// binary reads an entry straight out of a string and holds the shipped file to what
// a goblin is.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VoxelEngine {

// One mob's numbers. Every field has a default, so a file may state only what it
// changes -- but the goblin states all of them, because that file is the answer to
// "what is a goblin" and not a patch over a script.
struct MobAttributes {
    std::string name;
    float health = 10.0f;        // half-hearts
    float damage = 2.0f;         // half-hearts one blow costs
    float speed = 0.25f;         // vanilla's movement value
    float follow_range = 32.0f;  // blocks: past this a chase forgets its target
    float alert_box = 16.0f;     // blocks, on every axis, that a pack's shout carries
    float step_height = 0.6f;            // blocks: the ledge walked up without jumping
    float safe_fall_distance = 3.0f;     // blocks it may fall before a landing hurts
    float fall_damage_multiplier = 1.0f; // scales what that landing costs
    float armor = 0.0f;                  // defense points, before any worn armor
    float knockback_resistance = 0.0f;   // 0..1 of a shove's horizontal push shrugged off
    int32_t anger_min_ticks = 400;
    int32_t anger_max_ticks = 799;
};

// Every mob the game loaded, by name. Filled once at startup from
// res://data/mobs.json (see Mobs::load in the Godot bindings, which owns the file
// read; this half owns the parsing and the rules).
class MobRegistry {
public:
    static MobRegistry& get_instance();

    // Parses a whole {"mobs": {...}} document. Returns false and leaves the current
    // mobs untouched when the text is not that document; `error`, when given, names
    // the mob and the field that made it false.
    [[nodiscard]] bool load_from_json(const std::string& text, std::string* error = nullptr);

    // nullptr when no mob has that name: a walker with no entry keeps its own
    // constants, which is why a missing file is a fallback and not a broken body.
    [[nodiscard]] const MobAttributes* find(const std::string& name) const noexcept;

    [[nodiscard]] size_t mob_count() const noexcept { return mobs_.size(); }

private:
    std::vector<MobAttributes> mobs_;
};

} // namespace VoxelEngine

#endif // FARLANDS_MOB_ATTRIBUTES_HPP
