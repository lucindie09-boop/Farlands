shader_type spatial;
render_mode blend_mix, unshaded, cull_back;

uniform sampler2DArray texture_array : filter_nearest_mipmap;

uniform float mipmap_bias = 1.0;

// Lighting
uniform float sky_light_intensity : hint_range(0.0,1.0)=1.0;
uniform vec3 sky_light_color=vec3(1.0);
uniform vec3 sun_direction=vec3(0.0,1.0,0.0);

// Player light
uniform vec3 player_light_position=vec3(0.0);
uniform float player_light_radius=8.0;
uniform float player_light_intensity=1.0;
uniform vec3 player_light_color=vec3(1.0,0.9,0.7);

// Fog
uniform float fog_begin=64.0;
uniform float fog_end=896.0;
uniform float fog_density=0.6;
uniform float height_fog_density=0.012;
uniform float sea_level=64.0;

// Color grading
uniform float saturation=1.0;
uniform float contrast=1.0;

// AO tint & darkness floor
uniform vec3 ao_color=vec3(0.0);
uniform float ao_strength=1.0;
uniform vec3 darkness_color=vec3(0.0);

// Atmosphere
uniform float fog_scatter=0.0;
uniform vec3 fog_scatter_color=vec3(1.0,0.85,0.55);

uniform vec3 aerial_color=vec3(0.62,0.80,0.96);
uniform float aerial_strength=0.35;

uniform vec3 sky_light_warmth=vec3(1.0,0.95,0.85);

uniform vec3 horizon_color=vec3(0.30,0.52,0.85);
uniform vec3 zenith_color=vec3(0.06,0.20,0.85);
uniform float sky_turbidity=0.35;

// Water appearance
uniform vec4 water_color=vec4(0.15,0.45,0.55,0.85);
uniform float water_edge_fade=0.3;
uniform float water_absorption=4.0;

