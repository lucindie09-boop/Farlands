#ifndef FARLANDS_ENTITY_LIGHTS_HPP
#define FARLANDS_ENTITY_LIGHTS_HPP

#include <cstddef>
#include <cstdint>

namespace VoxelEngine {

// The moving lights the world draws besides the player's own glow: today, the torch in a
// goblin's offhand (scripts/mob_lights.gd gathers them, ChunkManager::set_entity_lights
// resolves them). They are shader uniforms rather than blocks in the light grid -- see
// world/player_light.hpp for why a light that moves every frame must not be marched
// through the chunk map.
//
// The cap is ALSO written as `MAX_ENTITY_LIGHTS` in the three shaders that loop over the
// array (Assets/shaders/voxel_shader.gdshader, voxel_shader_water.gdshader and
// item_lighting.gdshaderinc). A count past their fixed-size arrays is a light that is
// silently not drawn, so the copies must agree: four places, one number.
inline constexpr int kMaxEntityLights = 4;

// The radius an entity light reaches, in blocks. The shaders default their own uniform to
// the same 7.0; the C++ side pushes this one so a change lands in one place.
//
// Half the torch's own LEVEL: 14 levels, 7 blocks. That halving is the rule the player's glow
// uses too (EnvironmentController::update takes half of PlayerLight::get_level), so one torch
// lights one pool in either hand -- the parity matters more than the absolute size, and the
// two used to disagree (a flat 8 for the player against 14 for a mob). The squared falloff
// leaves 25% at half the radius, so 7 blocks is a tight, close pool rather than a room. The
// cost does not depend on it (the fragment loop is one distance either way), and
// scripts/mob_lights.gd tests this same sphere against the camera's frustum.
inline constexpr float kEntityLightRadius = 7.0f;

// One light as the shaders receive it: a world position and an RGB colour that already
// carries the light's own intensity. Packing the level into the colour is what lets the
// shaders take one array instead of two -- the quadratic falloff is the only distance
// term, and the level is how bright the light is at its source. Kept godot-free so the
// packing can be tested without an engine.
struct EntityLight {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// A light's shader colour from its level (items.json's 0-15 scale) and its tint: level/15
// of the colour, so a torch at 14 starts at 0.933 of its own hue. A level outside 0-15 is
// clamped rather than dropped: the registry already clamps on read, and a light that is
// somehow out of range is still a light.
inline EntityLight pack_entity_light(float x, float y, float z, int32_t level,
                                     float r, float g, float b) {
    const int32_t clamped = level < 0 ? 0 : (level > 15 ? 15 : level);
    const float share = static_cast<float>(clamped) / 15.0f;
    EntityLight out;
    out.x = x;
    out.y = y;
    out.z = z;
    out.r = r * share;
    out.g = g * share;
    out.b = b * share;
    return out;
}

// How many of the lights asked for fit the shader arrays. The caller's list is in
// priority order (nearest first), so the tail is what falls off.
inline int clamp_entity_light_count(int count) {
    if (count < 0) return 0;
    return count > kMaxEntityLights ? kMaxEntityLights : count;
}

}  // namespace VoxelEngine

#endif  // FARLANDS_ENTITY_LIGHTS_HPP
