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
//     a far hill wears the grass the near hill wears,
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

// The face's own variation, put back on top of its average.
//
// A cell is 32 to 256 blocks wide and the texture coordinate IS the world
// coordinate (see UV above), so the sampler legitimately lands on a mip that has
// averaged the whole 16x16 face away -- which is why the far field read as one
// flat colour per biome, yellow or green or blue and nothing in between. The
// average is the right colour and it stays the base; what this adds back is the
// face's variation at a fixed world scale, read with the sampler's OWN mip
// selection at that scale and divided by that scale's average, so the mean cannot
// drift (a yellow cell stays exactly as yellow) and the detail fades to nothing on
// its own as the footprint grows: past the distance where one repeat is a pixel,
// the sampler hands back the average on both sides of the division and the ratio
// is 1. Nothing here can alias, and nothing here is a constant to keep in step
// with the shadow of a mip count.
uniform float detail_scale = 16.0;    // world blocks per repeat of the detail
uniform float detail_strength = 0.75; // 0 = the flat average, 1 = the face's own contrast

// The mip that IS the face's average: a face is one 16x16 tile (see
// src/render/texture_array_generator.cpp and BLOCK_NOISE_TEXELS in
// shaders/block_noise.gdshaderinc), so mip 4 is a single texel of it.
const float DETAIL_AVERAGE_MIP = 4.0;

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

void fragment() {
    if (clip_radius > 0.5 && distance(world_xz, clip_center) < clip_radius) {
        discard;
    }

    float layer = UV2.x;
    float is_water = UV2.y;

    vec3 albedo = texture(texture_array, vec3(UV, layer), mipmap_bias).rgb;

    if (detail_strength > 0.0 && detail_scale > 0.0) {
        vec2 detail_uv = UV / detail_scale;
        vec3 fine = texture(texture_array, vec3(detail_uv, layer), mipmap_bias).rgb;
        vec3 average = textureLod(texture_array, vec3(detail_uv, layer), DETAIL_AVERAGE_MIP).rgb;
        vec3 ratio = clamp(fine / max(average, vec3(0.02)), vec3(0.0), vec3(3.0));
        albedo *= mix(vec3(1.0), ratio, detail_strength);
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
