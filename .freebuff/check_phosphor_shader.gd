extends SceneTree

# Compiles shaders/phosphor.gdshader in a real renderer (--headless cannot: the
# dummy renderer never compiles a shader) and prints the uniforms it declares, so
# that the overlay's own contract - a pass declaring `previous_frame` is handed the
# previous frame - can be read off before anything is looked at by eye.
#
#   "$GODOT" --path . --script res://.freebuff/check_phosphor_shader.gd

var _frames := 0


func _initialize() -> void:
	var shader: Shader = load("res://shaders/phosphor.gdshader")
	if shader == null:
		print("phosphor: shader did not load")
		quit(1)
		return
	var names: Array = []
	for uniform in shader.get_shader_uniform_list():
		names.append(String(uniform["name"]))
	print("phosphor: uniforms %s" % [names])
	print("phosphor: declares previous_frame=%s" % names.has("previous_frame"))

	var material := ShaderMaterial.new()
	material.shader = shader
	var rect := ColorRect.new()
	rect.size = Vector2(256, 128)
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	rect.material = material
	rect.color = Color(0.2, 0.4, 0.9)
	root.add_child(rect)


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames >= 8:
		print("phosphor: survived %d drawn frame(s)" % _frames)
		quit(0)
		return true
	return false
