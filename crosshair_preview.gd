extends Control

const Crosshair := preload("res://crosshair.gd")
const CONTRAST_SHADER := preload("res://shaders/crosshair_contrast.gdshader")
const SRC_PATH := "/root/Main/HUD/Crosshair"

var _material: ShaderMaterial

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_material = ShaderMaterial.new()
	_material.shader = CONTRAST_SHADER

func _process(_delta):
	if is_visible_in_tree():
		queue_redraw()
		_sync_material()

func _sync_material():
	var src := get_node_or_null(SRC_PATH)
	if src == null:
		return
	var marker_mode: bool = src.cross_contrast or src.dot_contrast
	if marker_mode:
		if material != _material:
			material = _material
		_material.set_shader_parameter("cross_color", src.cross_color)
		_material.set_shader_parameter("cross_opacity", src.cross_opacity)
		_material.set_shader_parameter("cross_contrast", src.cross_contrast)
		_material.set_shader_parameter("dot_color", src.dot_color)
		_material.set_shader_parameter("dot_opacity", src.dot_opacity)
		_material.set_shader_parameter("dot_contrast", src.dot_contrast)
	elif material != null:
		material = null

func _draw():
	var src := get_node_or_null(SRC_PATH)
	if src == null:
		return
	var cx := int(round(size.x / 2.0))
	var cy := int(round(size.y / 2.0))
	Crosshair.draw_crosshair(self, cx, cy,
		src.cross_enabled,
		maxi(1, int(round(src.cross_thickness))),
		int(round(src.cross_length)),
		int(round(src.cross_spacing)),
		src.cross_color, src.cross_opacity,
		src.top_line_enabled,
		src.dot_enabled,
		int(round(src.dot_size)),
		src.dot_color, src.dot_opacity,
		src.cross_rotation, src.dot_rotation,
		src.cross_dot_collision,
		src.cross_contrast or src.dot_contrast)
