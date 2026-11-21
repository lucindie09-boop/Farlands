shader_type canvas_item;
render_mode unshaded;

// Farlands' grain: the picture with static laid over it.
//
// This is a screen pass and nothing more - one read of the frame, one write - so
// like the invert and the sepia it needs no history and measures nothing against
// the frame it draws into. What it does have is a *pattern*, and the whole of the
// effect is where that pattern comes from.
//
// The pattern is one hash per cell of the picture, and a cell is `grain_size`
// screen pixels square: the grain is measured in the screen's own pixels rather
// than as a fraction of the frame, for the reason the CRT's raster is (see
// crt.gdshader) - a pattern defined as a fraction of the frame is a different
// grain at every window size, where one screen pixel of grain is one screen
// pixel on every monitor. At the default of one, a cell *is* a pixel, and the
// frame comes back grained pixel by pixel; wider cells read as film rather than
// as static. `frame_size` is pushed here by shader_overlay.gd, which knows the
// viewport this pass is drawn over.
//
// `grain_strength` is how far a cell is pushed from its own colour, added to all
// three channels at once: a grain that moved the channels apart would colour the
// picture, and static is grey.
//
// The grain is *static* by default: the hash is a function of the cell's own
// index, so the pattern is nailed to the screen and the world moves under it.
// With `grain_animated` on, one more number is hashed in - the frame's own step,
// re-rolled `GRAIN_STEPS_PER_SECOND` times a second - and the pattern flickers.
// The step is a whole number rather than the clock itself, so the grain is held
// for a few frames and re-rolled at a fixed rate instead of once per frame: a
// re-roll every frame is a different effect at 30 fps than at 144, and what it
// shows is the frame rate rather than grain. 24 is what film grain is re-rolled
// at, and it is the same reason the camera jitter holds its own steps.
//
// Alpha is forced opaque rather than taken from the frame, like the sepia's: a
// transparent region in the stack must not punch a hole through the grain
// instead of being grained.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_nearest;

// How far a cell is pushed from its own colour, either way: 1.0 is the whole
// range from black to white, and the default is a grain that is plainly there
// and leaves the picture under it readable.
uniform float grain_strength : hint_range(0.0, 1.0) = 0.15;

// One grain cell, in screen pixels.
uniform float grain_size : hint_range(1.0, 4.0) = 1.0;

// Whether the grain is re-rolled as the frames go by, or nailed to the screen.
uniform bool grain_animated = false;

// The frame's own size in pixels, the one thing this shader cannot read out of
// the texture it samples.
uniform vec2 frame_size = vec2(1920.0, 1080.0);

// How often an animated grain is re-rolled. See the header.
const float GRAIN_STEPS_PER_SECOND = 24.0;

// A hash of a whole-numbered cell: the picture's pattern of static. Written as
// the usual multiply-and-fract mix rather than as sin(dot(...)): a sine's
// argument loses its last bits exactly where a wide frame needs them, and the
// grain comes back as soft bands of one value rather than as static. The three
// inputs are the cell's x, its y and their sum, so a cell's neighbours differ in
// all three of them.
float grain_hash(vec2 cell) {
	vec3 p = fract(vec3(cell.x, cell.y, cell.x + cell.y) * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

void fragment() {
	vec3 here = texture(screen_texture, SCREEN_UV).rgb;

	// The cell this fragment is in, and the frame's step when the grain moves.
	// Both are whole numbers, which is what makes the pattern the pattern: the
	// hash is a function of the cell rather than of the exact position within
	// it, so a wider `grain_size` is one shared value per cell instead of a
	// finer grain with holes in it.
	vec2 cell = floor(SCREEN_UV * frame_size / max(grain_size, 1.0));
	float step = 0.0;
	if (grain_animated) {
		step = floor(TIME * GRAIN_STEPS_PER_SECOND);
	}
	// Offsetting by the step rather than hashing the clock separately: the same
	// hash, one more term, and a static grain's step is zero forever.
	float grain = grain_hash(cell + vec2(step * 71.0, step * 113.0)) - 0.5;

	COLOR = vec4(clamp(here + vec3(grain * 2.0 * grain_strength), 0.0, 1.0), 1.0);
}
