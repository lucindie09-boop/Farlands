extends RefCounted

# The shatter's motion, in one place: the pixels of a piece of HUD art that has
# been destroyed, thrown up and out and dropped by gravity. It exists so every
# surface that wants something to come apart gets the same motion instead of a
# copy of it, and so a new surface only has to hand over what its art is and
# where it is drawn.
#
# A shard is one texel of that art, drawn at the texel's own size and from the
# place the art drew it, so the frame at the instant of the hit is the frame it
# was. Nothing about a shard turns: at that size a square covers the same pixels
# at every angle it could be drawn at, so a spin could not be seen at all.
#
# What a caller does:
#
#   const UIShatter := preload("res://scripts/ui_shatter.gd")
#   var _shards := UIShatter.new()
#
#   # when the art is destroyed -- `texels` and `art` describe which of it leaves
#   _shards.burst(texels, art, origin, texel, colour)
#
# A shard is drawn in one flat `colour`, or -- when the call passes `colours`,
# one per texel, which is what `colours_from_texture()` builds -- in the art's
# own colours, so an icon comes apart as itself rather than as a silhouette.
#
#   # every frame
#   if _shards.advance(delta):
#       queue_redraw()
#
#   # and from _draw()
#   _shards.draw(self)
#
# `texels` is one 0/1 flag per texel of the art, row-major, which is what
# `mask_from_texture()` and `freed()` build: the art's mask says which texels are
# the thing that can be destroyed (the hearts' red, an icon's ink), and the
# difference between the old and the new mask is exactly what a hit took away.
#
# `origin` is where the art's top-left corner sits in the drawing space; `texel`
# is how many units one art texel covers there (a GUI scale's own unit, so a
# shard is 2x2 device pixels at scale 2).

# The motion, in GUI units (one unit is one art texel of the art being drawn) and
# seconds. A hit throws the art's pixels up and out: POP is a lift every shard
# gets, so the destroyed pixels go up as a sheet and come straight back down; the
# pulse from the art's middle fans it outward, hardest at the outside, and only a
# share of the pulse acts vertically (PULSE_RISE) or it would sling the top of
# the art up and the bottom down and the lift would be lost in it.
# Every value a shard is thrown with is then spread far wider than the sheet it
# comes from: lifts and speeds half to double (POP_VARY, SPRAY_SPEED), angles the
# same way (SPRAY_ANGLE), and no two bursts come out alike either, because the
# whole burst lands harder or softer from one to the next (BURST_VARY). That
# spread is the point: art that comes apart into pixels all thrown the same way
# reads as a puff, not as damage.
# Gravity is deliberately hard (GRAVITY): the pop is what the eye reads as the
# hit landing, and a fall that eases down slowly reads as floating rather than
# falling.
const GRAVITY = 420.0  # units/s^2, one acceleration for every shard
const POP = 52.5       # units/s of lift, for every shard of the burst
const POP_VARY = 0.50  # fraction of the lift, per shard
const PULSE = 40.0     # units/s at the art's outer texels
const PULSE_FLOOR = 0.30   # how much of the pulse the middle texels get
const PULSE_RISE = 0.35    # how much of the pulse acts vertically
const SPRAY_ANGLE = 0.90   # radians of angle, per shard
const SPRAY_SPEED = 0.50   # fraction of speed, per shard
const BURST_VARY = 0.20    # fraction of the throw, per burst
const LIFE = 0.95      # seconds until the longest-lived shard is gone
const LIFE_VARY = 0.75  # the shortest-lived shard gets this much of it
const FADE = 0.15      # the last stretch of a shard's life, fading out
# A death drops 340 pixels (ten hearts); the cap is only a bound on a
# pathological pile-up of bursts while older pixels are still in the air.
const MAX_FRAGMENTS = 512

