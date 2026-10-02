extends Control

# Health bar: 10 hearts floating above the hotbar's left edge. A heart is the
# sprite's own 9 units on the art's 10-unit pitch (1 unit of space between), so
# the row spans 99 units and every position in it is a whole number of units at
# every GUI scale. Sizing a heart as a fraction of the hotbar's width instead --
# what this did before -- resampled the sprite at every setting.
# Health is in half-hearts (0..20), polled from PlayerController.get_health();
# fall damage drains it.
#
# Damage shatters: the red a hit takes away from a heart leaves as shards that
# fall off the bottom of the screen, instead of the heart simply being drawn one
# state emptier. What falls is read off the art -- every texel red in the heart's
# old state and not in its new one -- so the right half of a full heart drops out
# of it (14 pixels), a half heart empties out (20), and a heart lost whole drops
# all 34. One shard is one texel, drawn at the texel's own size and from the place
# the heart drew it, so at scale 2 a shard is the 2x2 block of device pixels the
# red was. The motion those shards are thrown with -- the pop, the fan, gravity
# and the lifetimes -- is scripts/ui_shatter.gd, shared with every other surface
# whose art comes apart.

const HEART_COUNT = 10
const MAX_HEALTH = 20
const GAP_ABOVE_HOTBAR = 2.0  # units between the hearts and the hotbar's top

# Heart layout in art texels: 9x9 sprite, one empty texel between neighbors.
const HEART_TEXELS = 9
const HEART_PITCH_TEXELS = 10

# The states a heart is drawn in, in the order `_heart_textures` holds them.
const HEART_EMPTY = 0
const HEART_HALF = 1
const HEART_FULL = 2

const SHATTER_RED = Color(1.0, 0.0, 0.0)  # #ff0000, the hearts' own red
const UIShatter := preload("res://scripts/ui_shatter.gd")

@onready var player_controller = get_node("/root/Main/Player")

var _heart_full: Texture2D = preload("res://textures/gui/heart_full.png")
var _heart_half: Texture2D = preload("res://textures/gui/heart_half.png")
var _heart_empty: Texture2D = preload("res://textures/gui/heart_empty.png")
var _hotbar_texture: Texture2D = preload("res://textures/gui/hotbar.png")
# The hotbar's top edge, read off its art. See _hotbar_floor().
var _floor: UIShatter.Surface = null
var _floor_scale := 0.0

var health := MAX_HEALTH

# Redraw gate, same pattern as hotbar.gd: only repaint on real state changes.
var _last_size := Vector2.ZERO
var _last_ui_scale := -1.0

# One flag per texel of each heart state, built in _ready: 1 where that state's
# art is red, 0 everywhere else. A texel that is red in the old state and not in
# the new one is what a hit took away.
var _red_masks: Array = []

# The shards still in the air, and the motion that carries them: see
# scripts/ui_shatter.gd.
var _shards := UIShatter.new()

func _ready():
	# Nearest, like every other GUI surface: the hearts are pixel art drawn at a
	# whole-unit size, so linear filtering can only blur them.
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_build_red_masks()

func _process(delta):
	var new_health := MAX_HEALTH
	if player_controller:
		new_health = clampi(int(player_controller.get_health()), 0, MAX_HEALTH)
	if new_health < health:
		_shatter(health, new_health)
	var redraw := new_health != health or size != _last_size or not is_equal_approx(UIScale.value, _last_ui_scale)
	if redraw:
		health = new_health
		_last_size = size
		_last_ui_scale = UIScale.value
	# The pixels are moving for as long as they exist, so their fall alone keeps
	# the redraw gate open.
	if _shards.advance(delta):
		redraw = true
	if redraw:
		queue_redraw()

func _draw():
	if not _hotbar_texture:
		return
	var ui_scale = UIScale.value
	var heart_size = HEART_TEXELS * ui_scale
	for i in range(HEART_COUNT):
		var half_hearts_left = health - i * 2
		var tex = _heart_empty
		if half_hearts_left >= 2:
			tex = _heart_full
		elif half_hearts_left == 1:
			tex = _heart_half
		draw_texture_rect(tex, Rect2(_heart_origin(i, ui_scale), Vector2(heart_size, heart_size)), false)
	_shards.draw(self)

