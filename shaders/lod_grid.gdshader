// The seed-grid far mode's material (docs/lod-modes.md): the surface the mode
// samples from the world seed, beyond the loaded world.
//
// It is deliberately much smaller than the terrain shader, because most of what
// that shader does has no input out here: there are no blocks, so no per-vertex
// light, no AO, no emissive layer, no fluid state. What it keeps is what a far
// field still needs to belong to the world it continues:
//
//   * the same sky model (intensity, colour and the sun's warmth), so the horizon
//     goes dark at the same rate the near terrain does,
//   * the same darkness floor and the same saturation/contrast finish,
//   * the same texture array, sampled per vertex at the surface block's layer, so
//     a far hill wears the grass the near hill wears, and blended between TWO layers
//     where two biomes meet -- worldgen mixes the near world's biomes before it picks
//     a surface material, and a far cell has no worldgen behind it, so the mix arrives
//     as a per-corner weight (see lod_surface.hpp) and happens here instead,
//   * and its own fog range, because the world's fog is tuned to the loaded
//     radius and would erase everything this mode exists to draw.
shader_type spatial;
// cull_disabled, not cull_back: the far field is a height field of cells, and a
// cell whose four corner heights form a saddle is not planar -- split along a
// diagonal, one of its two triangles can tilt past the viewer and, with back faces
// culled, vanish. That reads as seeing the terrain behind through a hole in the
// terrain in front, and it happens exactly on the steep cells. Both sides are
// drawn instead and depth decides, which is what a height field wants anyway.
//
// depth_draw_always, and the fragment stage does NOT write ALPHA. Both matter, and
// both are about one thing: this material must write depth. Writing ALPHA sends a
// spatial shader down the TRANSPARENT pipeline, whose contract is no depth write
// (Godot's own note: "shaders going through the transparent pipeline when ALPHA is
// written to may exhibit transparency sorting issues"), and transparent geometry is
// ordered by painter's sort -- per INSTANCE. The far field is one merged mesh per
// spacing level, so there is no instance sort left to order its triangles: with no
// depth write they land in index order, and a ridge drawn after the ridge in front
// of it shows through it. That is what "terrain visible through other terrain"
// was, and depth_draw_always plus an unwritten ALPHA is the fix -- the first makes
// the depth write unconditional, the second keeps the material opaque, where the
// early-Z of the opaque pass is not given up either. The world's terrain shader
// also writes ALPHA = 1.0 and gets away with it only because each chunk is its own
// instance and is sorted.
render_mode unshaded, cull_disabled, depth_draw_always;

uniform sampler2DArray texture_array : source_color, filter_nearest_mipmap;
uniform float mipmap_bias = 0.1;

// The sky the rest of the world is lit by (EnvironmentController pushes these).
uniform float sky_light_intensity = 1.0;
uniform vec3 sky_light_color = vec3(1.0);
uniform vec3 sky_light_warmth = vec3(1.0);

// Colour grading, matching the world's own finishing.
uniform float saturation = 1.0;
uniform float contrast = 1.0;
uniform vec3 darkness_color = vec3(0.0);

// The liquid plane: a flat quad at the water level, tinted rather than textured,
// because an ocean two kilometres out has no waves worth resolving.
uniform vec3 water_color = vec3(0.20, 0.42, 0.78);
uniform float water_mix = 0.55;
// The texture-array layer the liquid quads wear, pushed by LodGrid so the water flag
// above is an exact test rather than a number written into this file (see there).
uniform float water_layer = 32.0;

