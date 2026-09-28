shader_type canvas_item;
render_mode unshaded;

// Farlands' first WORLD shader: the world's own picture, drawn as an anime frame.
//
// This pass reads the same frame every screen shader reads, and is built the same
// way, so what makes it a different *kind* of thing is where it draws. A world
// pass is put at a negative z_index by shader_overlay.gd, which means it is drawn
// over the 3D world and *under* everything else in the HUD: the frame it reads is
// the world and nothing else, and the hotbar, the health bar and the menus over
// it are drawn back on top of the result untouched. A screen pass - the CRT - is
// a picture of the whole screenful and grades the HUD along with the terrain.
// The registry's `kinds` list says which is which, and the Shaders page groups by
// it; see data/shaders.json.
//
// What "anime" means here is three things, in this order:
//
//   - The colours are flattened - not blurred. A blur takes the texture off a
//     wall and takes the wall's edge with it. Each pixel instead looks at four
//     windows around itself, one per quadrant, and takes the average colour of
//     whichever quadrant is the *flattest* (the filter Kuwahara published in
//     1976, and the reason every "anime filter" is built out of it). In the
//     middle of a patch of grass every quadrant is the same greens and any of
//     them will do; across the edge of a block the windows that straddle the edge
//     have a wide spread and the one that does not is chosen, so the surfaces
//     either side come out flat while the edge between them comes out *sharper*
//     than it went in - at one pixel, which is what ink is anyway.
//   - The shading is banded into a few steps, so a lit face and a shadowed one
//     are two flats rather than a gradient. This is the frame's own shading
//     banded, not the world re-lit: a pass over a finished picture has no normals
//     to light with, and it does not need them, because the light is already in
//     the picture. The step is softened over a third of a band - a hard step
//     through a gradient whose two sides are nearly the same brightness reads as
//     banding rather than as shading.
//   - The shapes are inked in, with a Sobel over the *banded* brightness rather
//     than the raw one. That is the second reason the shading is stepped: the
//     step map is what draws the line, so texture that stays inside a band draws
//     none, and the line lands where the shading had to jump. Fewer steps means
//     a quieter frame; a texture quiet enough for the eye is quiet for the ink
//     too, which is the whole reason flattening comes before the line rather
//     than after it. The ink is multiplied in and not laid on, so it darkens
//     what is under it instead of replacing it with a flat black - real ink
//     lines keep the colour of the shadowed side they are drawn along.
//
// The line is therefore the *picture's* edges and not the world's geometry, and
// that is the honest limit of this pass: a canvas pass over a 3D frame has no
// depth buffer to compare neighbouring pixels in - Godot's canvas built-ins do
// not expose one to a 2D shader - so a wall and a sky of the same brightness are
// one flat region here, and two faces of one block meeting at an angle are one
// unless the lighting puts them in different bands. Where the picture has
// contrast, the line is where an anime frame would put it.

// How much of the flattened colour replaces the frame's own. 1 is the filtered
// picture, 0 is the frame with only the shading and the line applied.
uniform float anime_flatten : hint_range(0.0, 1.0) = 0.8;

// The flattening window's reach in screen pixels. Each quadrant samples
// (radius + 1) squared pixels, so this is the knob that costs: 2 is 36 samples,
// 3 is 64. It is the size of the detail that is treated as texture.
uniform float anime_radius : hint_range(1.0, 3.0, 1.0) = 2.0;

// How many flats the light is divided into. 2 is cel shading proper, 8 is nearly
// the frame itself; it is also how tall a colour has to jump before it gets an
// ink line, so this is the noise the outlines are willing to ignore.
uniform float anime_bands : hint_range(2.0, 8.0, 1.0) = 5.0;

// How much of the frame's own light is replaced by those flats. At 1 the shading
// is the bands and nothing else, at 0 the frame keeps its whole gradient and the
// pass is the flattening and the ink; it exists because how hard a step has to
// land is a matter of taste and of what the frame under it is doing, and a knob
// is a better answer to that than a number chosen here.
uniform float anime_shading : hint_range(0.0, 1.0) = 1.0;

