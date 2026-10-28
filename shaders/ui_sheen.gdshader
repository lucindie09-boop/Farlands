shader_type canvas_item;
render_mode blend_add, unshaded;

// Farlands' sheen over a Control's rect: broad, warped diagonals of light sweeping across a
// surface, for a card's frame. It lit the menu's rows for a while and no longer does - a 40-unit
// face takes the whole band, which reads as a pattern rather than as light passing - and a version
// masked to the wordmark's letters was tried and removed too.
//
// The shape is the whole effect, and it is three things together. A band is WIDE - a sheet of light
// rather than a line - and it is the only one on the surface at a time (see below). Its EDGES are
// warped: the axis the band is measured along is pushed in and out by a slow 2-D noise first, so
// neither edge is a straight line and the band leans and narrows as it travels. And its inside is
// MOTTLED - a second, finer noise multiplies its light - so the band reads as a sheet of shimmer
// rather than a painted stripe.
//
// It is added at a low strength: a card's fill and its text are what the eye is reading, and the
// light is something passing along its edge, not a second colour on top. The two tones run from
// deep violet through the body to pale lilac at the brightest part, which is what makes the edge of
// a band read as the edge of a reflection.
//
// ONE band at a time. The period is deliberately longer than the frame it is drawn on (a card's
// frame is a few hundred units around, with the warp on top), so one card can never show two bands
// at once: what it shows is a sheet of light crossing its edge, and dark on either side of it. A
// period short enough to put two on a card reads as a pattern rather than as something passing
// over it, which is the same failure as the fine hatching in a different scale.
//
// Two modes: a FACE (`ring_width` 0), masked to the rounded rectangle the surface actually is, and
// a FRAME (`ring_width` > 0), where the light is confined to that many pixels of border - a card's
// inside carries text, and light over its text is a smudge. Every caller today takes the frame.
//
// The numbers are in interface units (`unit` is the GUI scale in pixels), because the effect is
// part of the interface and must keep its proportions when the window or the scale changes.

uniform vec4 deep : source_color = vec4(0.26, 0.09, 0.50, 1.0);
uniform vec4 pale : source_color = vec4(0.55, 0.33, 0.86, 1.0);
uniform float period = 240.0;      // units along the sweep's axis between bands
uniform float duty = 0.28;         // fraction of the period a band covers
uniform float travel = 120.0;      // units per second along the axis
uniform float tilt = -0.90;        // x per y of the sweep: about 42 degrees
uniform float warp = 36.0;         // units a band's edge is pushed in and out
uniform float grain = 7.0;         // units per cell of the mottling inside a band
uniform float strength = 0.26;
uniform float unit = 2.0;          // pixels per interface unit (the GUI scale)
uniform float phase = 0.0;         // 0..1 of a period: what part of the cycle this surface is at
uniform float ring_width = 0.0;    // 0 = the whole face, > 0 = only that many px of frame
uniform float corner = 5.0;        // px radius of a face's own corners
uniform vec2 size_px = vec2(200.0, 40.0);
uniform float soften = 1.5;        // px of fade at a face's edge


float hash21(vec2 p) {
	vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
	q += dot(q, q.yzx + 33.33);
	return fract((q.x + q.y) * q.z);
}


float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash21(i);
	float b = hash21(i + vec2(1.0, 0.0));
	float c = hash21(i + vec2(0.0, 1.0));
	float d = hash21(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}


float fbm(vec2 p) {
	return 0.66 * value_noise(p) + 0.34 * value_noise(p * 2.03 + 17.0);
}


// One sweep along an axis: the warped band and the mottling inside it. `local` is in pixels.
float sweep(vec2 local, float slope, float speed, float seed) {
	float axis = local.x * slope + local.y;
	float edge = (fbm(local / (26.0 * unit) + seed) - 0.5) * warp * unit;
	float f = fract((axis + edge - TIME * speed * unit) / (period * unit) + phase);
	// A soft rise, a plateau, a soft fall: the band has a body, not a gradient.
	float body = smoothstep(0.0, 0.04, f) * (1.0 - smoothstep(duty, duty + 0.13, f));
	float mottle = 0.60 + 0.40 * fbm(local / (grain * unit) + seed + 9.0);
	return body * mottle;
}


void fragment() {
	// Local pixels from UV, not VERTEX: in this shader's fragment stage VERTEX arrives in the
	// item's parent space, so an offset node reads its own position as distance from the origin
	// and every measurement of its edge is wrong (a card lit face-on, a row lit not at all).
	vec2 local = UV * size_px;

	float light = sweep(local, tilt, travel, 3.1);

	// How far into the surface this pixel is, from its nearest edge.
	float inset = min(min(local.x, size_px.x - local.x), min(local.y, size_px.y - local.y));

	if (ring_width > 0.0) {
		// The frame only: a distance to the nearest edge rather than a second rect, so the light
		// and the bevel cannot disagree about where the border is.
		light *= 1.0 - smoothstep(ring_width - 1.0, ring_width + 1.0, inset);
	} else {
		// The face, rounded to the corners the surface has: a square of light laid over a rounded
		// face pokes out of it at all four corners, and where it pokes it lands on the bevel.
		vec2 q = abs(local - size_px * 0.5) - (size_px * 0.5 - vec2(corner));
		float sd = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - corner;
		light *= smoothstep(0.0, soften, -sd);
	}

	vec3 col = mix(deep.rgb, pale.rgb, clamp(light, 0.0, 1.0));
	COLOR = vec4(col * light * strength, 1.0);
}
