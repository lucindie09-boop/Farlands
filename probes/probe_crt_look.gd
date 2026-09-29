extends SceneTree
## Lookdev for a screen shader.
##
## WINDOWED. It renders the real game once, freezes that frame, and then draws
## the frozen frame back through each candidate pass in turn, so every candidate
## in a run sees the *same* pixels - the world keeps animating underneath, but
## nothing of it reaches the passes. What lands in the shots is only the pass.
##
##   "$GODOT" --path . --script res://probes/probe_crt_look.gd
##
## Shots go to user://shader_shots/look_*.png and are copied out by hand, or by
## .freebuff/run_probe_shot.sh, which harvests that directory.
##
## The user's settings.cfg is read at the start and written back at the end (the
## menu saves on _exit_tree, so the file is restored AND the live state re-read:
## restoring the file alone would be overwritten by that exit save).

const SHOT_DIR := "user://shader_shots"
const SETTINGS_PATH := "user://settings.cfg"
const OVERLAY_PATH := "HUD/ShaderOverlay"
const LAYER := 200

# label, shader, params. Add one line per look you want to see.
const CANDIDATES := [
	{
		"name": "b_base",
		"shader": "res://shaders/crt.gdshader",
		"params": {},
	},
	{
		"name": "c_new",
		"shader": "res://shaders/crt.gdshader",
		"params": {},
	},
	{
		"name": "d_more",
		"shader": "res://shaders/crt.gdshader",
		"params": {
			"scanline_depth": 0.7, "curvature": 0.3, "glow": 1.0,
			"vignette": 0.4, "mask_strength": 0.25, "saturation": 1.2,
		},
	},
]

var size := Vector2i(1280, 720)
var overlay: Control = null
var menu: Control = null
var _settings_backup := PackedByteArray()
var _had_settings := false


func _initialize() -> void:
	_run()


func _run() -> void:
	if OS.get_environment("PROBE_W").is_valid_int():
		size.x = int(OS.get_environment("PROBE_W"))
	if OS.get_environment("PROBE_H").is_valid_int():
		size.y = int(OS.get_environment("PROBE_H"))
	DisplayServer.window_set_size(size)
	_backup_settings()
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)

	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		_finish()
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	overlay = main.get_node_or_null(OVERLAY_PATH)
	menu = main.get_node_or_null("HUD/SettingsMenu")
	if overlay != null:
		overlay.visible = false

	for i in range(90):
		await process_frame

	# The frame every candidate is drawn over, frozen.
	var source := _capture()
	print("probe: froze %dx%d source frame" % [source.get_width(), source.get_height()])

	var layer := CanvasLayer.new()
	layer.layer = LAYER
	root.add_child(layer)
	var picture := TextureRect.new()
	picture.texture = ImageTexture.create_from_image(source)
	picture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	picture.stretch_mode = TextureRect.STRETCH_SCALE
	picture.set_anchors_preset(Control.PRESET_FULL_RECT)
	picture.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(picture)

	var pass_rect := ColorRect.new()
	pass_rect.color = Color(1, 1, 1, 1)
	pass_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	pass_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	pass_rect.visible = false
	var material := ShaderMaterial.new()
	pass_rect.material = material
	layer.add_child(pass_rect)

	# `off` is the same frozen frame with no pass over it, so every shot below
	# can be read against it.
	await _settle()
	_save(_capture(), "look_off")

	for candidate in CANDIDATES:
		var shader: Shader = load(String(candidate["shader"]))
		if shader == null:
			print("PROBE FAIL: no shader %s" % candidate["shader"])
			continue
		material.shader = shader
		# A fresh material per candidate: uniforms set by a previous candidate
		# must not leak into the next one.
		var fresh := ShaderMaterial.new()
		fresh.shader = shader
		pass_rect.material = fresh
		for name in _uniform_names(shader):
			var default_value: Variant = _uniform_default(shader, name)
			if default_value != null:
				fresh.set_shader_parameter(name, default_value)
		fresh.set_shader_parameter("frame_size", Vector2(size))
		var params: Dictionary = candidate["params"]
		for key in params:
			fresh.set_shader_parameter(String(key), params[key])
		pass_rect.visible = true
		await _settle()
		var img := _capture()
		_save(img, "look_%s" % candidate["name"])
		print("probe: %s (%d override(s))" % [candidate["name"], params.size()])
		pass_rect.visible = false
		await _settle()

	_finish()


# --- Uniform defaults ---------------------------------------------------------

func _uniform_names(shader: Shader) -> Array:
	var names: Array = []
	for uniform in shader.get_shader_uniform_list():
		names.append(String(uniform["name"]))
	return names


## The default the shader declares for a uniform, or null when it has none (a
## sampler, or a uniform with no initialiser).
func _uniform_default(shader: Shader, name: String) -> Variant:
	for uniform in shader.get_shader_uniform_list():
		if String(uniform["name"]) != name:
			continue
		if not uniform.has("default_value"):
			return null
		return uniform["default_value"]
	return null


# --- Odds and ends ------------------------------------------------------------

func _settle() -> void:
	for i in range(3):
		await process_frame


func _capture() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _save(img: Image, name: String) -> void:
	var path := "%s/%s_%dx%d.png" % [SHOT_DIR, name, img.get_width(), img.get_height()]
	img.save_png(path)
	print("probe: shot %s" % path)


func _backup_settings() -> void:
	if not FileAccess.file_exists(SETTINGS_PATH):
		return
	var file := FileAccess.open(SETTINGS_PATH, FileAccess.READ)
	if file == null:
		return
	_settings_backup = file.get_buffer(file.get_length())
	file.close()
	_had_settings = true


func _finish() -> void:
	if _had_settings:
		var file := FileAccess.open(SETTINGS_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_settings_backup)
			file.close()
	if menu != null and is_instance_valid(menu):
		menu.call("_load_settings")
	print("probe: done")
	quit(0)
