// The world's light, for meshes that are lit as OBJECTS.
//
// The terrain bakes light into every vertex (mesh/mesh_builder_faces.cpp): a
// block light RGB and a sky light, both through the mesher's brightness curve
// (kBlockBrightness), which the voxel shader combines with the time of day and
// the shader's own face shading. A held or dropped item is a handful of
// triangles that belongs to no single cell to bake from, so it carries the light
// of the cell it is IN -- ChunkManager.get_light_at, baked into the per-instance
// `item_light` by the two consumers -- and runs the same arithmetic here, on the
// same uniforms the terrain material is fed by EnvironmentController. An item
// can therefore never disagree with the ground it is standing on, at any hour
// and at any depth underground.
//
// This is also why items are drawn UNSHADED: the engine's own sky and ambient
// light know nothing about caves, torches or the day/night uniforms, and shading
// them with it is exactly the mismatch this file exists to remove. What is
// deliberately not here: fog and aerial perspective (an item is at arm's length
// by construction, where the terrain's fog is still zero), vertex AO (an item is
// a convex body in open air -- the face shading below is all the occlusion it
// has), and emissive block textures (a dropped light block is lit by its own
// cell, which it keeps bright).
//
// Included by shaders/item_shader.gdshader (the world's own items, depth-tested)
// and shaders/viewmodel_shader.gdshader (the first-person arm, drawn over the
// world). Both stage functions are defined here so the two shaders differ in
// nothing but their render_mode.

// World lighting, pushed into every item material once a frame by
// EnvironmentController::apply_item_shader_lighting. Same names and meanings as
// the terrain material's own uniforms -- see shaders/voxel_shader.gdshader.
uniform float sky_light_intensity : hint_range(0.0, 1.0) = 1.0;
uniform vec3 sky_light_color = vec3(1.0);
uniform vec3 sky_light_warmth = vec3(1.0);
uniform vec3 darkness_color = vec3(0.0);
uniform float saturation = 1.0;
uniform float contrast = 1.0;
uniform vec3 player_light_position = vec3(0.0);
uniform float player_light_radius = 8.0;
uniform float player_light_intensity = 0.0;
uniform vec3 player_light_color = vec3(1.0);

// The light of the cell this mesh is in: rgb = block light, a = sky light, both
// already through the mesher's brightness curve (ChunkManager.get_light_at).
// Per instance, not per material: every dropped block of one type shares one
// material, and each lies in a cell of its own.
instance uniform vec4 item_light = vec4(0.0, 0.0, 0.0, 1.0);

// How much of the item is left, so a dropped one fades out at the end of its
// life instead of blinking away. Instance-level for the same reason.
instance uniform float item_fade = 1.0;

uniform sampler2D albedo_texture : filter_nearest;
// A sprite item's transparent texels have to be cut away (0.5); a block's
// texture is opaque and keeps every texel (0.0). Clipping rather than blending,
// so a cut texel cannot write depth and hide what is behind it.
uniform float alpha_scissor = 0.0;

varying vec3 item_world_normal;
varying vec3 item_world_position;

void vertex() {
    // World space: an item that has tumbled to a stop shows the face it happens
    // to be showing, and has to be shaded by it exactly as a placed block would.
    item_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
    item_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
    vec4 tex = texture(albedo_texture, UV);
    if (alpha_scissor > 0.0 && tex.a < alpha_scissor) {
        discard;
    }

    vec3 world_normal = normalize(item_world_normal);

    // The terrain's face shade -- a top face full, a bottom half, the four sides
    // in between by axis (voxel_shader.gdshader) -- but read off a normal that
    // TURNS. A terrain face never moves, so its table is exact there; here a
    // tumbling body or a swinging hand sweeps its normals through the whole
    // sphere, and a table with thresholds in it (top above |y| 0.9, then x
    // against z) steps a whole shade the moment a face crosses one -- a flicker
    // on a turning block that has nothing to do with the light arriving on it.
    //
    // The squared components are the weights that keep the terrain's own table
    // exact on the six axes (+/-X 0.6, top 1.0, bottom 0.5, +/-Z 0.8) and slide
    // continuously between them. probes/probe_item_face_shade.gd re-runs this
    // arithmetic over a sweep of normals and reads the rendered frame back.
    float face_shade = 0.60 * world_normal.x * world_normal.x
                     + 0.80 * world_normal.z * world_normal.z
                     + (0.75 + 0.25 * world_normal.y) * world_normal.y * world_normal.y;

    // The sky half of the light, warm at sunrise/sunset and white at noon: the
    // same mix the terrain does, so an item and the ground agree on the colour
    // of the light as well as its amount.
    float sky_brightness = item_light.a * sky_light_intensity;
    vec3 sky_temp = mix(vec3(1.0), sky_light_warmth, sky_light_intensity);
    vec3 combined = max(item_light.rgb, sky_temp * sky_brightness * sky_light_color);

    // The glow that follows the player, on the same falloff the terrain uses.
    float player_falloff = 1.0 - clamp(distance(item_world_position, player_light_position) / max(player_light_radius, 0.001), 0.0, 1.0);
    player_falloff *= player_falloff;
    combined = max(combined, player_light_color * player_falloff * player_light_intensity);

    vec3 lit_color = tex.rgb * combined * face_shade;

    if (sky_light_intensity > 0.1) {
        // Sky bounce off the ground, the terrain's own top-lit ambient.
        float bounce_amount = max(world_normal.y * 0.5 + 0.5, 0.5);
        lit_color += tex.rgb * (vec3(1.0) * sky_light_intensity * 0.15) * bounce_amount;
    }

    // The tinted floor where light cannot reach, then the terrain's finishing:
    // the same texel in the same light has to read the same whether it is lying
    // on the ground, held in the hand, or drawn on the ground behind it.
    float light_amount = clamp(max(max(combined.r, combined.g), combined.b), 0.0, 1.0);
    lit_color += tex.rgb * darkness_color * (1.0 - light_amount);

    lit_color = lit_color * (2.51 * lit_color + 0.03) / (lit_color * (2.43 * lit_color + 0.59) + 0.14);
    lit_color = clamp(lit_color, 0.0, 1.0);
    float luma = dot(lit_color, vec3(0.299, 0.587, 0.114));
    lit_color = mix(vec3(luma), lit_color, saturation);
    lit_color = clamp((lit_color - 0.5) * contrast + 0.5, 0.0, 1.0);

    ALBEDO = lit_color;
    ALPHA = (alpha_scissor > 0.0 ? tex.a : 1.0) * item_fade;
}
