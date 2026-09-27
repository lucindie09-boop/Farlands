shader_type canvas_item;
render_mode unshaded;

// Farlands' screen shaders, pass one: a CRT picture.
//
// This is a canvas_item pass on a full-screen rect, so it reads the frame that
// has already been drawn (world first, then whatever the overlay sits above in
// the HUD) and writes the processed picture back. Nothing here comes from a
// public shader collection; it is built from what the hardware actually does,
// and the order below is the order the light takes through a real set:
//
//   - the picture is not a frame. A 240-line set is a picture 240 lines tall,
//     and the frame is showing that picture stretched over the glass. So the
//     frame is first *integrated down* to that raster - each picture cell is
//     the average of the frame under it - and only then painted. This is the
//     single biggest difference between a tube and a filter laid over a sharp
//     image, and it is why the pass has to average rather than pick one frame
//     pixel per cell: point sampling a sharp frame at a coarse grid does not
//     make a coarse picture, it makes a noisy one.
//   - each picture cell is drawn by the beam as a soft round spot, not a hard
//     square. The spots overlap, so a bright cell bleeds into its neighbours,
//     which is what smears thin detail into the blobs a real set shows. The
//     overlap is gathered here and re-normalised, so spreading the picture out
//     never dims it.
//   - the beam is off between one line and the next. That gap is the scanline,
//     and it belongs to the tube rather than to a texture: it is measured on
//     the picture's own grid so it stays put, and it bends with the glass.
//   - the three electron guns never converge perfectly, so red and blue draw
//     their spot slightly off to one side of where green drew it.
//   - the face is an aperture grille: three phosphor stripes per picture dot,
//     with a dark line between them, and a stripe only lets through the one
//     gun whose colour it is. Up close that is where a white highlight gets its
//     colour fringing from.
//   - bright phosphor bleeds into its neighbours, and the glass is a bulging
//     rounded rectangle that is dimmer at its corners than in the middle.
//
// Every pattern below is measured either in the picture's own pixels (the
// raster: cells, lines, stripes) or in frame pixels (the glass: vignette, spot
// radii). frame_size is pushed here by shader_overlay.gd, which knows the
// viewport it is drawing over.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;

// The frame's own size in pixels, and the one thing this shader cannot read out
// of the texture it samples.
uniform vec2 frame_size = vec2(1920.0, 1080.0);

// --- Tube geometry ---
uniform float curvature : hint_range(0.0, 0.6) = 0.22;
uniform float corner_band : hint_range(0.05, 0.9) = 0.5;

// --- The picture's own raster, and the beam that paints it ---
uniform float scanline_count : hint_range(80.0, 900.0) = 180.0;
uniform float scanline_depth : hint_range(0.0, 1.0) = 0.55;
uniform float beam_softness : hint_range(0.15, 1.5) = 0.5;

// --- Phosphor grille ---
uniform float mask_strength : hint_range(0.0, 1.0) = 0.15;
uniform float mask_pitch : hint_range(0.25, 3.0) = 1.0;

// --- Guns and glass ---
uniform float convergence : hint_range(0.0, 0.01) = 0.0025;
uniform float glow : hint_range(0.0, 1.5) = 0.7;
uniform float glow_radius : hint_range(0.3, 3.0) = 1.0;
uniform float vignette : hint_range(0.0, 1.5) = 0.3;

// --- Picture tone ---
uniform float brightness : hint_range(0.5, 1.5) = 1.05;
uniform float contrast : hint_range(0.5, 1.5) = 1.0;
uniform float saturation : hint_range(0.0, 2.0) = 1.1;

// --- Mains hum: the slow bright bar that rolls up a badly shielded set ---
uniform float hum : hint_range(0.0, 0.15) = 0.04;

// TAU, PI and TIME are the shader language's own.
const float HUM_ROLLS_PER_SECOND = 0.12;
const float GLOW_THRESHOLD = 0.45;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// Where the beam's edge falls, as a fraction of the way from the middle of a
// line to the boundary between two of them, and how much of the line it is off
// for. The soft edge ramps over the middle half of the period either side of
// BEAM_EDGE, and a smooth ramp from 0.3 to 0.7 covers half of its own span, so
// the black it takes out of the picture averages exactly BEAM_DUTY. That number
// is only here so the pass can pay the light back and turning the scanlines up
// changes the shape of the picture instead of just dimming it.
const float BEAM_EDGE = 0.5;
const float BEAM_DUTY = 0.5;

// How much of a phosphor stripe is lit, averaged over the stripe: the stripe
// profile below is a raised cosine, whose mean is exactly a half. MASK_GAIN is
// the other half of the same book-keeping - one stripe in three is showing at a
// time, and a stripe shows one gun, so without it the grille would cost the
// picture two thirds of its light. Three would pay all of it back, but that
// leaves every grey of the picture blown out into its three guns; two keeps
// some of the tube's brightness structure and some of its colour.
const float GRILLE_OPEN = 0.5;
const float MASK_GAIN = 2.0;

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