# The one deliberate reduction of the effect above, for art that is destroyed as a
# matter of routine rather than as an event: an inventory slot emptying as a stack
# is dragged off it. That happens one slot at a time and repeatedly, so at the full
# throw the shards of one are still falling when the next one empties, and a tidy
# grid fills with debris that reads as clutter rather than as feedback. At this
# intensity a burst neither flies nor lingers, so the screen settles between
# drags.
#
# It belongs here rather than in either caller because there are two inventory
# screens -- inventory.gd, and the grid the crafting table menu draws inside itself
# -- and the moment is the same one in both. A craft is NOT this: it happens once,
# it is what the player was reaching for, and its cells coming apart is the
# confirmation it worked, so the crafting boxes stay at the full effect.
const SPEND = 0.3

## What the shards land on, in one of two shapes, and in the drawing space
## `origin` was given in.
##
## A box is what a slot or a grid wants: a flat top and hard sides, nothing read
## off any art. A top edge is what a panel wants, and it is measured from the art
## itself, because a panel's top is not a straight line. The hotbar's, for one,
## steps down two texels at every gap between slots -- ten times across its
## width -- so a flat line drawn along it floats above the art at each of those
## gaps and the shards land on nothing at all. Reading the edge is what makes the
## dips real, and it costs one pass over the texture, once.
##
## One Surface is built per burst and shared by every shard of it, so the
## per-column profile is paid for once however many shards are in the air.
class Surface:
	## Where the art's top-left corner is drawn, and how many GUI units one art
	## texel covers there.
	var origin := Vector2.ZERO
	var texel := 1.0
	## The topmost row that draws anything, per art column; -1 for a column with
	## no ink at all, which is a gap and is left as one. Empty for a box.
	var top := PackedInt32Array()
	## The extent, and so the sides a shard turns off, in either shape.
	var rect := Rect2()

	## A flat box. Every column of it is the same height.
	static func box(r: Rect2) -> Surface:
		var s := Surface.new()
		s.rect = r
		return s

	## The art's own top edge, a texel at a time: for each column of `tex`, the
	## row of the topmost texel that draws anything.
	static func from_top_edge(tex: Texture2D, at: Vector2, texel_size: float) -> Surface:
		var s := Surface.new()
		s.origin = at
		s.texel = texel_size
		var image := tex.get_image()
		var w := image.get_width()
		var h := image.get_height()
		s.top.resize(w)
		for x in range(w):
			s.top[x] = -1
			for y in range(h):
				if image.get_pixel(x, y).a > 0.0:
					s.top[x] = y
					break
		s.rect = Rect2(at, Vector2(w, h) * texel_size)
		return s

	## The y the bottom of a shard of `size` at `x` comes to rest on, or INF where
	## there is nothing there to rest on. A box is its own bottom edge; an edge is
	## whatever the art has in the shard's own column, sampled at the shard's
	## middle so it cannot jitter either side of a column boundary.
	func rest_y(x: float, size: Vector2) -> float:
		if top.is_empty():
			return rect.end.y
		var column := clampi(int(floor((x + size.x * 0.5 - origin.x) / texel)), 0, top.size() - 1)
		var row: int = top[column]
		if row < 0:
			return INF
		return origin.y + float(row) * texel

# How much of its speed a shard keeps when it hits the side or the floor of a box
# it was given. Low on purpose: a shard that keeps most of its speed bounces
# around the box for its whole life and never looks like it settled, and the
# point of the box is that the debris comes to rest in it.
const RESTITUTION = 0.35

# Below this speed a shard that has hit a wall or the floor stops instead of
# bouncing again. Without it a shard that lands never settles: gravity re-
# accelerates it into the floor every frame and it buzzes there for the rest of
# its life, which is not the same thing as having come to rest.
const REST_SPEED = 14.0

# The shards still in the air: pos and vel in GUI units, size and colour as they
# are drawn, plus age and life in seconds.
var _falling: Array = []


## The multiplier for one burst, so that a caller putting more than one piece of
## art into the air for the same event can roll it once and pass the same one to
## each `burst()`.
static func roll_strength() -> float:
	return 1.0 + randf_range(-BURST_VARY, BURST_VARY)


