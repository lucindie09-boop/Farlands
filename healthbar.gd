extends Control

# Minecraft-style health bar: 10 hearts floating above the hotbar's left
# edge. Sized off the hotbar's on-screen width so the row spans ~40% of the
# bar -- hearts render as 9-texel sprites on a 10-texel pitch (1 texel of
# space between), and the resulting 99-texel span maps onto the 40% fraction.
# Health is in half-hearts (0..20, Minecraft convention), polled from
# PlayerController.get_health(); fall damage drains it.

const HEART_COUNT = 10
const MAX_HEALTH = 20
const HOTBAR_WIDTH_FRACTION = 0.4
const GAP_ABOVE_HOTBAR = 2.0  # UI-scale pixels between hearts and hotbar top

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
	texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR

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
	var hotbar_width = _hotbar_texture.get_width() * ui_scale
	var hotbar_height = _hotbar_texture.get_height() * ui_scale
	var hotbar_x = (size.x - hotbar_width) / 2.0
	var hotbar_top = size.y - hotbar_height

	var span_texels = float(HEART_COUNT * HEART_PITCH_TEXELS - HEART_TEXELS)
	var texel = hotbar_width * HOTBAR_WIDTH_FRACTION / span_texels
	var heart_size = texel * float(HEART_TEXELS)
	var pitch = texel * float(HEART_PITCH_TEXELS)
	var y = hotbar_top - GAP_ABOVE_HOTBAR * ui_scale - heart_size

	for i in range(HEART_COUNT):
		var half_hearts_left = health - i * 2
		var tex = _heart_empty
		if half_hearts_left >= 2:
			tex = _heart_full
		elif half_hearts_left == 1:
			tex = _heart_half
		draw_texture_rect(tex, Rect2(hotbar_x + i * pitch, y, heart_size, heart_size), false)
