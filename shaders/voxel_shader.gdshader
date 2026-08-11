shader_type spatial;
render_mode unshaded, cull_back, depth_draw_always;

uniform sampler2DArray texture_array : filter_nearest_mipmap;
uniform sampler2DArray emissive_array : filter_nearest_mipmap;

uniform float mipmap_bias = 0.1;

// Lighting
uniform float sky_light_intensity : hint_range(0.0,1.0)=1.0;
uniform vec3 sky_light_color=vec3(1.0);
uniform vec3 sun_direction=vec3(0.0,1.0,0.0);

// Player light
uniform vec3 player_light_position=vec3(0.0);
uniform float player_light_radius=8.0;
uniform float player_light_intensity=1.0;
uniform vec3 player_light_color=vec3(1.0,1.0,1.0);

// Fog
uniform float fog_begin=64.0;
uniform float fog_end=896.0;
uniform float fog_density=0.6;
uniform float height_fog_density=0.012;
uniform float sea_level=64.0;
uniform int fog_mode=1; // 0=disabled, 1=edge, 2=linear, 3=exponential

// Color grading
uniform float saturation=1.0;
uniform float contrast=1.0;

// AO tint & darkness floor
uniform vec3 ao_color=vec3(0.0);
uniform float ao_strength=1.0;
uniform vec3 darkness_color=vec3(0.0);

// Atmosphere
uniform float fog_scatter=0.0;
uniform vec3 fog_scatter_color=vec3(1.0,1.0,1.0);

uniform vec3 aerial_color=vec3(1.0,1.0,1.0);
uniform float aerial_strength=0.35;

uniform vec3 fog_color=vec3(1.0,1.0,1.0);

uniform vec3 sky_light_warmth=vec3(1.0,1.0,1.0);

uniform vec3 horizon_color=vec3(1.0,1.0,1.0);
uniform vec3 zenith_color=vec3(1.0,1.0,1.0);
uniform float sky_turbidity=0.35;

varying vec3 world_position;
varying vec3 world_normal;
varying vec3 view_direction;
varying float fog_distance;
varying vec4 custom0_attr;
varying vec4 custom1_attr;
varying vec4 custom2_attr;
vec3 decode_normal(float encoded) {
    int dir = int(encoded * 255.0 + 0.5);
    vec3 normals[6] = vec3[](
        vec3(0, 1, 0),
        vec3(0, -1, 0),
        vec3(1, 0, 0),
        vec3(-1, 0, 0),
        vec3(0, 0, 1),
        vec3(0, 0, -1)
    );
    return normals[dir];
}

void vertex(){

    world_position=(MODEL_MATRIX*vec4(VERTEX,1.0)).xyz;
    world_normal=normalize((MODEL_MATRIX*vec4(decode_normal(CUSTOM1.b),0.0)).xyz);

    vec3 camera_position=INV_VIEW_MATRIX[3].xyz;

    view_direction=normalize(world_position-camera_position);

    fog_distance=distance(world_position,camera_position);
    custom0_attr = CUSTOM0;
    custom1_attr = CUSTOM1;
    custom2_attr = CUSTOM2;
}

