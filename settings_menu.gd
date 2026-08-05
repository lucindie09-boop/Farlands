extends Control

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")

@onready var player_controller = get_node("/root/Main/Player")

var is_open = false

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_ui()
	hide()

func _build_ui():
	var bg := ColorRect.new()
	bg.color = Color(0.02, 0.02, 0.05, 0.5)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(bg)

	var title := Label.new()
	title.text = "SETTINGS"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_override("font", MUNRO_FONT)
	title.add_theme_font_size_override("font_size", 32)
	title.add_theme_color_override("font_color", Color.WHITE)
	title.set_anchors_preset(Control.PRESET_CENTER)
	title.offset_left = -200.0
	title.offset_right = 200.0
	title.offset_top = -160.0
	title.offset_bottom = -120.0
	add_child(title)

	var resume := Button.new()
	resume.text = "Resume"
	resume.add_theme_font_override("font", MUNRO_FONT)
	resume.add_theme_font_size_override("font_size", 16)
	resume.add_theme_color_override("font_color", Color.WHITE)
	resume.add_theme_color_override("font_hover_color", Color.WHITE)
	resume.add_theme_color_override("font_pressed_color", Color.WHITE)
	var normal := StyleBoxTexture.new()
	normal.texture = BUTTON_TEX
	var hover := StyleBoxTexture.new()
	hover.texture = BUTTON_TEX
	hover.modulate_color = Color(1.2, 1.2, 1.2)
	var pressed := StyleBoxTexture.new()
	pressed.texture = BUTTON_TEX
	pressed.modulate_color = Color(0.75, 0.75, 0.75)
	resume.add_theme_stylebox_override("normal", normal)
	resume.add_theme_stylebox_override("hover", hover)
	resume.add_theme_stylebox_override("pressed", pressed)
	resume.add_theme_stylebox_override("focus", normal)
	resume.custom_minimum_size = Vector2(200, 32)
	resume.set_anchors_preset(Control.PRESET_CENTER)
	resume.offset_left = -100.0
	resume.offset_right = 100.0
	resume.offset_top = -16.0
	resume.offset_bottom = 16.0
	resume.pressed.connect(_on_resume_pressed)
	add_child(resume)

func _input(event):
	if not event.is_action_pressed("ui_cancel"):
		return
	if get_viewport().is_input_handled():
		return
	if is_open:
		_close()
		get_viewport().set_input_as_handled()
	elif not player_controller.is_chat_open() and not player_controller.is_inventory_open():
		_open()
		get_viewport().set_input_as_handled()

func _on_resume_pressed():
	_close()

func _open():
	is_open = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	show()
	player_controller.set_settings_open(true)

func _close():
	is_open = false
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	hide()
	player_controller.set_settings_open(false)
