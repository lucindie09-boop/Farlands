// Noisy Blocks: the block makers' own grain slider, over every block at once.
//
// The two makers paint one texture in the editor and hand it to the world as a
// block's pixels; this include is the same grain applied to the *atlas* the whole
// world is drawn from, live, so one slider reaches every block on screen rather
// than one texture at a time. What it is not is a repaint: nothing is uploaded
// and nothing is regenerated, the world's own fragment stage adds the grain to
// the texel it just sampled. That is also the only shape the effect could have -
// the atlas is shared by every block, so a live per-texel edit of it would be a
// texture upload per slider tick, and the whole world would re-upload to be told
// what one fragment stage adds for free.
//
// The grain is monochromatic, like the makers': the same delta on all three
// channels, so a noisy block is a block with dirt in it rather than a block of
// colour speckle. `block_noise` is the makers' own 0..100 scale and its ceiling
// is the makers' own MAX_GRAIN - 0.35 of full brightness, in block_manager.gd -
// so Noise 100 is the same texture in the world as it is in the block maker's
// preview. `block_noise_on` is the menu's switch, pushed beside it, which is why
// a block whose switch is off is grained by nothing at all rather than by a
// slider that merely reads zero.
//
// The grain is a hash of the *texel* and not of the fragment: a 16x16 face is a
// field of 256 of them, and where a block is close to the camera one texel is
// many screen pixels wide - the grain is then a patch of colour per texel, which
// is exactly what the makers' own noise map is. Further out it is the other way
// round: one texel covers less than a screen pixel, and a field sampled finer
// than its own cells is an aliasing pattern that crawls with the camera, so the
// grain is faded out over that footprint (`fwidth`), which keeps a distant
// hillside from boiling. Where it fades, the atlas is being mip-filtered to a
// flat average anyway, and grain on top of an average is not the makers' grain -
// the fade is a rule about the picture rather than a compromise.
//
// It is included by both of the world's materials, the terrain and the liquids,
// so a shoreline is one grain field and not two; `tile_uv` is the tile space the
// material sampled (the water passes its flowed coordinate, so its grain travels
// with its texture) and `layer` the atlas layer, which is the block kind the
// face belongs to - two kinds of block do not share one field.

uniform float block_noise_on = 0.0;
uniform float block_noise = 0.0;

// One atlas tile, and the makers' own ceiling on the grain. See MAX_GRAIN in
// scripts/block_manager.gd and the tile size in
// src/render/texture_array_generator.cpp: every face is one 16x16 tile.
const float BLOCK_NOISE_TEXELS = 16.0;
const float BLOCK_NOISE_CEILING = 0.35;

// A hash of a texel, per layer. The three inputs are the texel's x, its y and
// the layer, so two texels that differ in any of them land somewhere else. The
// usual multiply-and-fract mix rather than a sine, for the reason the grain pass
// gives (see grain.gdshader): a sine's argument is the thing that runs out of
// bits first, and what it costs is the fine grain the effect is made of.
float block_noise_hash(vec2 texel, float layer) {
	vec3 p = fract(vec3(texel.x + layer * 17.0, texel.y + layer * 31.0, texel.x - texel.y) * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

// The grain this texel's patch is pushed by. Signed, monochromatic, and zero
// when either the switch or the slider is off, so a caller can add it
// unconditionally - and *computed* only when they are on: the test is on
// uniforms, so every fragment of a draw call takes the same side of it, and the
// world's fragment stage is not asked for a hash, a footprint and a fade to end
// up multiplying all three by nothing. An effect that is off costs a comparison.
vec3 block_noise_delta(vec2 tile_uv, float layer) {
	if (block_noise_on < 0.5 || block_noise <= 0.0) {
		return vec3(0.0);
	}

	vec2 texel = floor(tile_uv * BLOCK_NOISE_TEXELS);
	float grain = block_noise_hash(texel, layer) - 0.5;

	// How many texels of the face one screen pixel covers. At one or fewer the
	// grain is drawn as it was authored; over the next doubling it fades out, so
	// by two texels per pixel - a texel half a pixel wide - it is gone rather
	// than aliasing.
	vec2 footprint = fwidth(tile_uv * BLOCK_NOISE_TEXELS);
	float crisp = 1.0 - clamp(max(footprint.x, footprint.y) - 1.0, 0.0, 1.0);

	float amount = block_noise * 0.01 * BLOCK_NOISE_CEILING * crisp;
	return vec3(grain * 2.0 * amount);
}
