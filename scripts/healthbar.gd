extends Control

# Health bar: 10 hearts floating above the hotbar's left edge. A heart is the
# sprite's own 9 units on the art's 10-unit pitch (1 unit of space between), so
# the row spans 99 units and every position in it is a whole number of units at
# every GUI scale. Sizing a heart as a fraction of the hotbar's width instead --
# what this did before -- resampled the sprite at every setting.
# Health is in half-hearts (0..20), polled from PlayerController.get_health();
# fall damage drains it.

const HEART_COUNT = 10
const MAX_HEALTH = 20
const GAP_ABOVE_HOTBAR = 2.0  # units between the hearts and the hotbar's top

# Heart layout in art texels: 9x9 sprite, one empty texel between neighbors.
const HEART_TEXELS = 9
const HEART_PITCH_TEXELS = 10

@onready var player_controller = get_node("/root/Main/Player")

var _heart_full: Texture2D = preload("res://textures/gui/heart_full.png")
var _heart_half: Texture2D = preload("res://textures/gui/heart_half.png")
var _heart_empty: Texture2D = preload("res://textures/gui/heart_empty.png")
var _hotbar_texture: Texture2D = preload("res://textures/gui/hotbar.png")

var health := MAX_HEALTH

# Redraw gate, same pattern as hotbar.gd: only repaint on real state changes.
var _last_size := Vector2.ZERO
var _last_ui_scale := -1.0

func _ready():
	# Nearest, like every other GUI surface: the hearts are pixel art drawn at a
	# whole-unit size, so linear filtering can only blur them.
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST

func _process(_delta):
	var new_health := MAX_HEALTH
	if player_controller:
		new_health = clampi(int(player_controller.get_health()), 0, MAX_HEALTH)
	if new_health != health or size != _last_size or not is_equal_approx(UIScale.value, _last_ui_scale):
		health = new_health
		_last_size = size
		_last_ui_scale = UIScale.value
		queue_redraw()

func _draw():
	if not _hotbar_texture:
		return
	var ui_scale = UIScale.value
	var hotbar_width = _hotbar_texture.get_width()
	var hotbar_height = _hotbar_texture.get_height()
	# The row shares the hotbar's origin, so it can never drift from the bar it
	# sits on: both are snapped to the same scale grid.
	var hotbar_x = UIScale.centered_origin(size.x, hotbar_width)
	var hotbar_top = UIScale.edge_origin(size.y, hotbar_height, 0.0)

	var heart_size = HEART_TEXELS * ui_scale
	var pitch = HEART_PITCH_TEXELS * ui_scale
	var y = hotbar_top - GAP_ABOVE_HOTBAR * ui_scale - heart_size

	for i in range(HEART_COUNT):
		var half_hearts_left = health - i * 2
		var tex = _heart_empty
		if half_hearts_left >= 2:
			tex = _heart_full
		elif half_hearts_left == 1:
			tex = _heart_half
		draw_texture_rect(tex, Rect2(hotbar_x + i * pitch, y, heart_size, heart_size), false)
