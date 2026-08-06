extends Control

const CONTRAST_SHADER := preload("res://shaders/crosshair_contrast.gdshader")

const MARKER_CROSS := Color(1.0, 0.0, 0.0, 1.0)
const MARKER_DOT := Color(0.0, 1.0, 0.0, 1.0)

@onready var player_controller = get_node("/root/Main/Player")

var _material: ShaderMaterial

var cross_enabled: bool = true
var cross_length: float = 9.0
var cross_thickness: float = 2.0
var cross_spacing: float = 0.0
var cross_opacity: float = 1.0
var cross_color: Color = Color.WHITE
var top_line_enabled: bool = true
var cross_rotation: float = 0.0
var cross_contrast: bool = true
var dot_enabled: bool = false
var dot_size: float = 3.0
var dot_opacity: float = 1.0
var dot_color: Color = Color.WHITE
var dot_rotation: float = 0.0
var dot_contrast: bool = false
var cross_dot_collision: bool = true

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_material = ShaderMaterial.new()
	_material.shader = CONTRAST_SHADER

func _process(_delta):
	queue_redraw()
	_sync_material()

func _sync_material():
	var marker_mode := cross_contrast or dot_contrast
	if marker_mode:
		if material != _material:
			material = _material
		_material.set_shader_parameter("cross_color", cross_color)
		_material.set_shader_parameter("cross_opacity", cross_opacity)
		_material.set_shader_parameter("cross_contrast", cross_contrast)
		_material.set_shader_parameter("dot_color", dot_color)
		_material.set_shader_parameter("dot_opacity", dot_opacity)
		_material.set_shader_parameter("dot_contrast", dot_contrast)
	elif material != null:
		material = null

func _draw():
	if player_controller.is_chat_open() or player_controller.is_inventory_open() or player_controller.is_settings_open():
		return
	var cx := int(round(size.x / 2.0))
	var cy := int(round(size.y / 2.0))
	draw_crosshair(self, cx, cy,
		cross_enabled,
		maxi(1, int(round(cross_thickness))),
		int(round(cross_length)),
		int(round(cross_spacing)),
		cross_color, cross_opacity,
		top_line_enabled,
		dot_enabled,
		int(round(dot_size)),
		dot_color, dot_opacity,
		cross_rotation, dot_rotation,
		cross_dot_collision,
		cross_contrast or dot_contrast)

static func draw_crosshair(canvas: CanvasItem, cx: int, cy: int, cross_on: bool, w: int, seg: int, gap: int, cross_col: Color, cross_opa: float, top_line: bool, dot_on: bool, d: int, dot_col: Color, dot_opa: float, cross_deg: float, dot_deg: float, collision: bool, marker_mode: bool):
	if cross_on:
		var cross_fill := MARKER_CROSS if marker_mode else cross_col
		var cross_alpha := 1.0 if marker_mode else cross_opa
		var t := floori(float(w - 1) / 2.0)
		var use_dot_ref := dot_on and collision
		var half := t
		if use_dot_ref:
			half = floori(float(d - 1) / 2.0)
		var arms: Array = []
		if use_dot_ref or gap > 0:
			arms = [
				[cx - half - gap - seg, cy, cx - half - gap - 1, cy],
				[cx + half + gap + 1, cy, cx + half + gap + seg, cy],
				[cx, cy + half + gap + 1, cx, cy + half + gap + seg],
			]
			if top_line:
				arms.append([cx, cy - half - gap - seg, cx, cy - half - gap - 1])
		else:
			var l_ref := cx - t
			var r_ref := l_ref + w - 1
			var u_ref := cy - t
			var d_ref := u_ref + w - 1
			arms = [
				[l_ref - seg, cy, r_ref + seg, cy],
				[cx, d_ref, cx, d_ref + seg],
			]
			if top_line:
				arms.append([cx, u_ref - seg, cx, u_ref])
		_draw_arms(canvas, cx, cy, arms, w, cross_fill, cross_alpha, cross_deg)
	if dot_on:
		var dot_fill := MARKER_DOT if marker_mode else dot_col
		var dot_alpha := 1.0 if marker_mode else dot_opa
		_draw_dot(canvas, cx, cy, d, dot_fill, dot_alpha, dot_deg)

static func _draw_arms(canvas: CanvasItem, cx: int, cy: int, arms: Array, w: int, color: Color, opacity: float, deg: float):
	if deg == 0.0:
		for a in arms:
			_draw_seg(canvas, a[0], a[1], a[2], a[3], w, color, opacity)
		return
	canvas.draw_set_transform(Vector2(cx, cy), deg_to_rad(deg), Vector2.ONE)
	for a in arms:
		_draw_seg(canvas, a[0] - cx, a[1] - cy, a[2] - cx, a[3] - cy, w, color, opacity)
	canvas.draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)

static func _draw_seg(canvas: CanvasItem, x0: int, y0: int, x1: int, y1: int, w: int, color: Color, opacity: float):
	var fill := color
	fill.a = opacity
	var t := floori(float(w - 1) / 2.0)
	if y0 == y1:
		canvas.draw_rect(Rect2(x0, y0 - t, x1 - x0 + 1, w), fill)
	else:
		canvas.draw_rect(Rect2(x0 - t, y0, w, y1 - y0 + 1), fill)

static func _draw_dot(canvas: CanvasItem, cx: int, cy: int, d: int, color: Color, opacity: float, deg: float):
	var fill := color
	fill.a = opacity
	var t := floori(float(d - 1) / 2.0)
	if deg == 0.0:
		canvas.draw_rect(Rect2(cx - t, cy - t, d, d), fill)
	else:
		canvas.draw_set_transform(Vector2(cx, cy), deg_to_rad(deg), Vector2.ONE)
		canvas.draw_rect(Rect2(-t, -t, d, d), fill)
		canvas.draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)
