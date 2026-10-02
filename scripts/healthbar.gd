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
# all 34. One shard is one texel: it is drawn at the texel's own size and from
# the place the heart drew it, so at scale 2 a shard is the 2x2 block of device
# pixels the red was, and the frame at the instant of the hit is the frame it was.
# Nothing about a shard turns: at that size a square covers the same four pixels
# at every angle it could be drawn at, so a spin could not be seen at all.

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

# The shatter's motion, in GUI units (one unit is one texel of the art) and
# seconds. A hit throws the heart's red up and out: FALL_POP is a lift every
# shard gets, so the freed red goes up as a sheet and comes straight back down;
# the pulse from the heart's middle fans it outward, hardest at the outside, and
# only a share of the pulse acts vertically (FALL_PULSE_RISE) or it would sling
# the top of the heart up and the bottom down and the lift would be lost in it.
# Every value a shard is thrown with is then spread far wider than the sheet it
# comes from: lifts and speeds half to double (FALL_POP_VARY, FALL_SPRAY_SPEED),
# angles the same way (FALL_SPRAY_ANGLE), and no two hits come out alike either,
# because the whole burst lands harder or softer from one hit to the next
# (FALL_BURST_VARY). That spread is the point: a heart that comes apart into red
# all thrown the same way reads as a puff, not as damage.
# Gravity is deliberately hard (FALL_GRAVITY): the pop is what the eye reads as
# the hit landing, and a fall that eases down slowly reads as floating rather
# than falling.
const FALL_GRAVITY = 420.0  # units/s^2, one acceleration for every shard
const FALL_POP = 52.5       # units/s of lift, for every shard of the hit
const FALL_POP_VARY = 0.50  # fraction of the lift, per shard
const FALL_PULSE = 40.0     # units/s at the heart's outer texels
const FALL_PULSE_FLOOR = 0.30   # how much of the pulse the middle texels get
const FALL_PULSE_RISE = 0.35    # how much of the pulse acts vertically
const FALL_SPRAY_ANGLE = 0.90   # radians of angle, per shard
const FALL_SPRAY_SPEED = 0.50   # fraction of speed, per shard
const FALL_BURST_VARY = 0.20    # fraction of the throw, per hit
const FALL_LIFE = 0.95      # seconds until the longest-lived shard is gone
const FALL_LIFE_VARY = 0.75  # the shortest-lived shard gets this much of it
const FALL_FADE = 0.15      # the last stretch of a shard's life, fading out
const SHATTER_RED = Color(1.0, 0.0, 0.0)  # #ff0000, the hearts' own red
# A death drops 340 (ten full hearts); the cap is only a bound on a pathological
# pile-up of hits while older pixels are still in the air.
const MAX_FALLING = 512

@onready var player_controller = get_node("/root/Main/Player")

var _heart_full: Texture2D = preload("res://textures/gui/heart_full.png")
var _heart_half: Texture2D = preload("res://textures/gui/heart_half.png")
var _heart_empty: Texture2D = preload("res://textures/gui/heart_empty.png")
var _hotbar_texture: Texture2D = preload("res://textures/gui/hotbar.png")

var health := MAX_HEALTH

# Redraw gate, same pattern as hotbar.gd: only repaint on real state changes.
var _last_size := Vector2.ZERO
var _last_ui_scale := -1.0

# One flag per texel of each heart state, built in _ready: 1 where that state's
# art is red, 0 everywhere else. A texel that is red in the old state and not in
# the new one is what a hit took away.
var _red_masks: Array = []

# The shards still in the air: pos and vel in GUI units, plus age in seconds.
var _falling: Array = []

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
	if _advance_falling(delta):
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
	_draw_falling(ui_scale)

func _build_red_masks() -> void:
	# The red is the art's own #ff0000 against a black outline, so the test is
	# simply "bright red" rather than a comparison with one sampled texel.
	_red_masks = []
	for tex in [_heart_empty, _heart_half, _heart_full]:
		var image = tex.get_image()
		var mask := PackedByteArray()
		mask.resize(HEART_TEXELS * HEART_TEXELS)
		for y in range(HEART_TEXELS):
			for x in range(HEART_TEXELS):
				var px = image.get_pixel(x, y)
				mask[y * HEART_TEXELS + x] = 1 if px.a > 0.5 and px.r > 0.5 and px.g < 0.5 and px.b < 0.5 else 0
		_red_masks.append(mask)