## A 0/1 flag per texel of `tex` (row-major, `art` texels across and down): 1
## where `keep` says the texel is the part that can be destroyed. The art is read
## as its own pixels -- no comparison against a sampled texel -- so it survives
## the art being redrawn.
static func mask_from_texture(tex: Texture2D, art: Vector2i, keep: Callable) -> PackedByteArray:
	var image := tex.get_image()
	var mask := PackedByteArray()
	mask.resize(art.x * art.y)
	for y in range(art.y):
		for x in range(art.x):
			mask[y * art.x + x] = 1 if keep.call(image.get_pixel(x, y)) else 0
	return mask


## The texels `after` no longer has that `before` did: what one hit took away.
static func freed(before: PackedByteArray, after: PackedByteArray) -> PackedByteArray:
	var out := PackedByteArray()
	out.resize(before.size())
	for i in range(before.size()):
		out[i] = 1 if before[i] == 1 and after[i] == 0 else 0
	return out


## One colour per texel of the art, read straight off `tex` at the same grid
## `mask_from_texture` samples: what a shard of an icon is drawn in, so the icon
## comes apart in its own colours.
static func colours_from_texture(tex: Texture2D, art: Vector2i) -> PackedColorArray:
	var image := tex.get_image()
	var colours := PackedColorArray()
	colours.resize(art.x * art.y)
	for y in range(art.y):
		for x in range(art.x):
			colours[y * art.x + x] = image.get_pixel(x * image.get_width() / art.x, y * image.get_height() / art.y)
	return colours


## Throw every texel of `texels` that is set. `strength` is one `roll_strength()`
## for the whole event; leave it at 0 for a burst of its own. `colours`, when
## given, is drawn per texel instead of the one `colour`. `intensity` is how hard
## the burst throws and how long what it throws lives, as one multiplier on both.
## 1.0 is the effect as tuned: an event worth shouting about, like losing a heart.
## A surface whose art is destroyed often -- a slot emptying as a stack is dragged
## off it -- passes less, because the same throw every few seconds is not an event
## and the shards are still on screen when the next one starts.
##
## `floor` is a `Surface` -- `Surface.box()` for a slot or a grid, or
## `Surface.from_top_edge()` for a panel whose top edge is read off its art. Left
## null they fall as they always did, which is what a caller wants when there is
## nothing underneath to land on.
func burst(texels: PackedByteArray, art: Vector2i, origin: Vector2,
		texel: float, colour: Color, strength := 0.0,
		colours := PackedColorArray(), intensity := 1.0,
		floor: Surface = null) -> void:
	var hit := (strength if strength > 0.0 else roll_strength()) * intensity
	for i in range(texels.size()):
		if texels[i] == 1:
			var ink := colours[i] if i < colours.size() else colour
			_drop(i, art, origin, texel, ink, hit, intensity, floor)


## Move the shards on by `delta`; returns whether any are still in the air, so
## the caller knows a redraw is still needed.
func advance(delta: float) -> bool:
	for i in range(_falling.size() - 1, -1, -1):
		var pixel: Dictionary = _falling[i]
		pixel["age"] = pixel["age"] + delta
		if pixel["age"] >= pixel["life"]:
			# The order does not matter -- every shard is drawn on its own -- so
			# the dead one is swapped out in constant time.
			_falling[i] = _falling[_falling.size() - 1]
			_falling.remove_at(_falling.size() - 1)
			continue
		pixel["vel"] = pixel["vel"] + Vector2(0.0, GRAVITY * delta)
		pixel["pos"] = pixel["pos"] + pixel["vel"] * delta
		if pixel["floor"] != null:
			_bounce(pixel)
	return not _falling.is_empty()