func _build_red_masks() -> void:
	_red_masks = []
	for tex in [_heart_empty, _heart_half, _heart_full]:
		_red_masks.append(UIShatter.mask_from_texture(tex, Vector2i(HEART_TEXELS, HEART_TEXELS), _is_red))

## The red is the art's own #ff0000 against a black outline, so the test is
## simply "bright red" rather than a comparison with one sampled texel.
func _is_red(px: Color) -> bool:
	return px.a > 0.5 and px.r > 0.5 and px.g < 0.5 and px.b < 0.5

## A hit's red: every texel that was red in the heart's old state and is not in
## its new one leaves as a shard of its own, from the texel it was drawn at.
##
## The floor is the hotbar's top edge. The hearts are drawn just above it, so
## there is nothing else for their red to reach: without this the shards fell the
## two-unit gap and off the bottom of the screen, which read as the damage
## dissolving rather than as it landing on the bar it was measured against.
func _shatter(old_health: int, new_health: int) -> void:
	if not _hotbar_texture or _red_masks.is_empty():
		return
	var ui_scale = UIScale.value
	# One strength for the whole hit, so a hit that takes more than one heart
	# throws each heart's red the same way -- and separately, how hard the hit was.
	# The damage rides on `intensity` rather than on `strength` for a reason:
	# `strength` only multiplies the throw, while `intensity` multiplies the throw
	# AND the life, so a bigger hit not only throws further but hangs around
	# longer. Folding the damage into `strength` would have scaled the arc and
	# left the debris vanishing on the floor at the same rate as a nick.
	var strength := UIShatter.roll_strength()
	var intensity := maxi(old_health - new_health, 1)
	for i in range(HEART_COUNT):
		var before = _heart_state(old_health - i * 2)
		var after = _heart_state(new_health - i * 2)
		if after >= before:
			continue
		var lost := UIShatter.freed(_red_masks[before], _red_masks[after])
		_shards.burst(lost, Vector2i(HEART_TEXELS, HEART_TEXELS),
			_heart_origin(i, ui_scale), ui_scale, SHATTER_RED, strength, PackedColorArray(), intensity, _hotbar_floor(ui_scale))

## The hotbar's top edge, read off its art, as the floor. The same surface
## hotbar.gd builds for its own spent stacks -- one reading of one texture, and
## the two agree by construction rather than by two sets of arithmetic.
##
## Read once and kept, but only for as long as the scale it was read at holds: the
## Surface carries the origin and texel size it was built with, so a UI scale
## change has to rebuild it. The art itself never changes.
func _hotbar_floor(ui_scale: float) -> UIShatter.Surface:
	if _floor == null or not is_equal_approx(_floor_scale, ui_scale):
		var at := Vector2(UIScale.centered_origin(size.x, _hotbar_texture.get_width()),
			UIScale.edge_origin(size.y, _hotbar_texture.get_height(), 0.0))
		_floor = UIShatter.Surface.from_top_edge(_hotbar_texture, at, ui_scale)
		_floor_scale = ui_scale
	return _floor

func _heart_state(half_hearts_left: int) -> int:
	if half_hearts_left >= 2:
		return HEART_FULL
	if half_hearts_left == 1:
		return HEART_HALF
	return HEART_EMPTY

## Where heart `i` sits in the control, in units: the one layout the hearts and
## their shattering shards both go through, so a shard can only ever fall from
## the texel the heart drew it at.
func _heart_origin(i: int, ui_scale: float) -> Vector2:
	var hotbar_x = UIScale.centered_origin(size.x, _hotbar_texture.get_width())
	var hotbar_top = UIScale.edge_origin(size.y, _hotbar_texture.get_height(), 0.0)
	var heart_size = HEART_TEXELS * ui_scale
	var y = hotbar_top - GAP_ABOVE_HOTBAR * ui_scale - heart_size
	return Vector2(hotbar_x + i * HEART_PITCH_TEXELS * ui_scale, y)
