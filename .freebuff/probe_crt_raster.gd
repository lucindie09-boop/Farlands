extends SceneTree
## Raster sweep for the CRT pass, at the resolution the user actually plays at.
##
## WINDOWED. It renders the real game once, freezes that frame, then draws the
## frozen frame back through the pass once per scanline count, so every shot in
## a run sees the *same* pixels. What lands in the shots is only the pass.
##
##   "$GODOT" --path . --script res://.freebuff/probe_crt_raster.gd
##
## Why: this is what the raster looked like when the knob was a count of lines.
## A count of lines re-derives a dot size of frame_size.y / count pixels, which
## is almost never a whole number; the averaging taps then land at a different
## place inside every dot, and the pattern of that difference is what you see
## instead of a raster. At 1908x960 the counts that happened to land on whole
## pixels (480, 320, 240, 192) were the clean ones and the rest (180, the old
## default, and 237 here) were blotchy.
##
## The knob is now `picture_scale`, in screen pixels per dot, so every setting
## is a whole number of pixels by construction. This probe renders every value
## the slider offers over one frozen frame: none of them may show the lattice.
##
## The user's settings.cfg is read at the start and written back at the end (the
## menu saves on _exit_tree, so the file is restored AND the live state re-read:
## restoring the file alone would be overwritten by that exit save).

const SHOT_DIR := "user://shader_shots"
const SETTINGS_PATH := "user://settings.cfg"
const OVERLAY_PATH := "HUD/ShaderOverlay"
const SHADER := "res://shaders/crt.gdshader"
const LAYER := 200

# The user's window. 960 is divisible by 2.
const WINDOW := Vector2i(1908, 960)

# picture_scale, one entry per value the slider can be set to. On a 960-pixel
# window these are 960, 480, 320, 240, 192, 160, 137 and 120 lines.
const CANDIDATES := [
	{"name": "s1", "params": {"picture_scale": 1}},
	{"name": "s2", "params": {"picture_scale": 2}},
	{"name": "s3", "params": {"picture_scale": 3}},
	{"name": "s4", "params": {"picture_scale": 4}},
	{"name": "s5", "params": {"picture_scale": 5}},
	{"name": "s6", "params": {"picture_scale": 6}},
	{"name": "s7", "params": {"picture_scale": 7}},
	{"name": "s8", "params": {"picture_scale": 8}},
]

var size := WINDOW
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

	print("probe: window %dx%d" % [size.x, size.y])
	for candidate in CANDIDATES:
		var scale := float(candidate["params"]["picture_scale"])
		print("probe: %-3s %g px per dot -> %.0f lines, each dot exactly %g px"
			% [candidate["name"], scale, float(size.y) / scale, scale])

	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		print("PROBE FAIL: Main.tscn missing")
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
	layer.add_child(pass_rect)

	await _settle()
	_save(_capture(), "raster_off")

	for candidate in CANDIDATES:
		var shader: Shader = load(SHADER)
		if shader == null:
			print("PROBE FAIL: no shader %s" % SHADER)
			break
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
		_save(_capture(), "raster_%s" % candidate["name"])
		print("probe: %s done" % candidate["name"])
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
