// Probe-only: a marker mesh drawn through both world-effect includes, wired the
// same way the game's two world shaders wire them (each effect in world space,
// the result put back into the mesh's own space, the engine projecting it, and
// both switched off costing nothing but a skipped branch). If this and
// shaders/voxel_shader.gdshader ever disagree about how to call the includes,
// the marker probe is measuring something the world does not do - so it calls
// them exactly as they do, in the same order, with the same camera, and passes 0
// for the ripple the terrain does not carry.
shader_type spatial;
render_mode unshaded, cull_disabled;

#include "res://shaders/world_bend.gdshaderinc"
#include "res://shaders/world_horizon.gdshaderinc"

void vertex() {
    vec3 camera_position = INV_VIEW_MATRIX[3].xyz;
    vec3 unbent = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 world = unbent;
    if (world_bend_active() || world_horizon_active()) {
        if (world_bend_active()) {
            world = world_bend_position(unbent, camera_position, 0.0);
        }
        if (world_horizon_active()) {
            world.y -= world_horizon_drop(unbent, camera_position);
        }
        VERTEX = (inverse(MODEL_MATRIX) * vec4(world, 1.0)).xyz;
    }
}

void fragment() {
    ALBEDO = vec3(1.0);
}