## A hit's red: every texel that was red in the heart's old state and is not in
## its new one leaves as a shard of its own, from the texel it was drawn at.
func _shatter(old_health: int, new_health: int) -> void:
	if not _hotbar_texture or _red_masks.is_empty():
		return
	var ui_scale = UIScale.value
	# One multiplier for the whole burst: a hit that lands hard throws every shard
	# of it further than one that lands soft.
	var burst := 1.0 + randf_range(-FALL_BURST_VARY, FALL_BURST_VARY)
	for i in range(HEART_COUNT):
		var before = _heart_state(old_health - i * 2)
		var after = _heart_state(new_health - i * 2)
		if after >= before:
			continue
		var origin = _heart_origin(i, ui_scale)
		var old_mask: PackedByteArray = _red_masks[before]
		var new_mask: PackedByteArray = _red_masks[after]
		for texel in range(HEART_TEXELS * HEART_TEXELS):
			if old_mask[texel] == 1 and new_mask[texel] == 0:
				_drop_shard(origin, texel % HEART_TEXELS, texel / HEART_TEXELS, ui_scale, burst)

## One shard of the shatter: thrown up and outward from the middle of the heart.
## A texel at the very centre has no outward direction to be thrown along, so it
## takes a random one; everything else keeps its own direction with a little
## spread around it, which is what keeps a column of the heart from falling as a
## column.
func _drop_shard(origin: Vector2, tx: int, ty: int, ui_scale: float, burst: float) -> void:
	if _falling.size() >= MAX_FALLING:
		return
	var from_centre := Vector2(tx, ty) - Vector2.ONE * (HEART_TEXELS - 1) * 0.5
	var angle := from_centre.angle() if from_centre.length_squared() > 0.01 else randf() * TAU
	angle += (randf() * 2.0 - 1.0) * FALL_SPRAY_ANGLE
	var reach := minf(from_centre.length() / (HEART_TEXELS * 0.5), 1.0)
	var speed := FALL_PULSE * (FALL_PULSE_FLOOR + (1.0 - FALL_PULSE_FLOOR) * reach)
	speed *= 1.0 - FALL_SPRAY_SPEED + randf() * FALL_SPRAY_SPEED * 2.0
	speed *= burst
	# The fan is the pulse, laid out sideways; the lift is the pop, and every
	# shard gets the same one, so the heart's red goes up as a sheet.
	var lift := FALL_POP * randf_range(1.0 - FALL_POP_VARY, 1.0 + FALL_POP_VARY) * burst
	var vel := Vector2(cos(angle) * speed, sin(angle) * speed * FALL_PULSE_RISE)
	vel.y -= lift
	# Placed at the corner of the texel it was drawn at, so a shard falls out of
	# the heart from the red it was, not from beside it. Each shard also keeps its
	# own life, so they wink out one by one instead of all on the same frame.
	_falling.append({
		"pos": origin + Vector2(tx, ty) * ui_scale,
		"vel": vel,
		"life": FALL_LIFE * randf_range(FALL_LIFE_VARY, 1.0),
		"age": 0.0,
	})

## Move the shards on by `delta`; returns whether any are still in the air, so
## the caller knows a redraw is still needed.
func _advance_falling(delta: float) -> bool:
	for i in range(_falling.size() - 1, -1, -1):
		var pixel: Dictionary = _falling[i]
		pixel["age"] = pixel["age"] + delta
		if pixel["age"] >= pixel["life"]:
			# The order does not matter -- every pixel is the same red of the
			# same size -- so the dead one is swapped out in constant time.
			_falling[i] = _falling[_falling.size() - 1]
			_falling.remove_at(_falling.size() - 1)
			continue
		pixel["vel"] = pixel["vel"] + Vector2(0.0, FALL_GRAVITY * delta)
		pixel["pos"] = pixel["pos"] + pixel["vel"] * delta
	return not _falling.is_empty()

func _draw_falling(ui_scale: float) -> void:
	var shard := Vector2(ui_scale, ui_scale)
	for pixel in _falling:
		var colour = SHATTER_RED
		colour.a = clampf((pixel["life"] - pixel["age"]) / FALL_FADE, 0.0, 1.0)
		# Snapped to the pixel grid, which is what holds a shard at whole device
		# pixels as it falls.
		var at: Vector2 = pixel["pos"]
		draw_rect(Rect2(at.round(), shard), colour)

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