// How dark the ink is drawn, as a multiplier on what is under it. 0 draws no ink.
uniform float anime_outline : hint_range(0.0, 1.0) = 0.55;

// How far away the line is looked for, in screen pixels. Wider lines survive on
// a picture with more texture in it.
uniform float anime_outline_width : hint_range(1.0, 3.0, 1.0) = 1.0;

// Colour punch, applied last, over the whole resulting colour. A cel frame is
// normally painted more saturated than a photograph of the same place.
uniform float anime_saturation : hint_range(0.5, 2.0) = 1.15;

// The frame: what the world has drawn so far, which - at this pass's z_index - is
// the world without the HUD. Nearest, because every sample below is taken at a
// whole number of pixels from the centre and there is nothing to interpolate.
uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_nearest;

// The frame in pixels, pushed here by shader_overlay.gd, which is the only thing
// that knows it: the built-in that would say it (SCREEN_PIXEL_SIZE) is visible in
// the fragment function and not in the functions this pass is built out of, so a
// line drawn "one pixel wide" has to be told what a pixel is.
uniform vec2 frame_size = vec2(1920.0, 1080.0);

// How dark the ink goes where a line is drawn. Not zero: an outline that reaches
// black turns a dark rock face into a silhouette.
const float INK_LEVEL = 0.18;

// A line has to jump this many of the frame's own bands before it is drawn, and
// is fully drawn at the far end. A block edge on a lit face is several bands; the
// soft end of a gradient - the sky - crosses its band over a few pixels and
// never reaches the floor, which is what keeps the sky free of contour lines.
const float INK_FLOOR = 2.0;
const float INK_CEIL = 3.5;

// One screen pixel in UV, which is what every offset below is measured in.
vec2 pixel_step() {
    return 1.0 / max(frame_size, vec2(1.0));
}

// Godot's canvas shaders have no brightness function of their own - the two
// passes that came before this one each bring their own - so this is the same
// three weights the CRT and the god rays work in, and the same constant they
// share. Without it every `luminance` below compiles to "No matching function
// found for: 'luminance'", which names the call and not the missing definition.
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float luminance(vec3 c) {
    return dot(c, LUMA);
}

// The most the shading may move a pixel either way. Banding is the frame's light
// rounded to a few steps, and the rounding is only a look while a step stays a
// step: at the bottom of the scale the flat a pixel rounds *down* to is zero, and
// a colour multiplied by nothing is black. That is not a band, it is a contrast
// control wound up - the first version of this pass did exactly that, and it
// turned every shadow in the picture into a blotch. Bounding the gain is what
// keeps a band a band: the darkest pixel can be taken a third of the way down and
// the brightest a third of the way up, and no further.
const float SHADE_GAIN = 1.3;

// The shading map, and the same map the ink is measured on: the brightness
// quantised into `steps` flats, with the step itself softened over a third of a
// band so that it reads as a step in the light rather than as a contour.
// The lower step is not called `flat`: that is a word the shader language keeps
// (the interpolation qualifier), and a local by that name parses as a type with
// an `=` after it - "Expected an identifier or '[' after type", at the line of
// the declaration, which says nothing about what is actually wrong.
float shade(float brightness, float steps) {
    float scaled = brightness * steps;
    float below = floor(scaled);
    float soft = smoothstep(0.35, 0.65, fract(scaled));
    return (below + soft) / steps;
}

float shaded_at(vec2 uv, float steps) {
    return shade(luminance(texture(screen_texture, uv).rgb), steps);
}