// What a cell of the picture carries, as a frame position: the cell's own
// centre, which is what table below is a table of offsets from.
vec3 cell_at(vec2 cell, vec2 grid, vec2 offset) {
    return texture(screen_texture, clamp((cell + offset) / grid, vec2(0.0), vec2(1.0))).rgb;
}

// The frame averaged over one cell, properly: the cell's own centre and the
// centres of the eight frame pixels around it. In a cell three frame pixels
// wide a third of a cell is exactly one pixel, so those nine taps are the nine
// pixels of the cell and their mean is its mean. That is what the picture needs
// - the frame is full of detail the picture cannot carry, and detail that gets
// sampled anyway does not come out as a coarse picture, it comes out as a
// stipple of whatever the detail happened to be doing at the tap positions.
// This is the one cell per sample that is worth nine taps; the eight around it
// get the cheapest average that covers them, which is four taps a quarter of a
// cell out, a (1/8, 3/4, 1/8) kernel. That one is too sharp to stop the frame's
// own texture aliasing through it, but the cells it is averaging are the ones
// the beam is only letting a tenth of the light through.
vec3 cell_signal(vec2 cell, vec2 grid) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        vec2 quadrant = (vec2(float(i % 2), float(i / 2)) - vec2(0.5)) * 0.5;
        sum += cell_at(cell, grid, quadrant);
    }
    return sum * 0.25;
}

vec3 cell_signal_box(vec2 cell, vec2 grid) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 9; i++) {
        vec2 ninth = (vec2(float(i % 3), float(i / 3)) - vec2(1.0)) * (1.0 / 3.0);
        sum += cell_at(cell, grid, ninth);
    }
    return sum * (1.0 / 9.0);
}

