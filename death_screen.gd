extends Control

# Death overlay: shown when PlayerController's health reaches 0. Vanilla-style
# dark red tint, "You died!" caption and a Respawn button. Respawn restores
# full health and teleports back to the game-start spawn point (handled in
# C++; this overlay just reflects the died/respawned signals). Styled with
# the shared munro font and button.png like the pause/settings buttons.

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")

var _player: Node = null
var _title: Label
var _button: Button

func _ui_scale() -> float:
	return UIScale.value * 2.0 / 3.0

func _ready():
	visible = false
	mouse_filter = Control.MOUSE_FILTER_STOP

	var bg := ColorRect.new()
	bg.color = Color(0.45, 0.0, 0.0, 0.55)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(bg)

	var box := VBoxContainer.new()
	box.set_anchors_preset(Control.PRESET_CENTER)
	box.grow_horizontal = Control.GROW_DIRECTION_BOTH
	box.grow_vertical = Control.GROW_DIRECTION_BOTH
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_theme_constant_override("separation", 24)
	add_child(box)

	var s := _ui_scale()

	_title = Label.new()
	_title.text = "You died!"
	_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_title.add_theme_font_override("font", MUNRO_FONT)
	_title.add_theme_font_size_override("font_size", int(36 * s))
	_title.add_theme_color_override("font_color", Color.WHITE)
	_title.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.6))
	_title.add_theme_constant_override("shadow_offset_x", 2)
	_title.add_theme_constant_override("shadow_offset_y", 2)
	box.add_child(_title)

	_button = Button.new()
	_button.text = "Respawn"
	_button.pressed.connect(_on_respawn_pressed)
	_style_button(_button, 200.0)
	var center := CenterContainer.new()
	center.add_child(_button)
	box.add_child(center)

	_player = get_node_or_null("/root/Main/Player")
	if _player:
		_player.died.connect(_on_died)
		_player.respawned.connect(_on_respawned)

func _style_button(btn: Button, width: float):
	var s := _ui_scale()
	btn.add_theme_font_override("font", MUNRO_FONT)
	btn.add_theme_font_size_override("font_size", int(12 * s))
	btn.add_theme_color_override("font_color", Color.WHITE)
	btn.add_theme_color_override("font_hover_color", Color.WHITE)
	btn.add_theme_color_override("font_pressed_color", Color.WHITE)
	var normal := StyleBoxTexture.new()
	normal.texture = BUTTON_TEX
	var hover := StyleBoxTexture.new()
	hover.texture = BUTTON_TEX
	hover.modulate_color = Color(1.2, 1.2, 1.2)
	var pressed := StyleBoxTexture.new()
	pressed.texture = BUTTON_TEX
	pressed.modulate_color = Color(0.75, 0.75, 0.75)
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", normal)
	btn.custom_minimum_size = Vector2(width, 20) * s

func _on_died():
	visible = true

func _on_respawned():
	visible = false

func _on_respawn_pressed():
	if _player:
		_player.respawn()