// One quadrant: its average colour in rgb and how far its colours spread in a,
// over the window that starts at this pixel and reaches `radius` pixels one way,
// so the window is (radius + 1) squared samples and this pixel is in all four of
// them. The spread is the variance of the colour across all three channels at
// once (dot products rather than per-channel sums), because two colours that
// differ only in hue separate two surfaces just as well as two that differ in
// brightness - a grey rock against a grey sky, which a luminance test calls one
// surface and the eye does not.
//
// The pair travels as one vec4 rather than through an `out` parameter, which is
// the only return value this file needs.
vec4 quadrant_at(vec2 uv, ivec2 dir, int radius) {
    vec3 total = vec3(0.0);
    float squares = 0.0;
    int count = 0;
    for (int y = 0; y <= 3; y++) {
        if (y > radius) {
            break;
        }
        for (int x = 0; x <= 3; x++) {
            if (x > radius) {
                break;
            }
            vec2 at = uv + vec2(float(x * dir.x), float(y * dir.y)) * pixel_step();
            vec3 colour = texture(screen_texture, at).rgb;
            total += colour;
            squares += dot(colour, colour);
            count += 1;
        }
    }
    float n = float(count);
    vec3 mean = total / n;
    return vec4(mean, max(squares / n - dot(mean, mean), 0.0));
}

// The flattest quadrant's colour: the flattening described at the top.
vec3 flatten_around(vec2 uv, int radius) {
    vec3 best = texture(screen_texture, uv).rgb;
    float flattest = 1000000.0;
    // The four quadrants, one diagonal each. Spelled out rather than derived from
    // the index: a sign taken out of a loop counter is a step the shader compiler
    // has to be trusted with, and this runs on every pixel.
    for (int i = 0; i < 4; i++) {
        ivec2 dir;
        if (i == 0) {
            dir = ivec2(1, 1);
        } else if (i == 1) {
            dir = ivec2(-1, 1);
        } else if (i == 2) {
            dir = ivec2(-1, -1);
        } else {
            dir = ivec2(1, -1);
        }
        vec4 quadrant = quadrant_at(uv, dir, radius);
        if (quadrant.a < flattest) {
            flattest = quadrant.a;
            best = quadrant.rgb;
        }
    }
    return best;
}

// How much of a shape boundary there is here, in bands of the shading: the
// Sobel's magnitude over the step map. A sharp edge between two neighbouring
// flats reaches the full weight of the stencil - four bands - while the soft end
// of a gradient only ever climbs a fraction of one from pixel to pixel.
float edge_here(vec2 uv, float steps) {
    vec2 w = pixel_step() * max(anime_outline_width, 1.0);
    float tl = shaded_at(uv + w * vec2(-1.0, 1.0), steps);
    float tc = shaded_at(uv + w * vec2(0.0, 1.0), steps);
    float tr = shaded_at(uv + w * vec2(1.0, 1.0), steps);
    float ml = shaded_at(uv + w * vec2(-1.0, 0.0), steps);
    float mr = shaded_at(uv + w * vec2(1.0, 0.0), steps);
    float bl = shaded_at(uv + w * vec2(-1.0, -1.0), steps);
    float bc = shaded_at(uv + w * vec2(0.0, -1.0), steps);
    float br = shaded_at(uv + w * vec2(1.0, -1.0), steps);
    float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
    float gy = (tl + 2.0 * tc + tr) - (bl + 2.0 * bc + br);
    return length(vec2(gx, gy)) * steps;
}

void fragment() {
    vec3 frame = texture(screen_texture, UV).rgb;
    float steps = max(floor(anime_bands + 0.5), 2.0);
    int radius = int(anime_radius + 0.5);

    // Flattened, then banded, then inked, then punched: the order is the order
    // the frame is painted in.
    vec3 colour = mix(frame, flatten_around(UV, radius), anime_flatten);
    float brightness = luminance(colour);
    float banded = mix(brightness, shade(brightness, steps), anime_shading);
    colour *= clamp(banded / max(brightness, 0.0001), 1.0 / SHADE_GAIN, SHADE_GAIN);

    float line = smoothstep(INK_FLOOR, INK_CEIL, edge_here(UV, steps)) * anime_outline;
    colour *= mix(1.0, INK_LEVEL, line);

    colour = mix(vec3(luminance(colour)), colour, anime_saturation);
    COLOR = vec4(colour, 1.0);
}
