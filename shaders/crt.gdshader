shader_type canvas_item;
render_mode unshaded;

// Farlands' screen shaders, pass one: a CRT picture.
//
// This is a canvas_item pass on a full-screen rect, so it reads the frame that
// has already been drawn (world first, then whatever the overlay sits above in
// the HUD) and writes the processed picture back. Nothing here comes from a
// public shader collection; it is built from what the hardware actually does:
//
//   - the face of the tube is a rounded rectangle, and the picture on it bulges,
//     so the sample coordinate is pushed outwards near the corners until it
//     leaves the frame, which is what draws the dark corners and the black
//     surround beyond them
//   - the three electron guns never converge perfectly, so red and blue are
//     sampled a hair further out and a hair further in than green
//   - the beam paints one bright line per scan, with a dark gap between lines
//   - the face is a grid of RGB phosphor triads, not one continuous emitter
//   - bright phosphor bleeds into its neighbours, and the glass is dimmer at its
//     corners than in the middle
//
// Every pattern below is measured in FRAME pixels (not screen UV), so the
// picture's structure is the same at every window size; frame_size is pushed
// here by shader_overlay.gd, which knows the viewport it is drawing over.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;

// The frame's own size in pixels, and the one thing this shader cannot read out
// of the texture it samples.
uniform vec2 frame_size = vec2(1920.0, 1080.0);

// --- Tube geometry ---
uniform float curvature : hint_range(0.0, 0.6) = 0.15;
uniform float corner_band : hint_range(0.05, 0.9) = 0.5;

// --- Beam structure ---
uniform float scanline_count : hint_range(80.0, 1200.0) = 360.0;
uniform float scanline_depth : hint_range(0.0, 1.0) = 0.30;

// --- Phosphor face ---
uniform float mask_strength : hint_range(0.0, 1.0) = 0.22;
uniform float mask_pitch : hint_range(0.5, 6.0) = 1.5;

// --- Guns and glass ---
uniform float convergence : hint_range(0.0, 0.01) = 0.0025;
uniform float glow : hint_range(0.0, 1.5) = 0.30;
uniform float glow_radius : hint_range(1.0, 12.0) = 3.0;
uniform float vignette : hint_range(0.0, 1.5) = 0.35;

// --- Picture tone ---
uniform float brightness : hint_range(0.5, 1.5) = 1.0;
uniform float contrast : hint_range(0.5, 1.5) = 1.0;
uniform float saturation : hint_range(0.0, 2.0) = 1.0;

// --- Mains hum: the slow bright bar that rolls up a badly shielded set ---
uniform float hum : hint_range(0.0, 0.15) = 0.04;

// TAU and PI are the shader language's own constants.
const float HUM_ROLLS_PER_SECOND = 0.12;
const float GLOW_THRESHOLD = 0.45;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float luminance(vec3 c) {
    return dot(c, LUMA);
}

// How "into the corner" a pixel is: 0 anywhere along the tube's four midlines
// (and everywhere in the middle), 1 in a corner. The tube's picture only bends
// and only falls off the glass out here, which is what keeps the picture
// reaching the bezel in the middle of every edge the way a real face does.
//
// Both axis factors are smoothsteps rather than a product of raw coordinates:
// a product has an infinite derivative on the midlines, and the crease that
// leaves is visible as a fold straight down the middle of the picture.
float corner_weight(vec2 uv) {
    vec2 rel = abs(uv - vec2(0.5)) * 2.0;
    vec2 edge = smoothstep(vec2(1.0 - corner_band), vec2(1.0), rel);
    return edge.x * edge.y;
}

// The bright part of one tap, for the phosphor bleed.
vec3 bleed_tap(vec2 uv, vec2 offset) {
    vec3 tap = texture(screen_texture, clamp(uv + offset, vec2(0.0), vec2(1.0))).rgb;
    float hot = max(luminance(tap) - GLOW_THRESHOLD, 0.0);
    return tap * hot;
}

