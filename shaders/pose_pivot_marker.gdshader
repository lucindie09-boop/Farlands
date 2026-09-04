// Debug marker cube for the pose-clone tool: drawn without depth testing so
// pivot points that sit inside the player geometry (shoulders, hips, neck,
// torso center) stay visible through the body.
shader_type spatial;
render_mode unshaded, depth_test_disabled;

uniform vec4 marker_color : source_color = vec4(1.0, 0.15, 0.15, 1.0);

void fragment() {
	ALBEDO = marker_color.rgb;
	ALPHA = marker_color.a;
}
