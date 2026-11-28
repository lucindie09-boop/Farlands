// The Horizon Curve: the world is not a plane, it is a sphere, and the ground
// falls away from the camera the way the ground falls away from you outdoors.
//
// This is the second geometry effect in the Shaders menu, and the opposite idea
// to the first. World Bend closes the world in around the player - the ground
// comes towards them and climbs, the way the world does in the Inception pack.
// This one leaves every distance exactly where it is and sinks the far field, so
// standing on a plain the ground in front of you curves away instead of running
// flat to the edge of the loaded world, a hill a long way off sits lower than it
// is, and the horizon is where the ground has fallen too far to be seen over.
// That is the curve you see outdoors: not a picture of a bend but the arc of the
// surface you are standing on.
//
// The radius has to be small enough to see, and it is worth being honest about
// how small. The Earth's own radius is 6,371 kilometres, and at the far edge of a
// 1,024-block view the ground would be 8 centimetres lower than flat - a rounding
// error, not a look. `world_horizon_radius` is a world a thousand blocks across
// on purpose, which is what turns the same geometry into something the eye can
// read at the distance a player actually looks: at the registry's own default of
// 1,500 blocks the far edge of the view (1,024 blocks out, the render distance the
// game ships with) is drawn 261 blocks lower than flat, the ground 512 blocks out
// is 80 blocks lower, and the ground 128 blocks out is five - which is the band
// where the effect stops being the curve and starts being the scene around you.
//
// The geometry, exactly: a sphere of radius `radius` touches the ground at the
// player's own feet, and the flat world is the plane tangent to it there. A point
// of that plane a horizontal distance d from the eye sits at an angle
// atan(d/radius) around the sphere from the point of tangency, and lands
//
//     radius * (1 - cos(atan(d/radius)))
//
// below the plane. Writing cos(atan(x)) as 1/sqrt(1 + x²) that is the expression
// below: distance squared over twice the radius at first, and never past `radius`
// however far away the point is. It is also, deliberately, the same expression
// the bend's lift uses - the two effects are one piece of geometry with opposite
// signs. Where that lift is measured against the player's own vertical axis and
// levels off at `rise * radius` of sky, this drop is measured against the ground
// underfoot and levels off at `radius` of the world's own thickness.
//
// Only y moves. Wrapping the world onto the sphere for real would also pull it in
// towards the eye - a point at distance d lands at radius * sin(d/radius) along
// the tangent, which is d - d³/(6 * radius²), a third-order term: five blocks of
// pull at a thousand blocks out with the slider at its floor, against two
// hundred-odd blocks of drop. Leaving it out costs nothing anyone can see, and it
// keeps two things the player's own geometry depends on: every vertical edge
// stays vertical, and every distance measured along the ground is the distance it
// was. It is also what makes this safe to draw: the drop is a continuous function
// of a vertex's world position, so two vertices along the edge a chunk shares
// with its neighbour move together, and no surface can be torn open by it.
//
// One knob, because on a sphere one is enough. The drop goes as the square of the
// distance over twice the radius, so an "amount" of a half would be very nearly
// the same picture as a doubled radius - and a slider that does what another
// slider does is a slider that makes the menu worse. The bend has two because its
// mix is between two different shapes; this one already has its shape.
//
// It is included by the two meshes the world is made of, beside the bend, and the
// two are independent: each is measured from where a vertex really is, so the
// horizon drops the world the bend has already moved and either effect is the
// same picture whether or not the other is switched on. Water and ground take the
// identical drop at the identical place - the factor is a pure function of world
// position - so a shoreline stays welded here for the same reason it does there.
//
// Two things this is deliberately not, exactly as for the bend:
//
//   - it is not collision. Collision, the blocks you can reach and the block you
//     are standing on stay where they are; only the drawing moves.
//   - it is not a projection. The vertices go somewhere and the camera's own
//     matrices run afterwards, so walking, pitching and the engine's own culling
//     all go on working on a world whose vertices have moved - which is also why
//     culling has to be told about this exactly as it is for the bend (see
//     src/core/world_cull.hpp and src/mesh/mesh_manager.cpp).
uniform float world_horizon_on = 0.0;
uniform float world_horizon_radius = 1500.0;

// Whether the curve is on at all. The two consumers ask this themselves before
// they call in, because the way back from world space to the mesh's own space is
// the inverse of the mesh's matrix and that is not a thing to compute per vertex
// on every chunk of the world for an effect that ships switched off.
bool world_horizon_active() {
    return world_horizon_on >= 0.5 && world_horizon_radius > 0.0;
}

// How far below its own place a world position is drawn: the sagitta above, in
// blocks, and 0 with the effect off. Only y is changed by this, so a consumer
// stages it as `world.y -= world_horizon_drop(...)` - and because that is all it
// is, the world's horizontal layout, and every vertical edge in it, is untouched.
float world_horizon_drop(vec3 world, vec3 camera_position) {
    if (!world_horizon_active()) {
        return 0.0;
    }
    vec3 relative = world - camera_position;
    // The distance from the camera's own vertical axis: the sphere is under the
    // player's feet, so everything at one distance around them is equally far
    // down the curve, whichever way the player is facing.
    float horizontal = length(relative.xz);
    float radius = max(world_horizon_radius, 1.0);
    float ratio = horizontal / radius;
    return radius * (1.0 - inversesqrt(1.0 + ratio * ratio));
}
