extends Control

const Crosshair := preload("res://crosshair.gd")
const SRC_PATH := "/root/Main/HUD/Crosshair"

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE

func _process(_delta):
	if is_visible_in_tree():
		queue_redraw()

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
		src.cross_rotation, src.dot_rotation)