void fragment(){

    float layer=custom1_attr.r * 255.0;
    vec3 tex_coord = vec3(custom2_attr.xy, layer);
    vec4 color = texture(texture_array, tex_coord, mipmap_bias);

    float emissive_layer = custom1_attr.a * 255.0;
    vec3 emissive_color = texture(emissive_array, vec3(custom2_attr.xy, emissive_layer)).rgb;

    float raw_ao=custom1_attr.g;
    // Smooth non-linear curved AO to reduce diagonal triangulation seams
    float smooth_ao = pow(raw_ao, 1.35);
    float ao = mix(smooth_ao, raw_ao, 0.25);
    
    // Apply face-direction shade (static directional darkening)
    // This is applied after the AO curve to avoid double-processing
    float face_shade = 1.0;
    if (abs(world_normal.y) > 0.9) {
        face_shade = world_normal.y > 0.0 ? 1.0 : 0.5;  // Top vs Bottom
    } else {
        // Sides: distinguish North/South (Front/Back) from East/West (Left/Right)
        // This matches the C++ get_face_shade values
        float abs_x = abs(world_normal.x);
        float abs_z = abs(world_normal.z);
        if (abs_x > abs_z) {
            face_shade = 0.6;   // East/West (Left/Right)
        } else {
            face_shade = 0.8;   // North/South (Front/Back)
        }
    }
    ao *= face_shade;

    // AO colour tint: fully-occluded faces darken toward ao_color instead of black
    float ao_amount = clamp((1.0 - ao) * ao_strength, 0.0, 1.0);
    vec3 ao_tint = mix(ao_color, vec3(1.0), 1.0 - ao_amount);

    vec3 block_light=custom0_attr.rgb;

    vec3 sun_dir_norm = normalize(sun_direction);

    float skyBrightness=
        custom0_attr.a*
        sky_light_intensity;

    vec3 skyTemp=
        mix(vec3(1.0,1.0,1.0),
            sky_light_warmth,
            sky_light_intensity);

    vec3 skyContribution=
        skyTemp*
        skyBrightness*
        sky_light_color;

    vec3 combinedLight=max(block_light,skyContribution);

    float playerDist=
        distance(world_position,player_light_position);

    float playerLight=
        1.0-clamp(playerDist/player_light_radius,0.0,1.0);

    playerLight*=playerLight;

    combinedLight=max(
        combinedLight,
        player_light_color*
        playerLight*
        player_light_intensity
    );

    vec3 lit_color=color.rgb*combinedLight*ao_tint;

    if(sky_light_intensity>0.1){

    vec3 bounce=
        vec3(1.0,1.0,1.0)*
        sky_light_intensity*
        0.15;

    float bounceAmount=
        world_normal.y*0.5+0.5;
    bounceAmount=max(bounceAmount,0.5);

        lit_color+=
            color.rgb*
            bounce*
            bounceAmount*
            ao_tint;
    }

    // Darkness colour: tinted minimum light where light cannot reach
    float light_amount = clamp(max(max(combinedLight.r, combinedLight.g), combinedLight.b), 0.0, 1.0);
    lit_color += color.rgb * darkness_color * (1.0 - light_amount);

    // ========= FOG =========
	float fog_amount = 0.0;
	float dist_factor = 0.0;

	if (fog_mode > 0 && fog_density > 0.001) {
    	float dist = max(fog_distance - fog_begin, 0.0);
    	float fog_range = max(fog_end - fog_begin, 1.0);
    	
    	if (fog_mode == 1) {
    		// Edge fog - linear starting 50 blocks before end
    		float edge_start = fog_end - 50.0;
    		float edge_range = max(fog_end - edge_start, 1.0);
    		float edge_dist = max(fog_distance - edge_start, 0.0);
    		float x = clamp(edge_dist / edge_range, 0.0, 1.0);
    		fog_amount = x;
    		dist_factor = smoothstep(edge_start, fog_end, fog_distance);
    	} else if (fog_mode == 2) {
    		// Linear fog - Minecraft-style, only applies near the end
    		float x = clamp(dist / fog_range, 0.0, 1.0);
    		fog_amount = x;
    		dist_factor = smoothstep(fog_begin, fog_end, fog_distance);
    	} else if (fog_mode == 3) {
    		// Exponential fog - current setup
    		float x = clamp(dist / fog_range, 0.0, 1.0);
    		fog_amount = x * sqrt(x);
    		fog_amount *= fog_density * 1.5;
    		dist_factor = smoothstep(fog_begin, fog_end, fog_distance);
    	} else {
    		// Unknown mode - fail safe
    		fog_amount = 0.0;
    		dist_factor = 0.0;
    	}
    	
    	fog_amount = clamp(fog_amount, 0.0, 1.0);
    	
    	// Apply height fog and horizon fog only when fog is enabled
    	float belowSea = max(sea_level - world_position.y, 0.0);
    	float hf = belowSea * height_fog_density;
    	float heightFog = hf / (1.0 + hf);
    	heightFog *= dist_factor;
    	fog_amount += heightFog * 0.5;

    	float horizon = abs(view_direction.y);
    	float h3 = (1.0 - horizon);
    	h3 = h3 * h3 * h3;
    	fog_amount += h3 * 0.18 * dist_factor;
    	fog_amount = clamp(fog_amount, 0.0, 1.0);

    	// ========= TURBIDITY (Rayleigh/Mie haze) =========
    	float haze = clamp(sky_turbidity * 1.4 - 0.2, 0.0, 1.0);
    	haze *= smoothstep(0.0, 0.35, sky_light_intensity);

    	// ========= SUN SCATTERING =========
    	vec3 view_norm = normalize(view_direction);
    	float sunDot = max(dot(view_norm, sun_dir_norm), 0.0);
    	float sd2 = sunDot * sunDot;
    	float sd4 = sd2 * sd2;
    	float sd8 = sd4 * sd4;
    	float sd16 = sd8 * sd8;
    	float sd64 = sd16 * sd16;
    	sd64 = sd64 * sd64;
    	float sunGlow = sd4 * 0.25 + sd16 + sd64 * 0.4;

    	vec3 scatter = fog_scatter_color * sunGlow * fog_scatter * sky_light_intensity;
    	scatter *= dist_factor * (1.0 + haze * 0.4);

    	// Horizon + zenith haze modulation
    	vec3 haze_color = mix(vec3(1.0, 1.0, 1.0), fog_scatter_color, 0.3);
    	vec3 turb_horizon = mix(fog_color, haze_color, haze * 0.55);

    	// Zenith desaturation: aerosols overwhelm Rayleigh, sky pales overhead
    	float z_luma = dot(zenith_color, vec3(0.299, 0.587, 0.114));
    	vec3 pale_zenith = mix(zenith_color, vec3(z_luma) * vec3(1.0, 1.0, 1.0), haze * 0.60);

    	// ========= SKY GRADIENT =========
    	float up2 = max(view_direction.y, 0.0);
    	float skyBlend = 1.0 - (1.0 - up2) * sqrt(1.0 - up2);
    	vec3 finalFogColor = fog_color;

    	float sun_elevation = sun_dir_norm.y;
    	float sun_vis = smoothstep(-0.08, 0.08, sun_elevation);

    	// Sun glow widens with turbidity (wider Mie forward lobe)
    	float sun_glow_width = mix(1.0, 2.2, haze);
    	float sd2w = sd2;
    	float sd4w = sd4;
    	for (int i = 0; i < 2; i++) { sd2w = sd2w * sd2; sd4w = sd4w * sd4; }
    	float sun_glow_turb = sd4 * 0.25 + sd2w * 0.15 + sd4w * 0.08;

    	vec3 warmFog = mix(fog_color, fog_scatter_color, 0.05);
    	finalFogColor = mix(finalFogColor, warmFog, sun_glow_turb * sun_vis * sun_glow_width * 0.2);
    	finalFogColor += scatter * (1.0 - fog_amount) * 0.3;

    	// Apply sky gradient with haze
    	vec3 skyColor = mix(finalFogColor, pale_zenith, skyBlend);
    	fog_color = mix(fog_color, skyColor, 0.3);
    	fog_color = mix(fog_color, turb_horizon, (1.0 - skyBlend) * haze * 0.7);
	} else {
    	// When fog is disabled, set defaults for variables used later
    	float haze = 0.0;
    	vec3 scatter = vec3(0.0);
    	vec3 finalFogColor = fog_color;
    	float skyBlend = 0.0;
    	vec3 pale_zenith = zenith_color;
    	vec3 turb_horizon = fog_color;
	}

    // ========= AERIAL PERSPECTIVE =========
    float aerial = fog_amount * fog_amount * aerial_strength;
    float luma = dot(lit_color, vec3(0.299, 0.587, 0.114));
    float tintStrength = aerial * (1.0 - luma * 0.6);
    float facingSky = clamp(world_normal.y * 0.5 + 0.5, 0.0, 1.0);
    tintStrength *= mix(1.2, 0.8, facingSky);
    vec3 sceneColor = mix(lit_color, finalFogColor, tintStrength);
    sceneColor += vec3(0.0, 0.0, 0.0) * fog_amount * (1.0 - sky_light_intensity);

    // ========= FINAL BLEND =========
    vec3 final_color = mix(sceneColor, finalFogColor, fog_amount);
    final_color += scatter * fog_amount * 0.20;

    float desat = smoothstep(0.0, 1.0, fog_amount) * 0.12;
    float lum = dot(final_color, vec3(0.299, 0.587, 0.114));
    final_color = mix(final_color, vec3(lum), desat);

    final_color += emissive_color;

    final_color = final_color * (2.51 * final_color + 0.03) / (final_color * (2.43 * final_color + 0.59) + 0.14);
    final_color = clamp(final_color, 0.0, 1.0);

    // Saturation
    float l = dot(final_color, vec3(0.299, 0.587, 0.114));
    final_color = mix(vec3(l), final_color, saturation);

    // Contrast
    final_color = (final_color - 0.5) * contrast + 0.5;
    final_color = clamp(final_color, 0.0, 1.0);

    ALBEDO = final_color;
    ALPHA = 1.0;
}