// The face's own variation, put back on top of its average.
//
// A cell is 32 to 256 blocks wide and the texture coordinate IS the world
// coordinate (see UV above), so the sampler legitimately lands on a mip that has
// averaged the whole 16x16 face away -- which is why the far field read as one
// flat colour per biome, yellow or green or blue and nothing in between. The
// average is the right colour and it stays the base; what this adds back is the
// face's variation at a fixed world scale, read with the sampler's OWN mip
// selection at that scale and divided by that scale's average, so the mean cannot
// drift (a yellow cell stays exactly as yellow).
//
// It does NOT fade itself out, which is what an earlier version of this comment
// claimed and why the far field came back from the field as "z fighting on every
// single face". The two samples only meet when the FINE one reaches
// DETAIL_AVERAGE_MIP, and it gets there only once a face texel -- a sixteenth of
// one repeat, half a block here -- is a single pixel: at 8 blocks a repeat that is a
// block an eighth of a pixel wide, some eight kilometres out. Over the ~2 km band
// before it the ratio is a 2x2-texel pattern of the face divided by the face's own
// average -- an amplitude of tens of percent -- drawn at one or two pixels a repeat,
// and THAT is a pattern the screen cannot carry: the sampler answers with whichever
// mip level its own 2x2 pixel quad landed on, the divisor does not move with it, and
// the difference between the two is a speckle that crawls when the camera moves.
// Which is the report, exactly, and why the fade below exists.
//
// The fade is the world's own answer to the same problem, in the same units: the
// grain in shaders/block_noise.gdshaderinc is faded by its footprint so that "a
// distant hillside" does not boil, and a repeat of the detail term is one repeat of
// a face's 16 texels, so the two rules differ only in which of those they count.
// What is left is drawn as authored: every cell the eye can read a texture on keeps
// the term it has, and what the fade takes away is a band in which the term was
// showing the screen's own sampling rather than the face.
// Eight blocks a repeat: the same 16x16 face the near world wears, one repeat per
// eight blocks, so the far field's blocks read as fine as the near world's instead of
// as eight-block tiles. Halving it was a report from the field ("the tiling is a
// little too big"), and the cost of halving it is paid by the fade rather than by the
// look: the finer the repeat, the sooner a repeat is too narrow on screen to be read,
// so half the tiling also means the term stops at half the distance -- the fade is
// where that trade is settled, not the repeat.
uniform float detail_scale = 8.0;     // world blocks per repeat of the detail
uniform float detail_strength = 0.75; // 0 = the flat average, 1 = the face's own contrast

// The mip that IS the face's average: a face is one 16x16 tile (see
// src/render/texture_array_generator.cpp and BLOCK_NOISE_TEXELS in
// shaders/block_noise.gdshaderinc), so mip 4 is a single texel of it.
const float DETAIL_AVERAGE_MIP = 4.0;

// Where the detail term stops, in pixels per repeat of it (see the fade in
// far_albedo). At and above the full width it is drawn as authored; at and below the
// minimum it is off. The width is what a person sees of one repeat, which is the
// number this is about: at eight pixels a repeat the texels are half a pixel across
// and are still being averaged into something, and under two pixels a repeat there is
// no texel left to average -- the answer is the sampler's own mip choice.
const float DETAIL_REPEAT_MIN_PX = 2.0;
const float DETAIL_REPEAT_FULL_PX = 8.0;

// The far field's own fog: begin at the loaded world's edge, end at the outer
// tile ring. Colour comes from the same day/night curve the world uses.
uniform vec3 fog_color = vec3(0.70, 0.80, 0.95);
uniform float fog_begin = 512.0;
uniform float fog_end = 4096.0;

// The inner boundary of the far field, as a DISC. The loaded world is a disc
// centred on the player; the tiles are a square lattice that does not move, and
// the tiles that straddle the disc's edge are the ones that close the corners the
// disc cannot reach. So the boundary is a fragment test against the same disc the
// world loads, not a hole in the tile set: a tile spanning the edge is built once
// and stays right while the player walks across it. Values come from LodGrid
// (clip_radius 0.0 disables the clip entirely).
uniform vec2 clip_center = vec2(0.0, 0.0);
uniform float clip_radius = 0.0;

// World XZ of this fragment: the mesh is already in world coordinates (the
// instance transform is identity), so in the vertex stage VERTEX is world space.
// The fragment stage's own VERTEX is view space, which is why it cannot be read
// there instead.
varying vec2 world_xz;

void vertex() {
    world_xz = VERTEX.xz;
}

