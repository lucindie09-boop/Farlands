// World Bend: the ground bends around the camera.
//
// Every other effect in the Shaders menu is a picture laid over the frame. This
// one is geometry: it moves the world's own vertices, so the ground curves up
// and away from the player and the far field climbs over the horizon, the way
// the world does in the Inception packs' bent-world shaders. Those bend the
// vertices of the whole world on the GPU; so does this, from first principles,
// with the camera's own position rather than the world's origin as the centre.
//
// It is included by the two meshes the world is made of - the voxel terrain
// material and the water material - so both bend by exactly the same amount at
// exactly the same place, which is the whole reason they can be bent at all:
// the bend is a pure function of a vertex's world position, so the two vertices
// a mesh edge shares with its neighbour's edge move together and the surface
// stays welded. Nothing else is bent on purpose: the held item, the block-break
// overlay, particles and the third-person body are all within a few blocks of
// the eye, where this function is the identity to within a hundredth of a
// block, and the sky is not part of the world.
//
// Two things this is deliberately not:
//
//   - it is not a projection. The bend moves the geometry where it is, before
//     the camera's own matrices run, so looking up and down, walking, and the
//     engine's own culling all go on working on a world whose vertices have
//     moved. Bending positions rather than the projection is what makes the
//     horizon curve the way it does when the camera pitches: the bend is
//     around the camera's vertical axis, not around the screen's middle.
//   - it is not collision. Collision and the blocks you can reach stay where
//     they are, so the world past a few dozen blocks is drawn somewhere other
//     than where it is: a hill that looks twenty blocks high at a hundred
//     blocks away is forty blocks of flat ground you would walk up if you went
//     there. At the distance a player builds and mines at, the difference is
//     under a block, which is why this can be a look rather than a lie.

// The switch, its amount, and the scale it is measured against. These four are
// the registry's `bend` params in data/shaders.json, pushed by shader_overlay.gd.
uniform float world_bend_on = 0.0;
uniform float world_bend = 0.6;
uniform float world_bend_radius = 256.0;
uniform float world_bend_rise = 1.0;

// The ripple's own two numbers, fixed: it is the shape of the wave (how fast it
// rolls, how many waves fit up a mountain) rather than a look of its own, and
// the amount of it is the water material's `world_bend_sway` uniform, which the
// terrain does not declare and does not want.
const float WORLD_BEND_SWAY_RATE = 0.6;
const float WORLD_BEND_SWAY_WAVES = 0.35;

// Whether the bend is on at all. The two consumers ask this themselves, before
// they call in, because the way back from world space to the mesh's own space is
// the inverse of the mesh's matrix, and that is not a thing to compute per
// vertex on every chunk of the world for an effect that is switched off - which
// is how this one ships. With the switch off the vertex stage is the shader the
// world had before the bend existed: it never inverts anything.
bool world_bend_active() {
    return world_bend_on >= 0.5 && world_bend > 0.0;
}

// Where a world position ends up. `sway` is the water's ripple in blocks, and
// the terrain passes 0 for it: only the water material declares the knob, and
// a world whose *ground* ripples is a different effect with a different name.
vec3 world_bend_position(vec3 world, vec3 camera_position, float sway) {
    if (!world_bend_active()) {
        return world;
    }

    vec3 relative = world - camera_position;
    float horizontal = length(relative.xz);
    if (horizontal < 0.0001) {
        // Directly under or over the eye: the bend is nothing there, and it is
        // also where the direction the pull needs is undefined.
        return world;
    }
    float radius = max(world_bend_radius, 1.0);

    // How far out the ground ends up, horizontally. Its distance from the
    // camera is mixed with that distance wrapped onto a cylinder of `radius`:
    // atan(horizontal/radius)*radius is 0 where the distance is 0 and, however
    // far away the vertex is, never past a quarter turn of that cylinder - so
    // endless world folds into a disc of radius (pi/2)*radius in front of the
    // player and the horizon becomes a circle rather than a line. The mix is a
    // convex one, so `world_bend` scales the bend between none of it and all of
    // it while keeping the map monotone: two vertices at different distances can
    // never trade places, which is what would tear the surface apart.
    //
    // The same pull is applied to both horizontal axes at once, by scaling the
    // pair, rather than to x and z one at a time: the ground closes in towards
    // the camera's vertical axis, so the bend looks the same whichever way the
    // player is facing.
    float wrapped = atan(horizontal / radius) * radius;
    float closed = mix(horizontal, wrapped, world_bend);

    // How far it climbs. Distance squared over the radius at first - the shape
    // of a sheet pinned at the player's feet and lifted at its far edge - and it
    // levels off at `rise * radius` instead of running away. A lift that never
    // levels off is what the tangent of the wrapped distance would give, and a
    // vertex sent to infinity is not a fold, it is a broken frame:
    // `rise * radius` at the default 256-block radius is 256 blocks of sky,
    // which is more than any camera can see over, so nothing visible is lost by
    // levelling off. `radius` is the ballast of the whole bend: the amount of
    // it, the distance it is measured against and the height it tops out at are
    // all one number, so a bend can never be tuned into something unrenderable.
    float ratio = horizontal / radius;
    float lift = world_bend * world_bend_rise * radius * (1.0 - inversesqrt(1.0 + ratio * ratio));

    relative.xz *= closed / horizontal;
    relative.y += lift;

    if (sway > 0.0) {
        // A wave rolling up the sheet: the ripple travels with height, so it
        // reads as the surface moving rather than the whole lake sliding.
        float phase = TIME * TAU * WORLD_BEND_SWAY_RATE + relative.y * WORLD_BEND_SWAY_WAVES;
        relative.x += sin(phase) * sway;
        relative.z += cos(phase * 0.9) * sway * 0.7;
    }

    return camera_position + relative;
}
