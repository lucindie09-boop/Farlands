#include "doctest.h"

#include "render/entity_lights.hpp"

using namespace VoxelEngine;

// The shader side of this is three loops over a fixed array (voxel_shader.gdshader,
// voxel_shader_water.gdshader, item_lighting.gdshaderinc), which a test cannot read.
// What it CAN pin is the arithmetic those loops are handed: how many lights a frame may
// send and what one light's colour is. The probes read the frame back for the rest.

TEST_CASE("entity lights: the count is clamped to what the shaders declare") {
    CHECK(clamp_entity_light_count(-7) == 0);
    CHECK(clamp_entity_light_count(0) == 0);
    CHECK(clamp_entity_light_count(1) == 1);
    CHECK(clamp_entity_light_count(kMaxEntityLights) == kMaxEntityLights);
    CHECK(clamp_entity_light_count(kMaxEntityLights + 1) == kMaxEntityLights);
    CHECK(clamp_entity_light_count(1000) == kMaxEntityLights);
    // The cap is the shader arrays' own length: `uniform vec3 ...[4]`.
    CHECK(kMaxEntityLights == 4);
}

TEST_CASE("entity lights: a level rides in the colour") {
    // A torch is level 14 of 15, colour (1.0, 0.83, 0.6) in data/items.json.
    const EntityLight torch = pack_entity_light(10.0f, 66.5f, -3.0f, 14, 1.0f, 0.83f, 0.6f);
    CHECK(torch.x == doctest::Approx(10.0f));
    CHECK(torch.y == doctest::Approx(66.5f));
    CHECK(torch.z == doctest::Approx(-3.0f));
    CHECK(torch.r == doctest::Approx(14.0f / 15.0f));
    CHECK(torch.g == doctest::Approx(0.83f * 14.0f / 15.0f));
    CHECK(torch.b == doctest::Approx(0.6f * 14.0f / 15.0f));

    // Full level is the tint itself, which is what makes the falloff the only dimming.
    const EntityLight full = pack_entity_light(0.0f, 0.0f, 0.0f, 15, 0.5f, 0.25f, 1.0f);
    CHECK(full.r == doctest::Approx(0.5f));
    CHECK(full.g == doctest::Approx(0.25f));
    CHECK(full.b == doctest::Approx(1.0f));

    // No level is no light: the colour is black, so a zero-level entry costs the loop one
    // distance and adds nothing.
    const EntityLight dark = pack_entity_light(4.0f, 4.0f, 4.0f, 0, 1.0f, 1.0f, 1.0f);
    CHECK(dark.r == doctest::Approx(0.0f));
    CHECK(dark.g == doctest::Approx(0.0f));
    CHECK(dark.b == doctest::Approx(0.0f));

    // Out of range clamps rather than wrapping or going negative.
    const EntityLight over = pack_entity_light(0.0f, 0.0f, 0.0f, 200, 1.0f, 1.0f, 1.0f);
    CHECK(over.r == doctest::Approx(1.0f));
    const EntityLight under = pack_entity_light(0.0f, 0.0f, 0.0f, -3, 1.0f, 1.0f, 1.0f);
    CHECK(under.r == doctest::Approx(0.0f));
}

TEST_CASE("entity lights: the radius matches the shaders' default") {
    // The three shaders declare `uniform float entity_light_radius = 7.0`; the C++ side
    // pushes this value over it every frame. A drift here would be a light whose shader
    // falloff and whose gather cull disagree about where it stops.
    CHECK(kEntityLightRadius == doctest::Approx(7.0f));
}