// Depth texture for Beer-Lambert absorption
uniform sampler2D depth_tex : hint_depth_texture, filter_linear;

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
    vec2 water_uv = custom2_attr.xy + vec2(TIME * 0.02, TIME * 0.01);
    vec3 tex_coord = vec3(water_uv, layer);
    vec4 color = texture(texture_array, tex_coord, mipmap_bias);

    float ao=custom1_attr.g;
    float ao_amount = clamp((1.0 - ao) * ao_strength, 0.0, 1.0);
    vec3 ao_tint = mix(ao_color, vec3(1.0), 1.0 - ao_amount);

    vec3 block_light=custom0_attr.rgb;

    vec3 sun_dir_norm = normalize(sun_direction);
    float sunFacing=max(dot(world_normal, sun_dir_norm),0.0);

    float skyBrightness=
        custom0_attr.a*
        sky_light_intensity*
        mix(0.25,1.0,sunFacing);

    vec3 skyTemp=
        mix(vec3(0.55,0.65,0.85),
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

    // Water shimmer
    float shimmer=
        sin(world_position.x*12.0+
        world_position.z*8.0)*0.5+0.5;

    shimmer*=
        sin(world_position.x*7.0-
        world_position.z*11.0+
        1.5)*0.5+0.5;

    lit_color*=1.0+shimmer*0.15;

    lit_color+=
        sky_light_color*
        sky_light_intensity*
        0.08;

    // Sun glint
    vec3 incident=normalize(view_direction);
    vec3 reflect_dir=reflect(incident,world_normal);
    float specAngle=max(dot(reflect_dir,sun_dir_norm),0.0);
    float glint=
        pow(specAngle,90.0)*1.2+
        pow(specAngle,20.0)*0.25;
    glint*=max(sky_light_intensity,0.05);

    lit_color+=vec3(1.0,0.85,0.6)*glint;

    // Bounce lighting
    if(sky_light_intensity>0.1){
        vec3 bounce=
            vec3(0.35,0.32,0.22)*
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

	if (fog_density > 0.001) {
    	float dist = max(fog_distance - fog_begin, 0.0);
    	float fog_range = max(fog_end - fog_begin, 1.0);
    	float x = clamp(dist / fog_range, 0.0, 1.0);
    	fog_amount = x * sqrt(x);
    	fog_amount *= fog_density * 1.5;
    	fog_amount = clamp(fog_amount, 0.0, 1.0);
    	dist_factor = smoothstep(fog_begin, fog_end, fog_distance);
	}

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
	vec3 haze_color = mix(vec3(0.95, 0.92, 0.88), fog_scatter_color, 0.3);
	vec3 turb_horizon = mix(horizon_color, haze_color, haze * 0.55);

	// Zenith desaturation: aerosols overwhelm Rayleigh, sky pales overhead
	float z_luma = dot(zenith_color, vec3(0.299, 0.587, 0.114));
	vec3 pale_zenith = mix(zenith_color, vec3(z_luma) * vec3(0.75, 0.80, 0.92), haze * 0.60);

	// ========= SKY GRADIENT =========
	float up2 = max(view_direction.y, 0.0);
	float skyBlend = 1.0 - (1.0 - up2) * sqrt(1.0 - up2);
	vec3 finalFogColor = mix(turb_horizon, pale_zenith, skyBlend);

	float sun_elevation = sun_dir_norm.y;
	float sun_vis = smoothstep(-0.08, 0.08, sun_elevation);

	// Sun glow widens with turbidity (wider Mie forward lobe)
	float sun_glow_width = mix(1.0, 2.2, haze);
	float sd2w = sd2;
	float sd4w = sd4;
	for (int i = 0; i < 2; i++) { sd2w = sd2w * sd2; sd4w = sd4w * sd4; }
	float sun_glow_turb = sd4 * 0.25 + sd2w * 0.15 + sd4w * 0.08;

	vec3 warmFog = mix(turb_horizon, fog_scatter_color, 0.35);
	finalFogColor = mix(finalFogColor, warmFog, sun_glow_turb * sun_vis * sun_glow_width);
	finalFogColor += scatter * (1.0 - fog_amount);

	// Extra horizon haze from turbidity
	float horizon_haze = (1.0 - up2) * haze * 0.12;
	finalFogColor = mix(finalFogColor, haze_color, horizon_haze);

    // ========= AERIAL PERSPECTIVE =========
    float aerial = fog_amount * fog_amount * aerial_strength;
    float luma = dot(lit_color, vec3(0.299, 0.587, 0.114));
    float tintStrength = aerial * (1.0 - luma * 0.6);
    float facingSky = clamp(world_normal.y * 0.5 + 0.5, 0.0, 1.0);
    tintStrength *= mix(1.2, 0.8, facingSky);
    vec3 sceneColor = mix(lit_color, finalFogColor, tintStrength);
    sceneColor += vec3(0.05, 0.08, 0.12) * fog_amount * (1.0 - sky_light_intensity);

    // ========= FINAL BLEND =========
    vec3 final_color = mix(sceneColor, finalFogColor, fog_amount);
    final_color += scatter * fog_amount * 0.20;

    float desat = (1.0 - smoothstep(0.95, 1.0, fog_amount)) * 0.12;
    float lum = dot(final_color, vec3(0.299, 0.587, 0.114));
    final_color = mix(final_color, vec3(lum), desat);

    final_color = final_color * (2.51 * final_color + 0.03) / (final_color * (2.43 * final_color + 0.59) + 0.14);
    final_color = clamp(final_color, 0.0, 1.0);

    // Saturation
    float l = dot(final_color, vec3(0.299, 0.587, 0.114));
    final_color = mix(vec3(l), final_color, saturation);

    // Contrast
    final_color = (final_color - 0.5) * contrast + 0.5;
    final_color = clamp(final_color, 0.0, 1.0);

    // ========= WATER ALPHA =========
    // Edge fade: shallow viewing angles (grazing) get more transparent.
    // This hides the hard transition where water meets terrain at the surface.
    float view_angle = abs(dot(normalize(view_direction), world_normal));
    float edge_fade = smoothstep(0.0, water_edge_fade, view_angle);

    // Beer-Lambert depth absorption: deeper water = more opaque.
    // Sample depth texture at current screen UV to get opaque geometry depth,
    // then compute non-linear depth difference vs water surface (FRAGCOORD.z).
    float scene_depth = texture(depth_tex, SCREEN_UV).r;
    float water_depth = abs(scene_depth - FRAGCOORD.z);
    float depth_factor = 1.0 - exp(-water_absorption * water_depth * 5.0);
    depth_factor = mix(0.35, 1.0, depth_factor);

    ALBEDO = final_color * water_color.rgb;
    ALPHA = water_color.a * depth_factor * edge_fade;
}
