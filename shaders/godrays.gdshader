shader_type canvas_item;

// Custom screen-space god rays (crepuscular rays).
// Classic radial-scatter technique: for every pixel, walk a straight line
// toward the sun's screen-space position, sampling the already-rendered
// frame along the way. Bright pixels near the sun (mostly sky/sun disc,
// since terrain sits well below the brightness threshold) get smeared
// outward and decayed, producing shafts of light behind whatever is
// occluding the sun (terrain, trees, the player's own geometry, etc).
//
// This is fully hand-written — no Environment volumetric fog, no
// DirectionalLight3D shadow sampling, no RenderingServer compositor
// effects. Just SCREEN_TEXTURE + math.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;

// Sun/moon screen-space positions and visibilities pushed from GDScript.
uniform vec2 sun_screen_uv = vec2(0.5, 0.5);
uniform float sun_visibility : hint_range(0.0, 1.0) = 1.0;
uniform vec2 moon_screen_uv = vec2(0.5, 0.5);
uniform float moon_visibility : hint_range(0.0, 1.0) = 0.0;

// Ray shaping.
uniform int num_samples = 48;
uniform float density : hint_range(0.0, 2.0) = 0.85;
uniform float decay : hint_range(0.0, 1.0) = 0.975;
uniform float weight : hint_range(0.0, 2.0) = 0.85;
uniform float exposure : hint_range(0.0, 2.0) = 0.55;
uniform float threshold : hint_range(0.0, 1.0) = 0.18;
uniform vec3 sun_ray_color : source_color = vec3(1.0, 1.0, 1.0);
uniform vec3 moon_ray_color : source_color = vec3(1.0, 1.0, 1.0);

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 accumulate_rays(vec2 screen_uv, vec2 light_uv, float light_visibility, vec3 light_color) {
    vec2 sample_uv = screen_uv;
    vec2 step = (screen_uv - light_uv) * (density / float(num_samples));
    float illumination_decay = 1.0;
    vec3 accum = vec3(0.0);
    for (int i = 0; i < num_samples; i++) {
        sample_uv -= step;
        vec3 samp = texture(screen_texture, sample_uv).rgb;
        float lum = luminance(samp);
        float bright = max(lum - threshold, 0.0) / max(1.0 - threshold, 0.0001);
        accum += samp * bright * illumination_decay * weight;
        illumination_decay *= decay;
    }
    accum *= exposure / float(num_samples);
    accum *= light_color * light_visibility;
    return accum;
}

void fragment() {
    vec3 base_color = texture(screen_texture, SCREEN_UV).rgb;
    vec3 accum = vec3(0.0);

    // Only run a pass when the light actually contributes: below ~0.02 the
    // accumulated ray contribution over num_samples is far below visible
    // brightness, so skipping it avoids a full ray march for a faded source
    // (notably the far side of the sky during twilight).
    if (sun_visibility > 0.02) {
        accum += accumulate_rays(SCREEN_UV, sun_screen_uv, sun_visibility, sun_ray_color);
    }
    if (moon_visibility > 0.02) {
        accum += accumulate_rays(SCREEN_UV, moon_screen_uv, moon_visibility, moon_ray_color);
    }

    COLOR = vec4(base_color + accum, 1.0);
}