// Two rings of taps around the sample: a tight halo and a wide spill from the
// same phosphor. Only the part of each tapped pixel that is above the tube's
// glow threshold takes part, so dark rock does not smear.
vec3 phosphor_bleed(vec2 uv, vec2 texel) {
    vec3 spill = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        float ring = 1.0 + float(i / 6);
        float angle = float(i % 6) * (TAU / 6.0) + ring * 0.7;
        vec2 offset = vec2(cos(angle), sin(angle)) * glow_radius * ring * texel;
        spill += bleed_tap(uv, offset) / ring;
    }
    return spill * (glow / 9.0);
}

void fragment() {
    vec2 uv = SCREEN_UV;
    vec2 texel = 1.0 / max(frame_size, vec2(1.0));

    // --- Geometry: where on the frame this pixel of glass looks ---
    vec2 off = uv - vec2(0.5);
    float bend = 1.0 + curvature * corner_weight(uv);
    vec2 sample_uv = vec2(0.5) + off * bend;

    // Off the picture means the bezel. Judged on how far out the sample went so
    // the edge is feather-light instead of a one-pixel staircase, and judged on
    // the same bend that put it there, so the corners it draws and the corners
    // the glass has are the same shape.
    vec2 outside = max(abs(sample_uv - vec2(0.5)) - vec2(0.5), vec2(0.0));
    float lost = max(outside.x, outside.y);
    float glass = 1.0 - smoothstep(0.0, 2.0 * texel.y, lost);

    // --- Guns: red and blue land off-centre, more so the further from the middle ---
    vec2 sample_r = vec2(0.5) + off * bend * (1.0 + convergence);
    vec2 sample_b = vec2(0.5) + off * bend * (1.0 - convergence);
    vec3 picture;
    picture.r = texture(screen_texture, clamp(sample_r, vec2(0.0), vec2(1.0))).r;
    picture.g = texture(screen_texture, clamp(sample_uv, vec2(0.0), vec2(1.0))).g;
    picture.b = texture(screen_texture, clamp(sample_b, vec2(0.0), vec2(1.0))).b;

    // --- Phosphor bleed, before the face's own structure cuts the picture up ---
    if (glow > 0.001) {
        picture += phosphor_bleed(clamp(sample_uv, vec2(0.0), vec2(1.0)), texel);
    }

    // --- Beam: one bright line per scan, dark between ---
    float gap = abs(sin(uv.y * scanline_count * PI));
    float beam = 1.0 - scanline_depth * gap;
    // The gaps take light away, so give the lines back what they lost: without
    // this, turning the scanlines up just darkens the picture. The beam's own
    // gap averages 2/pi over a scan, so paying back exactly that much leaves the
    // picture as bright as it was and the extra brightness on the lines.
    beam *= 1.0 + scanline_depth * (2.0 / PI);

    // --- Face: RGB triads, a mask_pitch-pixel stripe per gun ---
    float triad = uv.x * frame_size.x / (3.0 * mask_pitch);
    vec3 face = 0.5 + 0.5 * cos(TAU * (vec3(triad) - vec3(0.0, 1.0 / 3.0, 2.0 / 3.0)));
    // A triad is dark between its three stripes, so scale it back up before
    // mixing: the mask is meant to colour the light, not eat it.
    face = mix(vec3(1.0), clamp(face * 2.0, 0.0, 1.0), mask_strength);

    // --- Glass: dimmer towards its corners, and a bar rolling up it ---
    float radius = clamp(length(abs(uv - vec2(0.5)) * 2.0) * 0.72, 0.0, 1.0);
    float corner_shade = 1.0 - vignette * radius * radius;
    float roll = 1.0 + hum * sin(TAU * (uv.y - TIME * HUM_ROLLS_PER_SECOND));

    // --- Picture tone, in the order a tube's own controls are wired ---
    vec3 col = max(picture, vec3(0.0)) * beam * face;
    col *= corner_shade * roll * brightness;
    col = (col - vec3(0.5)) * contrast + vec3(0.5);
    col = mix(vec3(luminance(col)) * vec3(1.0), col, saturation);

    // Outside the glass is the case, and the case is black.
    COLOR = vec4(max(col, vec3(0.0)) * glass, 1.0);
}
