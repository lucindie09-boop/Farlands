shader_type canvas_item;

// Custom screen-space god rays (crepuscular rays).
// Classic radial-scatter technique: for every pixel, walk a straight line
// toward the sun's screen-space position, sampling the already-rendered
// frame along the way. Bright pixels near the sun (mostly sky/sun disc,
// since terrain sits well below the brightness threshold) get smeared
// outward and decayed, producing shafts of light behind whatever is
// occluding the sun (terrain, trees, the player's own geometry, etc).
//
// The march is bounded so the effect reads the same wherever the light sits.
// It stops after max_ray_length instead of always walking the whole way to
// the light, which used to make every pixel farther from the light march
// longer and accumulate more; its sources are the sky around the light
// (source_radius) rather than any bright pixel anywhere; and taps outside the
// frame are rejected instead of being clamped to the border, which used to
// repeat a bright edge texel down every ray when the light was just off
// screen. The accumulation runs continuously up to the light itself: masking
// the light's own disc out of the result punched a dark hole there, which read
// as a dark disc through whatever the light sat behind.
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
// The most screen-space UV a single ray may travel. The unbounded march ran
// all the way to the light, so rays grew longer -- and brighter -- the farther
// the light was from a pixel, and a light near the frame edge washed the
// picture. The near-light look is unchanged under this cap.
uniform float max_ray_length : hint_range(0.05, 1.0) = 0.5;
// How wide the bright source is allowed to be, in screen UV. Only sky near
// the light seeds a ray; bright sky elsewhere is just sky.
uniform float source_radius : hint_range(0.05, 1.5) = 0.7;
uniform vec3 sun_ray_color : source_color = vec3(1.0, 1.0, 1.0);
uniform vec3 moon_ray_color : source_color = vec3(1.0, 1.0, 1.0);

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 accumulate_rays(vec2 screen_uv, vec2 light_uv, float light_visibility, vec3 light_color, vec2 aspect) {
    vec2 to_light = light_uv - screen_uv;
    float light_dist = length(to_light);
    if (light_dist < 0.0001) {
        return vec3(0.0);
    }

    vec2 direction = to_light / light_dist;
    float march_length = min(light_dist * density, max_ray_length);
    float step_length = march_length / float(num_samples);

    vec3 accum = vec3(0.0);
    float illumination_decay = 1.0;
    for (int i = 0; i < num_samples; i++) {
        vec2 sample_uv = screen_uv + direction * (step_length * float(i + 1));

        // Reject off-frame taps. The screen texture is repeat_disable, so an
        // off-frame tap would otherwise return the border texel -- a bright
        // edge repeated all the way down the ray is what used to flash the
        // whole frame when the light sat just outside it.
        float inside = step(0.0, sample_uv.x) * step(sample_uv.x, 1.0)
                     * step(0.0, sample_uv.y) * step(sample_uv.y, 1.0);

        // Only the sky around the light seeds rays. Without this window a long
        // march accumulated ordinary bright sky and tinted the frame.
        float source = 1.0 - smoothstep(source_radius * 0.35, source_radius,
                                        length((sample_uv - light_uv) * aspect));

        vec3 samp = texture(screen_texture, sample_uv).rgb;
        float lum = luminance(samp);
        float bright = max(lum - threshold, 0.0) / max(1.0 - threshold, 0.0001);
        accum += samp * bright * source * inside * illumination_decay * weight;
        illumination_decay *= decay;
    }
    accum *= exposure / float(num_samples);
    accum *= light_color * light_visibility;
    return accum;
}

void fragment() {
    vec3 base_color = texture(screen_texture, SCREEN_UV).rgb;
    vec3 accum = vec3(0.0);

    // Screen UV is normalized per axis; the masks around the light are
    // measured in screen heights so they stay round on any window shape.
    // SCREEN_PIXEL_SIZE is only visible in fragment(), so it is read here and
    // passed down (the same reason anime.gdshader is handed frame_size).
    vec2 aspect = vec2(SCREEN_PIXEL_SIZE.y / SCREEN_PIXEL_SIZE.x, 1.0);

    // Only run a pass when the light actually contributes: below ~0.02 the
    // accumulated ray contribution over num_samples is far below visible
    // brightness, so skipping it avoids a full ray march for a faded source
    // (notably the far side of the sky during twilight).
    if (sun_visibility > 0.02) {
        accum += accumulate_rays(SCREEN_UV, sun_screen_uv, sun_visibility, sun_ray_color, aspect);
    }
    if (moon_visibility > 0.02) {
        accum += accumulate_rays(SCREEN_UV, moon_screen_uv, moon_visibility, moon_ray_color, aspect);
    }

    COLOR = vec4(base_color + accum, 1.0);
}
