extends Control

# Air bubbles: ten slots above the hotbar's right edge, shown only while the
# breath is not full -- i.e. the eye has gone under a liquid. They mirror
# healthbar.gd's hearts on the left: a 9-unit sprite on an 8-unit pitch, so
# neighbours share an outline column, and the row spans the same 81 units of the
# hotbar's own width. Air is polled from PlayerController.get_air() in ticks
# (0..get_max_air()); each bubble is one tenth of the maximum, and a depleted
# slot reuses the same sprite under a dark modulate rather than a second file.

const BUBBLE_COUNT = 10
const GAP_ABOVE_HOTBAR = 2.0  # units between the bubbles and the hotbar's top

# Bubble layout in art texels: a 9x9 sprite on an 8-unit pitch, matching the
# hearts so the two rows sit on the same grid.
const BUBBLE_TEXELS = 9
const BUBBLE_PITCH_TEXELS = 8

# A depleted bubble is the same art dimmed, so nothing new has to be drawn or
# loaded for the empty state.
const EMPTY_TINT = Color(0.28, 0.30, 0.36, 1.0)

@onready var player_controller = get_node("/root/Main/Player")

var _bubble_texture: Texture2D = preload("res://textures/gui/bubble.png")
var _hotbar_texture: Texture2D = preload("res://textures/gui/hotbar.png")

var air := 0
var max_air := 1

# Redraw gate, same pattern as healthbar.gd: only repaint on real state changes.
var _last_size := Vector2.ZERO
var _last_ui_scale := -1.0

func _ready():
	# Nearest, like every other GUI surface: the bubbles are pixel art drawn at a
	# whole-unit size, so linear filtering can only blur them.
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST

func _process(_delta):
	var new_air := 0
	var new_max := 1
	if player_controller:
		new_max = maxi(int(player_controller.get_max_air()), 1)
		new_air = clampi(int(player_controller.get_air()), 0, new_max)
	var redraw := new_air != air or new_max != max_air or size != _last_size or not is_equal_approx(UIScale.value, _last_ui_scale)
	if redraw:
		air = new_air
		max_air = new_max
		_last_size = size
		_last_ui_scale = UIScale.value
		queue_redraw()

func _draw():
	# Full breath means not submerged: the row is absent, not empty.
	if not _hotbar_texture or air >= max_air:
		return
	var ui_scale = UIScale.value
	var bubble_size = BUBBLE_TEXELS * ui_scale
	var per = float(max_air) / float(BUBBLE_COUNT)
	for i in range(BUBBLE_COUNT):
		# Slot i covers air ((i)*per, (i+1)*per]; it is full once the remaining
		# air still reaches its top, so the row empties from the right first --
		# the direction the hearts empty in.
		var full = float(air) >= float(i + 1) * per
		draw_texture_rect(_bubble_texture,
			Rect2(_bubble_origin(i, ui_scale), Vector2(bubble_size, bubble_size)),
			false, Color(1, 1, 1, 1) if full else EMPTY_TINT)

## Where bubble `i` sits in the control, in units: the row right-aligned to the
## hotbar's far edge, on the same baseline as the hearts. One layout, so the
## bubbles line up with the hotbar and the hearts at every scale.
func _bubble_origin(i: int, ui_scale: float) -> Vector2:
	var hotbar_x = UIScale.centered_origin(size.x, _hotbar_texture.get_width())
	var hotbar_top = UIScale.edge_origin(size.y, _hotbar_texture.get_height(), 0.0)
	var row_units = BUBBLE_PITCH_TEXELS * (BUBBLE_COUNT - 1) + BUBBLE_TEXELS
	var x = hotbar_x + (_hotbar_texture.get_width() - row_units) * ui_scale \
		+ i * BUBBLE_PITCH_TEXELS * ui_scale
	var y = hotbar_top - GAP_ABOVE_HOTBAR * ui_scale - BUBBLE_TEXELS * ui_scale
	return Vector2(x, y)
