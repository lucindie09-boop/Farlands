#ifndef FARLANDS_MOB_ATTRIBUTES_HPP
#define FARLANDS_MOB_ATTRIBUTES_HPP

// -----------------------------------------------------------------------------
// Mob attributes as data.
//
// The numbers that make a goblin a goblin — how much health it has, what its blow
// costs, how fast it walks, how far it will follow, how far its shout carries and
// how long its anger lasts — were constants in scripts/goblin.gd, so tuning one was
// a code change and the second mob would copy a sheet of them. They live in
// res://data/mobs.json now, the way blocks, biomes, items and shaders do: the
// walker reads the entry named by its own `mob`, and the next mob is a data entry
// with a model.
//
// Three units are easy to mix up and every field says which it is: health and
// damage are in HALF-HEARTS (the unit `PlayerController.get_health()` counts in),
// speed is vanilla's movement value (the acceleration a body adds per tick — see
// MOVEMENT_SPEED in scripts/goblin.gd for the arithmetic that turns it into blocks
// a second), and every distance is in blocks.
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
// changes -- but the goblin states all six, because that file is the answer to
// "what is a goblin" and not a patch over a script.
struct MobAttributes {
    std::string name;
    float health = 10.0f;        // half-hearts
    float damage = 2.0f;         // half-hearts one blow costs
    float speed = 0.25f;         // vanilla's movement value
    float follow_range = 32.0f;  // blocks: past this a chase forgets its target
    float alert_box = 16.0f;     // blocks, on every axis, that a pack's shout carries
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
