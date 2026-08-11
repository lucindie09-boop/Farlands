extends Control

const Crosshair := preload("res://crosshair.gd")
const CONTRAST_SHADER := preload("res://shaders/crosshair_contrast.gdshader")
const SRC_PATH := "/root/Main/HUD/Crosshair"

var _material: ShaderMaterial
var _last_signature := -1

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_material = ShaderMaterial.new()
	_material.shader = CONTRAST_SHADER

func _process(_delta):
	if is_visible_in_tree():
		var sig := _draw_signature()
		if sig != _last_signature:
			_last_signature = sig
			queue_redraw()
			_sync_material()

func _draw_signature() -> int:
	var src := get_node_or_null(SRC_PATH)
	if src == null:
		return 0
	# Note: This is a hash, not a true equality check. Different settings states could
	# theoretically collide and skip a needed redraw, but probability is extremely low.
	# Low stakes since this only affects the settings menu preview.
	var h := int(round(size.x)) * 73856093 ^ int(round(size.y)) * 19349663
	h ^= (1 if src.cross_enabled else 0) * 104729
	h ^= int(round(src.cross_thickness * 8)) * 4051
	h ^= int(round(src.cross_length * 8)) * 521
	h ^= int(round(src.cross_spacing * 8)) * 3329
	h ^= int(round(src.cross_opacity * 32)) * 617
	h ^= int(src.cross_color.to_rgba32()) * 19
	h ^= (1 if src.top_line_enabled else 0) * 3559
	h ^= int(round(src.cross_rotation * 8)) * 8929
	h ^= (1 if src.cross_contrast else 0) * 7669
	h ^= (1 if src.dot_enabled else 0) * 10037
	h ^= int(round(src.dot_size * 8)) * 577
	h ^= int(round(src.dot_opacity * 32)) * 191
	h ^= int(src.dot_color.to_rgba32()) * 3359
	h ^= (1 if src.dot_contrast else 0) * 8117
	h ^= (1 if src.cross_dot_collision else 0) * 6221
	h ^= int(round(src.dot_rotation * 8)) * 4967
	return h

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
