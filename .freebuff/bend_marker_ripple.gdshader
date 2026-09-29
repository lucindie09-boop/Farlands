// Probe-only: the water material's own call site, mirrored.
//
// shaders/voxel_shader_water.gdshader is the only one of the world's two
// materials that declares `world_bend_sway` and passes it to the include, so it
// is the only one whose geometry moves with time. A marker that hard-codes 0
// there cannot measure that, and a marker invented for the purpose should not
// invent a second call shape either: this one is the water shader's call, on a
// mesh small enough to read its position off the frame.
shader_type spatial;
render_mode unshaded, cull_disabled;

#include "res://shaders/world_bend.gdshaderinc"

// Declared here exactly as the water material declares it: the include takes the
// ripple as an argument rather than reading a uniform, so each consumer decides
// whether it has one. The terrain does not, and passes a literal 0.
uniform float world_bend_sway = 0.0;

void vertex() {
    vec3 camera_position = INV_VIEW_MATRIX[3].xyz;
    vec3 unbent = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 bent = world_bend_position(unbent, camera_position, world_bend_sway);
    VERTEX = (inverse(MODEL_MATRIX) * vec4(bent, 1.0)).xyz;
}

void fragment() {
    ALBEDO = vec3(1.0);
}
