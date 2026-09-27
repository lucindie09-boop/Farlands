extends Control

# The screen shaders' live state, and the layers that draw them.
#
# One child ColorRect per shader in data/shaders.json, in that file's order: it
# passes the frame already drawn beneath it (the world, plus whichever HUD art
# this node sits above in Main.tscn) through its shader and paints the result
# back. A shader that is off simply has its layer hidden, so an off stack costs
# nothing but a hidden Control.
#
# This node owns the state but not the file: settings_menu.gd is the one that
# reads and writes user://settings.cfg, so the Shaders page calls in here for
# every value it shows and every value it changes.
#
# The uniform a param drives is the param's own `key`, so a shader's uniforms,
# data/shaders.json and the saved config keys all carry the same names.

const DEFINITIONS_PATH := "res://data/shaders.json"

var _definitions: Array = []
var _layers: Dictionary = {}   # id -> { rect, material, uniforms }
var _enabled: Dictionary = {}  # id -> bool
var _values: Dictionary = {}   # id -> { key: value }


func _ready() -> void:
	# The layers are decoration: the pointer belongs to whatever HUD art is under
	# them, exactly like the god-rays overlay.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	_load_definitions()
	for definition in _definitions:
		_build_layer(definition)
	_push_frame_size()
	# The patterns a shader measures are counted in frame pixels, so a resized
	# window has to tell them how many there are now.
	get_viewport().size_changed.connect(_push_frame_size)


# --- The registry -------------------------------------------------------------

func get_definitions() -> Array:
	return _definitions


func get_shader_ids() -> Array:
	var ids: Array = []
	for definition in _definitions:
		ids.append(String(definition.get("id", "")))
	return ids


func get_shader_name(id: String) -> String:
	for definition in _definitions:
		if String(definition.get("id", "")) == id:
			return String(definition.get("name", id))
	return id


# --- Turned on, turned off ----------------------------------------------------

func is_enabled(id: String) -> bool:
	return bool(_enabled.get(id, false))


func set_enabled(id: String, enabled: bool) -> void:
	if not _layers.has(id):
		return
	_enabled[id] = enabled
	(_layers[id]["rect"] as ColorRect).visible = enabled


# --- Param values -------------------------------------------------------------

func get_value(id: String, key: String) -> Variant:
	var values: Dictionary = _values.get(id, {})
	return values.get(key, null)


func set_value(id: String, key: String, value: Variant) -> void:
	if not _values.has(id):
		return
	_values[id][key] = value
	var layer: Dictionary = _layers.get(id, {})
	if layer.is_empty():
		return
	# Only push what the shader actually declares, so a mistyped key in the
	# registry is reported once at load (below) instead of erroring every frame.
	var uniforms: Dictionary = layer["uniforms"]
	if uniforms.has(key):
		(layer["material"] as ShaderMaterial).set_shader_parameter(key, value)


# --- Building the stack -------------------------------------------------------

func _load_definitions() -> void:
	var file := FileAccess.open(DEFINITIONS_PATH, FileAccess.READ)
	if file == null:
		push_error("Failed to load shaders.json")
		return
	var json_text := file.get_as_text()
	file.close()

	var json := JSON.new()
	var parse_err := json.parse(json_text)
	if parse_err != OK:
		push_error("Failed to parse shaders.json: " + json.get_error_message())
		return

	_definitions = json.data.get("shaders", []) if json.data is Dictionary else []


func _build_layer(definition: Dictionary) -> void:
	var id := String(definition.get("id", ""))
	if id == "":
		push_error("shaders.json: a shader has no id")
		return
	var path := String(definition.get("shader", ""))
	if path == "" or not ResourceLoader.exists(path):
		push_error("shaders.json: %s has no shader at %s" % [id, path])
		return
	var shader: Shader = load(path)
	if shader == null:
		push_error("shaders.json: %s failed to load %s" % [id, path])
		return

	var rect := ColorRect.new()
	rect.name = id
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	# The shader writes the finished picture, alpha and all: the rect's own
	# colour only has to not be drawn over it before the shader runs.
	rect.color = Color(0, 0, 0, 0)
	var material := ShaderMaterial.new()
	material.shader = shader
	rect.material = material

	var uniforms := _uniform_names(shader)
	_enabled[id] = bool(definition.get("enabled", false))
	rect.visible = _enabled[id]

	_values[id] = {}
	for param in definition.get("params", []):
		var key := String(param.get("key", ""))
		if key == "":
			continue
		if not uniforms.has(key):
			push_warning("shaders.json: %s has no uniform %s" % [id, key])
		var value: Variant = param.get("default", 0.0)
		_values[id][key] = value
		material.set_shader_parameter(key, value)

	_layers[id] = {"rect": rect, "material": material, "uniforms": uniforms}
	add_child(rect)


func _uniform_names(shader: Shader) -> Dictionary:
	var names := {}
	for uniform in shader.get_shader_uniform_list():
		names[String(uniform["name"])] = true
	return names


# The frame the shaders make their patterns out of: the viewport they are drawn
# over, in pixels, which is the size of the screen texture they sample.
func _push_frame_size() -> void:
	var size := Vector2(get_viewport().get_visible_rect().size)
	for id in _layers:
		var layer: Dictionary = _layers[id]
		if (layer["uniforms"] as Dictionary).has("frame_size"):
			(layer["material"] as ShaderMaterial).set_shader_parameter("frame_size", size)