## Keep a shard on the surface it was given, and turn it off whatever it hits.
## Without one a shard falls out of the frame and is simply never seen again,
## which reads as the debris thinning away rather than as it landing.
func _bounce(pixel: Dictionary) -> void:
	var floor: Surface = pixel["floor"]
	var box := floor.rect
	var at: Vector2 = pixel["pos"]
	var size: Vector2 = pixel["size"]
	var vel: Vector2 = pixel["vel"]
	# Asked of the surface rather than read off the box, because on a panel the
	# floor is not a straight line: it is whatever the art has in this shard's own
	# column, so a shard over a gap in the top edge falls through it.
	var rest := floor.rest_y(at.x, size)
	if rest < INF and at.y + size.y > rest:
		at.y = rest - size.y
		vel.y = -vel.y * RESTITUTION
	if at.x < box.position.x:
		at.x = box.position.x
		vel.x = absf(vel.x) * RESTITUTION
	elif at.x + size.x > box.end.x:
		at.x = box.end.x - size.x
		vel.x = -absf(vel.x) * RESTITUTION
	if rest < INF and absf(at.y + size.y - rest) < texel_epsilon(size) and absf(vel.y) < REST_SPEED:
		vel.y = 0.0
	pixel["pos"] = at
	pixel["vel"] = vel


## How close to the floor counts as being on it. The shard's own height, so it is
## exact for a box and a hair loose for a stepped edge, where "on the floor" means
## within the column's own texel rather than at one exact y.
func texel_epsilon(size: Vector2) -> float:
	return maxf(size.y * 0.5, 0.5)


## Throw away everything still in the air: for a surface that stops drawing them
## (a screen closing), where a fall nobody can see is not worth carrying on with.
func clear() -> void:
	_falling.clear()


## Draw the shards. Call it from a CanvasItem's `_draw()`, in the same space
## `origin` was given in.
func draw(canvas: CanvasItem) -> void:
	for pixel in _falling:
		var colour: Color = pixel["colour"]
		colour.a *= clampf((pixel["life"] - pixel["age"]) / FADE, 0.0, 1.0)
		# Snapped to the pixel grid, which is what holds a shard at whole device
		# pixels as it falls.
		var at: Vector2 = pixel["pos"]
		canvas.draw_rect(Rect2(at.round(), pixel["size"]), colour)


## One shard: thrown up and outward from the middle of the art. A texel at the
## very centre has no outward direction to be thrown along, so it takes a random
## one; everything else keeps its own direction with a spread around it, which is
## what keeps a column of the art from falling as a column.
##
## `hit` has already had the burst's strength and intensity folded into it, so it
## is the throw itself; `intensity` comes along separately only to shorten the
## life, which is not a matter of how hard something was thrown but of how long
## debris should sit on a screen before it goes. `floor`, when given, is the one
## Surface every shard of this burst shares.
func _drop(index: int, art: Vector2i, origin: Vector2, texel: float,
		colour: Color, hit: float, intensity: float, floor: Surface) -> void:
	if _falling.size() >= MAX_FRAGMENTS:
		return
	var tx := index % art.x
	var ty := index / art.x
	var from_centre := Vector2(tx, ty) - Vector2(art.x - 1, art.y - 1) * 0.5
	var angle := from_centre.angle() if from_centre.length_squared() > 0.01 else randf() * TAU
	angle += (randf() * 2.0 - 1.0) * SPRAY_ANGLE
	var reach := minf(from_centre.length() / (minf(art.x, art.y) * 0.5), 1.0)
	var speed := PULSE * (PULSE_FLOOR + (1.0 - PULSE_FLOOR) * reach)
	speed *= 1.0 - SPRAY_SPEED + randf() * SPRAY_SPEED * 2.0
	speed *= hit
	# The fan is the pulse, laid out sideways; the lift is the pop, and every
	# shard gets one, so the art's pixels go up as a sheet.
	var lift := POP * randf_range(1.0 - POP_VARY, 1.0 + POP_VARY) * hit
	var vel := Vector2(cos(angle) * speed, sin(angle) * speed * PULSE_RISE)
	vel.y -= lift
	# Placed at the corner of the texel it was drawn at, so a shard falls out of
	# the art from the pixel it was, not from beside it. Each shard also keeps its
	# own life, so they wink out one by one instead of all on the same frame.
	_falling.append({
		"pos": origin + Vector2(tx, ty) * texel,
		"vel": vel,
		"size": Vector2(texel, texel),
		"colour": colour,
		"life": LIFE * intensity * randf_range(LIFE_VARY, 1.0),
		"age": 0.0,
		"floor": floor,
	})
