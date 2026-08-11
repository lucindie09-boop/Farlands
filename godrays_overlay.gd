extends ColorRect

# Drives shaders/godrays.gdshader. Projects the sun's world-space direction
# (read from ChunkManager/DayNightCycle) into screen space every frame and
# fades the effect out smoothly when the sun swings behind the camera or
# below the horizon, so the radial-scatter shader never gets a degenerate
# "sun point" to smear from.

const SUN_DISTANCE := 5000.0
const FRONT_FADE_START := -0.5   # dot(cam_forward, sun_dir) below this = fully hidden
const FRONT_FADE_END := 0.25     # dot(cam_forward, sun_dir) above this = fully visible
const HORIZON_FADE := 0.10       # sun_dir.y below this starts fading rays out

var camera: Camera3D
var chunk_manager: Node

func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)

	if material == null:
		var shader_material := ShaderMaterial.new()
		shader_material.shader = load("res://shaders/godrays.gdshader")
		material = shader_material

func _process(_delta: float) -> void:
	if camera == null:
		camera = get_viewport().get_camera_3d()
	if chunk_manager == null:
		chunk_manager = get_node_or_null("/root/Main/ChunkManager")
	if camera == null or chunk_manager == null or material == null:
		return

	var sun_dir: Vector3 = chunk_manager.get_sun_direction()
	if sun_dir.length_squared() < 0.0001:
		return
	sun_dir = sun_dir.normalized()

	var cam_forward: Vector3 = -camera.global_transform.basis.z
	var viewport_size: Vector2 = get_viewport_rect().size
	if viewport_size.x <= 0.0 or viewport_size.y <= 0.0:
		return

	# Dynamically reduce sample count at higher resolutions.
	# 32 samples at 1080p, scaling down for larger viewports. The ray march
	# self-normalizes (step and exposure scale by num_samples), so cutting the
	# count is visually free and directly halves the worst-case cost when sun
	# and moon both run a pass during twilight (up to 64 dependent samples/px).
	var shader_material := material as ShaderMaterial
	var samples := maxi(16, int(32.0 * 1080.0 / max(viewport_size.y, 1.0)))
	shader_material.set_shader_parameter("num_samples", samples)

	# --- Sun ---
	var sun_facing: float = cam_forward.dot(sun_dir)
	var sun_facing_fade: float = smoothstep(FRONT_FADE_START, FRONT_FADE_END, sun_facing)
	var sun_horizon_fade: float = smoothstep(-HORIZON_FADE, HORIZON_FADE, sun_dir.y)
	var sun_visibility: float = 0.0
	if sun_facing > 0.0:
		sun_visibility = sun_facing_fade * sun_horizon_fade
		if sun_visibility > 0.0:
			var sun_world_pos: Vector3 = camera.global_position + sun_dir * SUN_DISTANCE
			var screen_pos: Vector2 = camera.unproject_position(sun_world_pos)
			var sun_uv: Vector2 = screen_pos / viewport_size
			# Fade when sun projects off-screen (prevents white tint from edge-clamped samples)
			var edge_margin := 0.15
			sun_visibility *= smoothstep(-edge_margin, 0.0, sun_uv.x)
			sun_visibility *= smoothstep(1.0 + edge_margin, 1.0, sun_uv.x)
			sun_visibility *= smoothstep(-edge_margin, 0.0, sun_uv.y)
			sun_visibility *= smoothstep(1.0 + edge_margin, 1.0, sun_uv.y)
			# Fade when sun projects near screen center (prevents crosshair hotspot)
			var sun_center_dist: float = (sun_uv - Vector2(0.5, 0.5)).length()
			sun_visibility *= smoothstep(0.05, 0.25, sun_center_dist)
			shader_material.set_shader_parameter("sun_screen_uv", sun_uv)
	shader_material.set_shader_parameter("sun_visibility", sun_visibility)

	# --- Moon (opposite of sun) ---
	var moon_dir: Vector3 = -sun_dir
	var moon_facing: float = cam_forward.dot(moon_dir)
	var moon_facing_fade: float = smoothstep(FRONT_FADE_START, FRONT_FADE_END, moon_facing)
	var moon_horizon_fade: float = smoothstep(-HORIZON_FADE, HORIZON_FADE, moon_dir.y)
	var moon_visibility: float = 0.0
	if moon_facing > 0.0:
		moon_visibility = moon_facing_fade * moon_horizon_fade
		if moon_visibility > 0.0:
			var moon_world_pos: Vector3 = camera.global_position + moon_dir * SUN_DISTANCE
			var screen_pos: Vector2 = camera.unproject_position(moon_world_pos)
			var moon_uv: Vector2 = screen_pos / viewport_size
			var edge_margin := 0.15
			moon_visibility *= smoothstep(-edge_margin, 0.0, moon_uv.x)
			moon_visibility *= smoothstep(1.0 + edge_margin, 1.0, moon_uv.x)
			moon_visibility *= smoothstep(-edge_margin, 0.0, moon_uv.y)
			moon_visibility *= smoothstep(1.0 + edge_margin, 1.0, moon_uv.y)
			var moon_center_dist: float = (moon_uv - Vector2(0.5, 0.5)).length()
			moon_visibility *= smoothstep(0.05, 0.25, moon_center_dist)
			shader_material.set_shader_parameter("moon_screen_uv", moon_uv)
	shader_material.set_shader_parameter("moon_visibility", moon_visibility)
