shader_type canvas_item;
render_mode unshaded;

// Farlands' phosphor pass: the screen as a tube that keeps glowing after the beam
// has moved on, so everything that moves leaves a fading after-image of itself.
//
// This is a screen pass, and it is the first one that needs something the engine
// does not hand a shader: the *previous* frame. A screen shader reads the frame
// through a copy of it, and that copy is of this frame, taken where the pass
// draws - so a pass looking at the screen sees where things are now and nothing
// about where they were. The overlay supplies the missing half. It keeps a
// viewport whose only content is the main viewport's own texture, which is the
// screen as it stood a moment ago, and hands that to this shader as
// `previous_frame`. So this pass is a function of two pictures: the frame it is
// drawn over, and the frame before it.
//
// What it does with them is a single line, and the choice of that line is the
// whole effect:
//
//   - The picture is the *brighter* of the two, not a blend of them. Light adds,
//     and a phosphor only ever adds: a pixel the beam has left behind holds its
//     brightness and fades from there. A blend would instead pull the frame
//     toward the previous one everywhere, which is not an after-image but a
//     smear - the picture would dim and lose its own edges in a fast turn, and
//     the effect would fight the frame instead of sitting behind it.
//     The same line is also what keeps this pass safe to stack: a brighter-of
//     cannot accumulate, so nothing runs away, and a scene that is standing
//     still is bit-for-bit the scene.
//   - The previous picture is *aged* before it is compared, and that is where the
//     knobs are. It is aged in brightness - multiplied by the retention, so a
//     trail that is a frame old is a little dimmer, one that is two frames old is
//     dimmer again, and the after-image of a fast camera reads as a length rather
//     than as a double exposure. It is aged in space - gathered over a small disc
//     of its own pixels, so the ghost of a moving object is a soft glow rather
//     than a crisp copy of it, and since the ghost is aged again every frame, the
//     old ones are the soft ones: the trail diffuses with age the way light in
//     glass does.
//   - The aging is not the same for every pixel. A tube holds a bright spot for
//     much longer than a dim one, so the brighter the pixel, the closer to the
//     retention it is held; and a pixel that was darker than the burn threshold
//     keeps nothing at all, which is what keeps a cave or a night from smearing
//     into a fog of its own noise. Turning "bright holds longer" off makes the
//     aging uniform, which is a perfectly good look and a very different one: a
//     flat decay with no preference for what the beam was really doing.
//
// It is measured in the screen's own pixels, so a ghost has the same size on any
// window, and `frame_size` is pushed here by shader_overlay.gd because that is
// the one thing a canvas shader cannot read outside `fragment()`.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;

// The frame before this one, as the overlay's history viewport last saw it.
uniform sampler2D previous_frame : repeat_disable, filter_linear;

// Pushed by shader_overlay.gd on every resize: the frame in pixels, which is what
// the ghost's spread is measured in.
uniform vec2 frame_size = vec2(1920.0, 1080.0);

uniform float phosphor_retention : hint_range(0.0, 0.95) = 0.8;
uniform float phosphor_floor : hint_range(0.0, 1.0) = 0.12;
uniform float phosphor_spread : hint_range(0.0, 1.0) = 0.4;
uniform bool phosphor_bright_hold = true;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// How far a ghost diffuses in one frame, in screen pixels, at the top of the
// spread slider: small on purpose, because this happens again every frame and
// the trail is many frames long.
const float SPREAD_PIXELS = 2.5;

// How much of a white pixel's brightness a black one shares in the aging: at 0.5
// the retention is taken to a power between 0.5 and 1, so a white pixel is held
// as the square root of the retention and a black one at the retention itself.
const float HOLD_BAND = 0.5;


float luminance(vec3 colour) {
	return dot(colour, LUMA);
}

// The previous frame, one frame older: gathered, decayed, and gated so that what
// was too dim to light the phosphor keeps none of itself.
vec3 aged_picture(vec2 uv) {
	vec3 sum = texture(previous_frame, uv).rgb;
	float taps = 1.0;
	if (phosphor_spread > 0.0) {
		// `offset` and not `step`: the latter is a built-in function.
		vec2 offset = (SPREAD_PIXELS * phosphor_spread) / max(frame_size, vec2(1.0));
		sum += texture(previous_frame, uv + vec2(offset.x, 0.0)).rgb;
		sum += texture(previous_frame, uv - vec2(offset.x, 0.0)).rgb;
		sum += texture(previous_frame, uv + vec2(0.0, offset.y)).rgb;
		sum += texture(previous_frame, uv - vec2(0.0, offset.y)).rgb;
		taps = 5.0;
	}
	vec3 colour = sum / taps;
	float light = luminance(colour);
	float hold = phosphor_bright_hold ? 1.0 - HOLD_BAND * light : 1.0;
	// A floor of zero is no threshold at all; a smooth ramp rather than a hard
	// cut, because a hard cut across a gradient reads as a contour of the trail.
	float gate = phosphor_floor > 0.0 ? smoothstep(0.0, phosphor_floor, light) : 1.0;
	return colour * pow(phosphor_retention, hold) * gate;
}

void fragment() {
	vec3 here = texture(screen_texture, SCREEN_UV).rgb;
	vec3 held = aged_picture(SCREEN_UV);
	COLOR = vec4(max(here, held), 1.0);
}
