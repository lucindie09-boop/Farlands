extends Node2D

# Does the trail appear now? The marker is moved away once and its old home is read
# every frame: a trail means it glows and fades, no trail means it is black the
# moment the marker leaves. The container is a CanvasLayer, so this also exercises
# the overlay's own rect - a zero-sized one draws nothing, silently.
#
#   "$GODOT" --path . res://.freebuff/probe_phosphor.tscn

const OVERLAY_SCRIPT := "res://scripts/shader_overlay.gd"
const EFFECT := "phosphor"
const OLD_SPOT := Vector2i(50, 90)
const NEW_SPOT := Vector2i(245, 90)
const EMPTY_SPOT := Vector2i(150, 150)

var _overlay: Control
var _marker: ColorRect
var _frame := 0


func _ready() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)

	var background := ColorRect.new()
	background.set_anchors_preset(Control.PRESET_FULL_RECT)
	background.color = Color(0, 0, 0)
	layer.add_child(background)

	_marker = ColorRect.new()
	_marker.size = Vector2(30, 30)
	_marker.position = Vector2(40, 80)
	_marker.color = Color(1, 1, 1)
	layer.add_child(_marker)

	_overlay = load(OVERLAY_SCRIPT).new()
	layer.add_child(_overlay)


func _process(_delta: float) -> void:
	_frame += 1
	if _frame == 2:
		_overlay.set_enabled(EFFECT, true)
	elif _frame == 10:
		_marker.position = Vector2(240, 80)
	elif _frame >= 12 and _frame <= 26:
		_report("trail")
	elif _frame == 28:
		_rect().material.shader = load("res://.freebuff/probe_red.gdshader")
	elif _frame == 31:
		_report("red")
	elif _frame == 33:
		get_tree().quit(0)


func _report(stage: String) -> void:
	var screen := get_viewport().get_texture().get_image()
	var rect := _rect()
	print("probe: %-5s f=%02d old=%.3f new=%.3f empty=%.3f | overlay size=%s rect size=%s"
		% [stage, _frame, _luma(screen, OLD_SPOT), _luma(screen, NEW_SPOT), _luma(screen, EMPTY_SPOT),
			_overlay.size, rect.size])


func _rect() -> ColorRect:
	return _overlay._layers.get(EFFECT, {}).get("rect", null)


func _luma(image: Image, spot: Vector2i) -> float:
	if image == null:
		return -1.0
	return image.get_pixel(spot.x, spot.y).get_luminance()
