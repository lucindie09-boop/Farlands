shader_type spatial;
render_mode unshaded, blend_mix, cull_back;

// Texel-grid overlay for the block maker preview: draws a box around every
// block pixel. The block texture is a 16x16 texture, so one cell = 1/16 of the
// UV range.

uniform vec4 grid_color : source_color = vec4(0.0, 0.0, 0.0, 0.9);
uniform float cells : hint_range(4.0, 256.0) = 16.0;
uniform float line_thickness = 1.5; // in screen pixels

void fragment() {
	vec2 uv = UV * cells;
	// Signed distance from the nearest texel edge, in texel units.
	// fract(uv - 0.5) centres the tile so 0.0 lands exactly on a grid line.
	vec2 d = abs(fract(uv - vec2(0.5)) - vec2(0.5));
	// Line width in texel units derived from the screen-space derivative, so
	// the grid stays ~line_thickness pixels thick regardless of face scale.
	vec2 w = fwidth(uv) * line_thickness;
	w = max(w, vec2(0.0001));
	// 1.0 on the line centre, 0.0 once we're more than w away.
	vec2 a = 1.0 - smoothstep(vec2(0.0), w, d);
	ALBEDO = grid_color.rgb;
	ALPHA = grid_color.a * max(a.x, a.y);
}
