shader_type spatial;
render_mode unshaded, cull_back, depth_draw_always, depth_test_disabled;

// Minecraft 1.8.8 ItemRenderer & RenderHelper standard item lighting
uniform sampler2D albedo_texture : filter_nearest;

// Canonical Minecraft directional light positions (RenderHelper.java)
const vec3 LIGHT0_POS = vec3(0.2, 1.0, -0.7);
const vec3 LIGHT1_POS = vec3(-0.2, 1.0, 0.7);

void fragment() {
	vec4 tex = texture(albedo_texture, UV);
	
	// DIAGNOSTIC: Output raw texture with no modifications
	// If this still whitens, the issue is NOT in this shader
	ALBEDO = tex.rgb;
	ALPHA = tex.a;
	
	/*
	// Normal in view/camera space
	vec3 N = normalize(NORMAL);
	
	// Transform canonical world light directions into view space
	vec3 l0 = normalize((VIEW_MATRIX * vec4(normalize(LIGHT0_POS), 0.0)).xyz);
	vec3 l1 = normalize((VIEW_MATRIX * vec4(normalize(LIGHT1_POS), 0.0)).xyz);
	
	// Diffuse lighting - start at base brightness (1.0) and only darken
	// Minecraft 1.8: ambient floor 0.4 means max darkness is 0.6 (1.0 - 0.4)
	// When a face is perfectly aligned with a light, it gets full brightness
	// When it's orthogonal/away from lights, it darkens to the ambient floor
	float light0 = max(dot(N, l0), 0.0);
	float light1 = max(dot(N, l1), 0.0);
	
	// Calculate how much darkness to apply: max darkness is 0.6
	// Reduce darkness based on how much light hits the face
	float darkness = 0.6 - (light0 + light1) * 0.3;
	darkness = max(darkness, 0.0); // Never negative (never brighten)
	
	// Apply darkness to base texture
	float shade = 1.0 - darkness;
	
	// Explicitly set albedo with our custom shading, ignoring any scene lighting
	ALBEDO = tex.rgb * shade;
	ALPHA = tex.a;
	
	// Disable any built-in lighting contributions
	METALLIC = 0.0;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	*/
}