// One layer's colour: the sampler's own average for this footprint, with the face's
// own variation put back on top of it (see detail_scale), as far out as a repeat of it
// is wide enough on screen to be read (see DETAIL_REPEAT_MIN_PX).
vec3 far_albedo(float layer, vec2 uv, vec2 detail_uv) {
    vec3 albedo = texture(texture_array, vec3(uv, layer), mipmap_bias).rgb;
    // The test is on uniforms, so every fragment of the draw takes the same side of it
    // and the samples inside are taken with a well-defined derivative; the fade below
    // is per-pixel and is a multiplier, which is why it is not part of this test.
    if (detail_strength > 0.0 && detail_scale > 0.0) {
        // How many pixels one repeat of the term covers here: its own world scale
        // against the world a pixel covers, which is the world-coordinate UV's own
        // derivative. Read on BOTH axes and the wider of the two is taken -- the same
        // axis the sampler's own mip choice is made on -- so ground seen edge-on,
        // where a repeat is a wide band across the screen and a thin sliver down it,
        // is faded with everything else that the screen cannot carry on one of its two
        // axes rather than textured along the axis that happens to survive.
        vec2 footprint = fwidth(uv);
        float repeat_px = detail_scale / max(max(footprint.x, footprint.y), 1.0e-4);
        float fade = clamp((repeat_px - DETAIL_REPEAT_MIN_PX)
            / max(DETAIL_REPEAT_FULL_PX - DETAIL_REPEAT_MIN_PX, 1.0), 0.0, 1.0);
        vec3 fine = texture(texture_array, vec3(detail_uv, layer), mipmap_bias).rgb;
        vec3 average = textureLod(texture_array, vec3(detail_uv, layer), DETAIL_AVERAGE_MIP).rgb;
        vec3 ratio = clamp(fine / max(average, vec3(0.02)), vec3(0.0), vec3(3.0));
        albedo *= mix(vec3(1.0), ratio, detail_strength * fade);
    }
    return albedo;
}

void fragment() {
    if (clip_radius > 0.5 && distance(world_xz, clip_center) < clip_radius) {
        discard;
    }

    float layer = UV2.x;
    // The liquid plane is the only thing in this mode that wears the LIQUID's own
    // layer, so the layer IS the flag: one comparison, on a value read straight out
    // of the vertex attribute.
    //
    // It used to be the low bit of UV2.y, which carries the blend layer above it
    // (UV2.y = water + 2 * layer2, see lod_grid_upload.cpp). The parity of a packed
    // float is not a flag a shader can trust: measured off a frame
    // (probes/probe_lod_grid_overlap.gd, the ycode/layercode states) the low bit was
    // set on the far field's own TERRAIN -- 95,294 pixels of it, at the terrain's own
    // heights, in a build whose mesh builder flags only its 6,506 sea quads -- so the
    // water tint landed on grass and sand: the report, exactly. `water_mix` off
    // (overlap_nomix) turned every one of those pixels green, which is what named the
    // tint as the thing drawing it.
    //
    // The layer cannot go wrong the same way (the builder hands the liquid plane
    // `water_layer` for both of its layers and a dry cell the biome's surface --
    // lod_surface.cpp), but the VALUE cannot be written down here: the layer is an
    // index into the texture array, and that array is built from the loaded texture
    // pack, where the same block gets a different index. A number in this file is
    // therefore right for one pack and wrong for the next, so LodGrid pushes the one
    // the mesh was built with and the test is exact.
    float is_water = step(abs(layer - water_layer), 0.5);
    float layer2 = floor(UV2.y * 0.5);
    float blend = COLOR.a;

    vec2 detail_uv = UV / detail_scale;
    vec3 albedo = far_albedo(layer, UV, detail_uv);
    // A biome boundary: a ramp across the cell rather than a line along its edge. The
    // second sample is skipped where there is nothing to blend with, and the test is
    // nearly uniform across a cell -- the pair is the cell's, so both of its triangles
    // take the same side of it.
    if (blend > 0.002 && layer2 != layer) {
        albedo = mix(albedo, far_albedo(layer2, UV, detail_uv), blend);
    }

    albedo = mix(albedo, water_color, is_water * water_mix);

    // The sky term, exactly as shaders/item_lighting.gdshaderinc builds it: the
    // sun's warmth fades in with the day, so the two channels stay one model.
    float sky = sky_light_intensity;
    vec3 sky_temp = mix(vec3(1.0), sky_light_warmth, sky);
    vec3 combined = sky_temp * sky * sky_light_color;

    // COLOR.rgb carries the baked face constant from the mesh builder, so a slope
    // is shaded by which way it faces and nothing has to be derived here.
    vec3 lit = albedo * combined * COLOR.rgb;
    lit += albedo * darkness_color * (1.0 - sky);
    lit = clamp(lit, vec3(0.0), vec3(1.0));

    float luma = dot(lit, vec3(0.2126, 0.7152, 0.0722));
    lit = mix(vec3(luma), lit, saturation);
    lit = (lit - 0.5) * contrast + 0.5;

    float distance = length(VERTEX);
    float fade = clamp((distance - fog_begin) / max(fog_end - fog_begin, 1.0), 0.0, 1.0);

    // No ALPHA write: it is what puts a shader in the transparent pipeline and
    // takes the depth write with it (see render_mode above). The far field is
    // opaque -- its water is mixed into ALBEDO rather than blended.
    ALBEDO = mix(lit, fog_color, fade);
}