void fragment() {
    vec2 uv = SCREEN_UV;
    vec2 texel = 1.0 / max(frame_size, vec2(1.0));

    // --- Geometry: where on the frame this pixel of glass looks ---
    vec2 off = uv - vec2(0.5);
    float bend = 1.0 + curvature * corner_weight(uv);
    vec2 sample_uv = vec2(0.5) + off * bend;

    // Off the picture means the case. Judged on how far out the sample went so
    // the edge is feather-light instead of a one-pixel staircase, and judged on
    // the same bend that put it there, so the corners it draws and the corners
    // the glass has are the same shape.
    vec2 outside = max(abs(sample_uv - vec2(0.5)) - vec2(0.5), vec2(0.0));
    float lost = max(outside.x, outside.y);
    float glass = 1.0 - smoothstep(0.0, 2.0 * texel.y, lost);

    // --- The raster: the picture's own grid, laid on the glass ---
    // `scanline_count` is how many lines the picture is drawn with, and the
    // frame is showing that picture stretched over it. The horizontal count
    // follows the frame's shape so the picture's cells stay square, which is
    // also why the same setting makes the pixels look wider on a wide window.
    // The whole grid is derived from the warped position, so the raster bends
    // with the glass instead of sliding underneath it.
    float aspect = frame_size.x / max(frame_size.y, 1.0);
    vec2 grid = vec2(max(scanline_count * aspect, 1.0), max(scanline_count, 1.0));
    vec2 video = sample_uv * grid;
    vec2 cell = floor(video) + 0.5;   // the picture cell this sample lands in
    vec2 rel = video - cell;          // where in it: -0.5 at its edges, 0 at its middle

    // --- The beam: this cell's spot and its neighbours' spots, overlapping ---
    // A Gaussian spot of `beam_softness` cells is gathered over the cell and its
    // 3x3 neighbourhood: its own spot keeps most of the weight, the neighbours
    // bleed in, and the sum of the weights divides the result back out, so the
    // picture is spread into blobs without getting darker or brighter.
    //
    // The guns' convergence is free here: moving the picture under the beam is
    // what a spot that lands off-centre does, so red and blue are the same
    // gather with their weights shifted to one side rather than a second set of
    // samples. `gun` is in cells, and a cell is a fraction of the glass, so the
    // shift works out to a fraction of the frame's width.
    //
    // The spot has a floor as well as a ceiling: a Gaussian so narrow that every
    // weight underflows to zero still sums to zero, and a gather divided by zero
    // is a black screen. At a sixth of a cell the spot is already sharper than
    // any set's, and its farthest weight is small rather than nothing.
    //
    // The same nine cells are gathered a second time for the halo, out of the
    // cells themselves rather than out of frame pixels. Phosphor bleeds into the
    // phosphor next to it, and its neighbours are cells, so a halo measured in
    // cells is the one grain of light a tube actually has. It also cannot ring:
    // a handful of taps on a circle draw the circle into the picture wherever
    // they cross a gradient, and a wider halo (which is what a ring of taps was
    // wanted for) is not worth drawing rings around every lit thing to get.
    float softness = max(beam_softness, 0.15);
    vec2 spot = vec2(softness, softness * 0.8);
    vec2 inv_spot = 1.0 / (2.0 * spot * spot);
    float gun = convergence * grid.x;
    float halo_falloff = 1.0 / (2.0 * max(glow_radius, 0.05) * max(glow_radius, 0.05));
    vec3 picture = vec3(0.0);
    vec3 weight_sum = vec3(0.0);
    vec3 hot = vec3(0.0);
    float hot_weight = 0.0;
    vec3 middle_signal = cell_signal_box(cell, grid);
    for (int i = 0; i < 9; i++) {
        vec2 step = vec2(float(i % 3) - 1.0, float(i / 3) - 1.0);
        vec3 signal = middle_signal;
        if (i != 4) {
            signal = cell_signal(cell + step, grid);
        }
        vec2 d = rel - step;
        vec3 dx = vec3(d.x - gun, d.x, d.x + gun);
        vec3 w = exp(-(dx * dx) * inv_spot.x - (d.y * d.y) * inv_spot.y);
        picture += signal * w;
        weight_sum += w;
        // Only the light above the tube's glow threshold spills, so dark rock
        // does not smear into its neighbours.
        float spill = exp(-dot(step, step) * halo_falloff);
        hot += signal * max(luminance(signal) - GLOW_THRESHOLD, 0.0) * spill;
        hot_weight += spill;
    }
    picture /= max(weight_sum, vec3(0.000001));
    picture += hot / max(hot_weight, 0.000001) * glow;

    // --- The beam is off between the lines there, so the gaps stay dark ---
    // The gaps take light out of the picture, so the lines are paid back what
    // the gaps cost: the scanlines are meant to change the shape of the
    // picture, and a set doing that does not also get dimmer.
    float across = abs(rel.y) * 2.0;   // 0 along a line, 1 between two of them
    float off_line = smoothstep(BEAM_EDGE - 0.2, BEAM_EDGE + 0.2, across);
    float beam = (1.0 - scanline_depth * off_line) / (1.0 - scanline_depth * BEAM_DUTY);

    // --- Face: the grille, and the one gun each stripe shows ---
    // One picture dot is one phosphor triad: three stripes inside the width of a
    // single cell, each of them painted by one gun. That is the whole point of
    // the mask being here - at a triad per cell the dark lines and the colour
    // fringing land *inside* a picture cell, so the colour is carried by the
    // cell's own three subpixels instead of three neighbouring cells each
    // showing one gun, which is what turns a picture into confetti.
    //
    // `mask_pitch` is the triad's width in picture pixels, so the grille scales
    // with the picture rather than with the window: past 1 it is coarser than
    // the picture and starts to look like a shadow mask held up to it. The
    // raised cosine is the stripe's own brightness - dark at both its edges,
    // which is the dark line between two phosphors, brightest in its middle.
    float slot = fract(video.x / mask_pitch) * 3.0;
    float index = floor(slot);
    float across_stripe = fract(slot);
    float stripe_lit = 0.5 - 0.5 * cos(TAU * across_stripe);
    vec3 grille = clamp(vec3(1.0) - abs(vec3(index) - vec3(0.0, 1.0, 2.0)), 0.0, 1.0);
    vec3 face = mix(vec3(1.0), grille * (stripe_lit / GRILLE_OPEN) * MASK_GAIN, mask_strength);

    // --- Glass: dimmer towards its corners, and a bar rolling up it ---
    // Measured on the glass rather than on the picture, so it stays put at the
    // frame's corners however hard the picture is bent onto them.
    float radius = clamp(length(abs(uv - vec2(0.5)) * 2.0) * 0.72, 0.0, 1.0);
    float corner_shade = 1.0 - vignette * radius * radius;
    float roll = 1.0 + hum * sin(TAU * (uv.y - TIME * HUM_ROLLS_PER_SECOND));

    // --- Picture tone, in the order a tube's own controls are wired ---
    // The contrast control turns about a dark grey rather than about mid grey:
    // a game frame is mostly dark, and a pivot at half brightness takes a third
    // of the light out of it before the picture is even looked at.
    vec3 col = max(picture, vec3(0.0)) * beam * face;
    col *= corner_shade * roll * brightness;
    col = (col - vec3(0.2)) * contrast + vec3(0.2);
    col = mix(vec3(luminance(col)) * vec3(1.0), col, saturation);

    // Outside the glass is the case, and the case is black.
    COLOR = vec4(max(col, vec3(0.0)) * glass, 1.0);
}
