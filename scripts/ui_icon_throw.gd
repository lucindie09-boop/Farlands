extends RefCounted

# The departure effect: HUD art that leaves WHOLE.
#
# The shatter (ui_shatter.gd) is for art that has been destroyed: it takes the
# texels a hit removed and throws them as pixels. A dropped item is not that. It
# is in transit -- the player still has it, and it is coming back -- so the thing
# that leaves has to leave as one piece, exactly the art the slot was drawing, and
# the piece has to TURN as it goes, because a rigid thing tumbling is the read and
# a shatter cannot rotate: it is not one thing any more.
#
# What a caller does:
#
#   const UIIconThrow := preload("res://scripts/ui_icon_throw.gd")
#   var _thrown := UIIconThrow.new()
#
#   # when the art leaves -- `rect` is where it was drawn, `side` is which way out
#   # of its slot it should be nudged (-1..1), `exit_y` is the line below which
#   # nothing of it is on screen any more, and `scale` is how big things are drawn
#   # in that space (the caller's GUI scale)
#   _thrown.launch(tex, rect, side, exit_y, scale)
#
#   # every frame
#   if _thrown.advance(delta):
#       queue_redraw()
#
#   # and from _draw()
#   _thrown.draw(self)
#
# The motion is gravity, in screen pixels and seconds, the same terms the shatter
# moves in: a lift out of the slot that is always more than the slot is tall, so
# the piece clears the bar it came off, then the fall -- and the caller's own
# bottom edge as the line it is gone by. Everything is varied per throw, so two
# drops in a row do not leave on the same arc.
#
# These are written for a 16-unit icon and are multiplied by the caller's `scale`
# (launch), so the throw is as strong in pixels as the thing it throws is big -- a
# GUI scale of 2 throws twice as far. GRAVITY, LIFT and PUSH are the three that
# measure LENGTHS, so they scale; the turn (SPIN, radians) and the drag and the
# clock (DRAG, LIFE, FADE) are already in the shape's own terms and do not, which
# is what keeps the arc the SAME arc at every scale rather than a different one.
const GRAVITY = 460.0    # px/s^2, the fall
const LIFT = 175.0       # px/s, straight up out of the slot
const LIFT_VARY = 0.25   # fraction of the lift, per throw
const PUSH = 60.0        # px/s along the caller's side
const PUSH_VARY = 0.35   # fraction of that, per throw
const DRAG = 1.4         # per second: the sideways push bleeds off as it falls,
                         # so the piece leaves down rather than down-and-away
const SPIN = 4.2         # radians/s, either way
const SPIN_VARY = 0.45   # fraction of the spin, per throw
const LIFE = 1.8         # seconds; a piece that somehow stayed on screen is gone
const FADE = 0.2         # the last stretch of that, fading out
# A drop is one piece per press; the cap is only a bound on a pile-up from a key
# held down while older pieces are still in the air.
const MAX_PIECES = 16

# The pieces still in the air: where the middle of the art is, which way it is
# turned, and how it is moving.
var _pieces: Array = []


## Throw one piece of `art` out of the HUD. `rect` is where it was drawn, in the
## space the caller draws in; `side` is -1..1 for which way it leaves the slot
## sideways (0 is straight up); `exit_y` is the caller's bottom edge -- once the
## piece is past it, nothing of the piece can be seen and it is gone; `scale` is
## the caller's GUI scale, so the throw is in the same proportion to the icon as
## the constants above are to a 16-unit one (1.0 for an unscaled UI).
func launch(art: Texture2D, rect: Rect2, side: float, exit_y: float, scale: float = 1.0) -> void:
	if art == null or _pieces.size() >= MAX_PIECES:
		return
	# A scale of zero or less would leave the piece where it was drawn, which is a
	# slot that still looks occupied; one small factor is a weaker throw, not none.
	scale = maxf(scale, 0.05)
	var lift := LIFT * scale * randf_range(1.0 - LIFT_VARY, 1.0 + LIFT_VARY)
	var push := PUSH * scale * randf_range(1.0 - PUSH_VARY, 1.0 + PUSH_VARY)
	# The spin follows the throw: a piece pushed off to one side turns that way,
	# and one thrown straight up takes whichever way it likes.
	var spin_dir := signf(side) if not is_zero_approx(side) else (1.0 if randf() < 0.5 else -1.0)
	_pieces.append({
		"tex": art,
		"centre": rect.get_center(),
		"size": rect.size,
		"vel": Vector2(side * push, -lift),
		"angle": 0.0,
		"spin": spin_dir * SPIN * randf_range(1.0 - SPIN_VARY, 1.0 + SPIN_VARY),
		"age": 0.0,
		"life": LIFE,
		"exit_y": exit_y,
		"gravity": GRAVITY * scale,
	})


## Move the pieces on by `delta`; returns whether any are still in the air, so the
## caller knows a redraw is still needed.
func advance(delta: float) -> bool:
	for i in range(_pieces.size() - 1, -1, -1):
		var piece: Dictionary = _pieces[i]
		var vel: Vector2 = piece["vel"]
		var centre: Vector2 = piece["centre"]
		var size: Vector2 = piece["size"]
		piece["age"] = piece["age"] + delta
		vel.y += piece["gravity"] * delta
		vel.x -= vel.x * minf(DRAG * delta, 1.0)
		centre += vel * delta
		piece["vel"] = vel
		piece["centre"] = centre
		piece["angle"] = piece["angle"] + piece["spin"] * delta
		# Gone when it is past the bottom edge, or when its life runs out above it
		# (the fade in draw() is what covers the second case).
		if piece["age"] >= piece["life"] or centre.y - size.y * 0.5 > piece["exit_y"]:
			_pieces[i] = _pieces[_pieces.size() - 1]
			_pieces.remove_at(_pieces.size() - 1)
	return not _pieces.is_empty()


## How many pieces are in the air, for a probe or a caller that wants to know.
func count() -> int:
	return _pieces.size()


## Throw away everything still in the air: for a surface that stops drawing them
## (a screen closing), where a fall nobody can see is not worth carrying on with.
func clear() -> void:
	_pieces.clear()


## Draw the pieces, each turned about its own middle. Call it from a CanvasItem's
## `_draw()`, in the same space `rect` was given in; the transform is restored
## afterwards so it cannot leak into whatever the caller draws next.
func draw(canvas: CanvasItem) -> void:
	for piece in _pieces:
		var alpha := 1.0
		var left: float = piece["life"] - piece["age"]
		if left < FADE:
			alpha = clampf(left / FADE, 0.0, 1.0)
		var size: Vector2 = piece["size"]
		canvas.draw_set_transform(piece["centre"], piece["angle"], Vector2.ONE)
		canvas.draw_texture_rect(piece["tex"], Rect2(-size * 0.5, size), false,
			Color(1.0, 1.0, 1.0, alpha))
	canvas.draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)
