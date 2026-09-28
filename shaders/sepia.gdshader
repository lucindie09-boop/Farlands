shader_type canvas_item;
render_mode unshaded;

// Farlands' sepia: the picture graded to the brown of an old photograph.
//
// Like the invert this is a pass with nothing behind it - one read of the frame,
// one write - so it is as short as a colour change can be. The grade is the
// classic sepia transform: each output channel is a different weighted sum of
// the pixel's own three channels, so a grey lands on a brown ramp - cream where
// the light was, coffee where it was not - and a colour is pulled toward the
// point of that ramp its brightness earned. The weights are the standard ones
// graphics programming has used for twenty years.
//
// The weights on their own are a *weak* grade, and the first version of this
// pass used them alone: their three rows sit close together by design, so the
// picture's own chroma is nearly averaged away and a bright frame came out
// washed tan - a desaturation with a hint of warmth - rather than the brown of
// a photograph. The tone is therefore lifted back off its own grey before it is
// mixed in: the toned pixel is pushed away from its own luminance along its
// existing hue, which deepens the ramp's cream into gold and its tan into
// coffee without inventing a hue the photograph never had.
//
// The amount is a mix, and a partial sepia is the more useful half of the
// slider: a little of it is a gentle warm grade that sits well under other
// effects, while a full sepia is a period piece. Alpha is forced opaque rather
// than taken from the frame, so a transparent region in the stack cannot punch a
// hole in the grade instead of being graded.

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;

uniform float sepia_amount : hint_range(0.0, 1.0) = 1.0;

// The standard sepia weights, one vector per output channel: new.red is the
// pixel dotted with SEPIA_RED, and so on. Written as three dot products rather
// than a mat3 multiply - the same numbers, but every line says which channel it
// is building and there is no column-major order to get wrong.
const vec3 SEPIA_RED = vec3(0.393, 0.769, 0.189);
const vec3 SEPIA_GREEN = vec3(0.349, 0.686, 0.168);
const vec3 SEPIA_BLUE = vec3(0.272, 0.534, 0.131);

// How far the tone is pushed off its own grey: 1.0 is the raw standard matrix
// (the washed version this pass first shipped with), 1.5 is where the ramp
// reads as a photograph rather than as a pale tint.
const float TONE_CHROMA = 1.5;

// Recipient-neutral luma weights, same constants crt.gdshader and
// phosphor.gdshader carry: canvas shaders have no luminance() builtin.
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

void fragment() {
	vec3 here = texture(screen_texture, SCREEN_UV).rgb;
	vec3 toned = vec3(
		dot(here, SEPIA_RED),
		dot(here, SEPIA_GREEN),
		dot(here, SEPIA_BLUE)
	);
	// Off its own grey, along its own hue: the chroma lift that makes the ramp
	// brown. Applied to the toned colour only, so the mix below still crossfades
	// between two fully-formed pictures rather than grading the original twice.
	float tone_luma = dot(toned, LUMA);
	toned = max(tone_luma + (toned - tone_luma) * TONE_CHROMA, vec3(0.0));
	COLOR = vec4(mix(here, toned, sepia_amount), 1.0);
}
