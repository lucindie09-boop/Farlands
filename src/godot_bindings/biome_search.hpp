#ifndef FARLANDS_GODOT_BINDINGS_BIOME_SEARCH_HPP
#define FARLANDS_GODOT_BINDINGS_BIOME_SEARCH_HPP

#include <atomic>
#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "worldgen/chunk_generator.hpp"

// The biome search with a generator of its own, so it can be handed to a thread
// while the world keeps streaming. It is built from the world's parameters and
// configs, shares nothing else with the running world, and `find` only reads:
// one probe is a whole column's climate and surface, and a far answer is
// thousands of them, which is a freeze on the frame that asked.
class BiomeSearch : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) - GDCLASS generates operator=
    GDCLASS(BiomeSearch, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // {found, x, y, z, gave_up, searched} -- the shape ChunkManager.find_biome
    // answers with, plus how the search ended: gave_up, the budget or a cancel
    // stopped it short; searched, the distance it covered.
    godot::Dictionary find(const godot::String& biome_name, int32_t center_x, int32_t center_z,
                           int32_t max_radius_blocks);

    // Stops the walk at its next probe. The chat asks for this before joining the
    // thread on quit or a world change, so the join waits one probe and not the
    // rest of the search.
    void cancel();

    // C++ only: the generator this search walks the world with. The owning
    // ChunkManager hands it one made from the live world's parameters.
    void set_generator(std::unique_ptr<VoxelEngine::ChunkGenerator> generator);

protected:
    static void _bind_methods();

private:
    std::unique_ptr<VoxelEngine::ChunkGenerator> generator_;
    std::atomic<bool> cancelled_{false};
};

#endif  // FARLANDS_GODOT_BINDINGS_BIOME_SEARCH_HPP
