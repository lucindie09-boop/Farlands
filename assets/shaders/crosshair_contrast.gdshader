shader_type canvas_item;

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_nearest;

uniform vec4 cross_color : source_color = vec4(1.0);
uniform float cross_opacity : hint_range(0.0, 1.0) = 1.0;
uniform bool cross_contrast = false;
uniform vec4 dot_color : source_color = vec4(1.0);
uniform float dot_opacity : hint_range(0.0, 1.0) = 1.0;
uniform bool dot_contrast = false;

void fragment() {
	vec4 marker = COLOR;
	vec3 col = vec3(0.0);
	float alpha = 0.0;
	if (marker.r > 0.5 && marker.g <= 0.5) {
		if (cross_contrast) {
			vec3 bg = textureLod(screen_texture, SCREEN_UV, 0.0).rgb;
			col = abs(bg - cross_color.rgb);
		} else {
			col = cross_color.rgb;
		}
		alpha = cross_opacity;
	} else if (marker.g > 0.5) {
		if (dot_contrast) {
			vec3 bg = textureLod(screen_texture, SCREEN_UV, 0.0).rgb;
			col = abs(bg - dot_color.rgb);
		} else {
			col = dot_color.rgb;
		}
		alpha = dot_opacity;
	}
	COLOR = vec4(col, alpha);
}
