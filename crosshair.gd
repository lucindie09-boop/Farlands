extends Control

@onready var player_controller = get_node("/root/Main/Player")

var cross_enabled: bool = true
var cross_length: float = 4.0
var cross_thickness: float = 1.0
var cross_spacing: float = 0.0
var cross_opacity: float = 1.0
var cross_color: Color = Color.WHITE
var top_line_enabled: bool = true
var dot_enabled: bool = true
var dot_size: float = 3.0
var dot_opacity: float = 1.0
var dot_color: Color = Color.WHITE

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE

func _process(_delta):
	queue_redraw()

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
		dot_color, dot_opacity)

static func draw_crosshair(canvas: CanvasItem, cx: int, cy: int, cross_on: bool, w: int, seg: int, gap: int, cross_color: Color, cross_opacity: float, top_line: bool, dot_on: bool, d: int, dot_color: Color, dot_opacity: float):
	if cross_on:
		var inner: int
		if dot_on:
			inner = floori(float(d - 1) / 2.0) + gap
		else:
			inner = gap
		_draw_seg(canvas, cx - inner - seg, cy, cx - inner - 1, cy, w, cross_color, cross_opacity)
		_draw_seg(canvas, cx + inner + 1, cy, cx + inner + seg, cy, w, cross_color, cross_opacity)
		_draw_seg(canvas, cx, cy + inner + 1, cx, cy + inner + seg, w, cross_color, cross_opacity)
		if top_line:
			_draw_seg(canvas, cx, cy - inner - seg, cx, cy - inner - 1, w, cross_color, cross_opacity)
	if dot_on:
		_draw_dot(canvas, cx, cy, d, dot_color, dot_opacity)

static func _draw_seg(canvas: CanvasItem, x0: int, y0: int, x1: int, y1: int, w: int, color: Color, opacity: float):
	var fill := color
	fill.a = opacity
	var t := floori(float(w - 1) / 2.0)
	if y0 == y1:
		canvas.draw_rect(Rect2(x0, y0 - t, x1 - x0 + 1, w), fill)
	else:
		canvas.draw_rect(Rect2(x0 - t, y0, w, y1 - y0 + 1), fill)

static func _draw_dot(canvas: CanvasItem, cx: int, cy: int, d: int, color: Color, opacity: float):
	var fill := color
	fill.a = opacity
	var t := floori(float(d - 1) / 2.0)
	canvas.draw_rect(Rect2(cx - t, cy - t, d, d), fill)
