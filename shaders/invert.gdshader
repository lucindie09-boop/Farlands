shader_type canvas_item;
render_mode unshaded;

// Farlands' colour invert: every channel of the picture taken from its opposite,
// so daylight becomes night and a torch's glow becomes a pool of dark.
//
// This is the simplest pass the overlay can build - a screen effect that reads
// the frame once and writes it back changed - and it is written as one because
// there is nothing to sample: no history, no offsets, no screen pixels measured.
// The only question an invert has to answer is *how much*, and that is a mix:
// at zero the picture is itself, at one it is its exact negative, and between
// them it is a washed grade rather than a half-negative, which is what makes a
// partial invert usable as a colour grade and not just an on/off gag.
//
// Alpha is forced opaque rather than taken from the frame: a filter is here to
// grade the whole picture, and reading the frame's own alpha back would let a
// transparent region anywhere in the stack punch a hole in it instead.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;

uniform float invert_amount : hint_range(0.0, 1.0) = 1.0;

void fragment() {
	vec3 here = texture(screen_texture, SCREEN_UV).rgb;
	vec3 inverted = 1.0 - here;
	COLOR = vec4(mix(here, inverted, invert_amount), 1.0);
}
