shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_mix, shadows_disabled, fog_disabled;

// The cloud mask is generated at runtime by scripts/cloud_layer.gd. Keeping the
// sampler nearest is what makes the cloud edges read as Minecraft-style cells
// instead of a soft alpha blur.
uniform sampler2D cloud_texture : filter_nearest, repeat_enable;
uniform float cloud_span = 3072.0;
uniform float opacity : hint_range(0.0, 1.0) = 0.82;
uniform vec3 cloud_color = vec3(1.0);

varying vec2 cloud_uv;
varying float cloud_side;
varying float cloud_shade;
varying vec3 cloud_normal;

void vertex() {
    // Local cloud space: the node snaps to the mask tile and carries the wind,
    // so the mask and the extruded walls move together.
    cloud_uv = VERTEX.xz / cloud_span;
    // Side walls are marked in the red vertex channel: the mask edge must not
    // punch a transparent texel through the side of the volume.
    cloud_side = COLOR.r;
    cloud_shade = COLOR.g;
    cloud_normal = normalize((MODELVIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
    // The volume is a closed shell: do not draw its inside faces. Use the vertex
    // normal rather than NORMAL here, because NORMAL is flipped on back faces.
    if (dot(cloud_normal, normalize(VIEW)) < 0.0) {
        discard;
    }
    float mask = mix(texture(cloud_texture, cloud_uv).a, 1.0, cloud_side);
    ALBEDO = cloud_color * cloud_shade;
    ALPHA = mask * opacity;
}
