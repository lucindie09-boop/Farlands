extends Node

## The interface's tooltips.
##
## The engine has its own tooltip, and this project cannot use it: a tooltip is a
## popup window, the whole HUD is a CanvasLayer, and the popup is drawn UNDER the
## layer, so a tooltip on any control in the game was invisible (probes/
## probe_tooltip_layer.gd shows the engine drawing nothing for a button inside a
## CanvasLayer at layer 1). This node owns a layer above every other - the Liquid
## Texture Lab is 100 - and draws the house box itself.
##
## It reads the hovered control's own `tooltip_text` (walking up the parents if
## the control under the pointer has none), so a screen only has to set text: the
## menu buttons, the makers, the lab and the inventory's per-slot names all work
## through it without knowing it exists. There is no delay - the box is there on
## the frame the name appears and follows the pointer from there.

const UIArt := preload("res://scripts/ui_art.gd")
const MUNRO_FONT: Font = preload("res://Assets/fonts/munro.ttf")

## The box is 90% solid (10% transparent), so what is under it still reads faintly
## through it: the item it names, and the row behind it.
const ALPHA := 0.9

## Where the box sits relative to the pointer: 14 px to the right, and its
## BOTTOM edge level with the pointer - the y here is the gap under the box, so 0
## puts the box's underside on the cursor's line and the rest of it above, over
## the item it names rather than over the row below.
const OFFSET := Vector2(14, 0)

var _panel: PanelContainer = null
var _label: Label = null
var _text := ""
var _built_scale := 0.0

func _ready() -> void:
	# The pause menu and the inventory stop the tree while they are open, and
	# those are exactly where tooltips are used.
	process_mode = Node.PROCESS_MODE_ALWAYS
	# The engine's own tooltip is fed by the same `tooltip_text` this layer reads,
	# so leaving it alone would draw a second, default box over this one wherever
	# it manages to show. Blanking it in the default theme - the only theme a
	# popup reads - leaves exactly one tooltip in the game.
	var theme := ThemeDB.get_default_theme()
	theme.set_stylebox("panel", "TooltipPanel", StyleBoxEmpty.new())
	theme.set_color("font_color", "TooltipLabel", Color(0.0, 0.0, 0.0, 0.0))
	theme.set_color("font_shadow_color", "TooltipLabel", Color(0.0, 0.0, 0.0, 0.0))
	var layer := CanvasLayer.new()
	layer.layer = 200
	add_child(layer)
	var surface := Control.new()
	surface.set_anchors_preset(Control.PRESET_FULL_RECT)
	surface.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(surface)
	_panel = PanelContainer.new()
	_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_panel.modulate = Color(1.0, 1.0, 1.0, ALPHA)
	_panel.visible = false
	surface.add_child(_panel)
	_label = Label.new()
	_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_label.add_theme_font_override("font", MUNRO_FONT)
	_label.add_theme_color_override("font_color", Color.WHITE)
	# The vanilla tooltip's text carries a shadow, which is what keeps white
	# readable against a pale item name.
	_label.add_theme_color_override("font_shadow_color", Color(0.0, 0.0, 0.0, 0.75))
	_label.add_theme_constant_override("shadow_offset_x", 1)
	_label.add_theme_constant_override("shadow_offset_y", 1)
	_panel.add_child(_label)
	_style()

func _style() -> void:
	var scale := UIScale.value
	if is_equal_approx(scale, _built_scale):
		return
	_built_scale = scale
	_panel.add_theme_stylebox_override("panel", UIArt.tooltip_panel(scale))
	# No minimum on the label: the box is the text plus the padding, so it is a
	# different size for every name.
	_label.add_theme_font_size_override("font_size",
			int(roundf(UIArt.TOOLTIP_FONT_UNITS * scale)))

func _process(_delta: float) -> void:
	_style()
	var text := _hovered_text()
	if text != _text:
		_text = text
		_label.text = text
		_panel.visible = text != ""
		if _panel.visible:
			_panel.reset_size()
	if _panel.visible:
		_follow()

## The text of the control under the pointer, or of the nearest ancestor that has
## any (a slot's icon inside a screen, say).
func _hovered_text() -> String:
	if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		return ""
	# A control with nothing to say - the panel itself is ignored for the pointer,
	# but the game's own overlays are not.
	var node: Node = get_viewport().gui_get_hovered_control()
	while node is Control:
		var text := (node as Control).get_tooltip()
		if text != "":
			return text
		node = node.get_parent()
	return ""

## Right of the pointer with the box's bottom on the pointer's line, pulled back
## inside the window at its edges.
func _follow() -> void:
	var screen := get_viewport().get_visible_rect().size
	var at := get_viewport().get_mouse_position()
	at += Vector2(OFFSET.x, OFFSET.y - _panel.size.y)
	at.x = minf(at.x, screen.x - _panel.size.x)
	at.y = minf(at.y, screen.y - _panel.size.y)
	_panel.position = at.max(Vector2.ZERO)
