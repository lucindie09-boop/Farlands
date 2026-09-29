#ifndef FARLANDS_PLAYER_LIGHT_HPP
#define FARLANDS_PLAYER_LIGHT_HPP
#include <cstdint>

#include <godot_cpp/variant/color.hpp>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Player light — the level, the colour and the on/off switch of the glow that
// follows the player. It is a set of values and nothing else.
//
// The light the player actually sees is the shader-uniform glow pushed by
// MaterialManager::update_player_light (see EnvironmentController::update), and
// a held item claims these values through PlayerController::update_held_light.
//
// An earlier design ALSO injected a real light block into the chunk map's light
// grid, marching it from block to block in step with the player. That call was
// commented out ("shader handles the visual") and never restored, so the whole
// path — the 3x3x3 chunk-map lock, the remove/add BFS, and the last-position
// tracking it needed — has been deleted rather than left behind as a second,
// unreachable source of truth for how bright the player is.
// -------------------------------------------------------------------------
class PlayerLight {
public:
    // The level and colour a light has before any held item claims it. Named so
    // that handing the light back after a torch is put away restores exactly the
    // state the scene started with rather than a copied guess.
    static constexpr uint8_t DEFAULT_LEVEL = 12;
    static godot::Color default_color() { return godot::Color(1.0f, 0.9f, 0.7f); }

    void set_enabled(bool e) { enabled = e; }
    bool get_enabled() const { return enabled; }
    void set_level(uint8_t l) { level = l; }
    uint8_t get_level() const { return level; }
    // Colour of the light as pushed to the world shaders. Warm white by
    // default — the value the shader uniforms used before this was settable —
    // so a held item that lights nothing changes no pixels.
    void set_color(const godot::Color& c) { color = c; }
    godot::Color get_color() const { return color; }

private:
    bool enabled = true;
    uint8_t level = DEFAULT_LEVEL;
    godot::Color color = default_color();
};

} // namespace VoxelEngine

#endif // FARLANDS_PLAYER_LIGHT_HPP
