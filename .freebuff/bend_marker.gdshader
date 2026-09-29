// Probe-only: a marker mesh drawn through the world-bend include, wired the
// same way the game's two world shaders wire it (bend in world space, put the
// result back into the mesh's own space, let the engine project it). If this
// and shaders/voxel_shader.gdshader ever disagree about how to call the
// include, the marker probe is measuring something the world does not do - so
// it calls it exactly as they do, with the same camera, and passes 0 for the
// ripple the terrain does not carry.
shader_type spatial;
render_mode unshaded, cull_disabled;

#include "res://shaders/world_bend.gdshaderinc"

void vertex() {
    vec3 camera_position = INV_VIEW_MATRIX[3].xyz;
    vec3 unbent = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 bent = world_bend_position(unbent, camera_position, 0.0);
    VERTEX = (inverse(MODEL_MATRIX) * vec4(bent, 1.0)).xyz;
}

void fragment() {
    ALBEDO = vec3(1.0);
}
