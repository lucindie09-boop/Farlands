extends Control
## The screen-change cover: a sheet of black blocks falls on the screen, collides,
## piles up, settles, holds, and then drops away.
##
## The motion is the ENGINE's 2D physics, not a curve. Every block is a
## `RigidBody2D` with its own mass, spin and friction, the screen's bottom and
## sides are static bodies, and the pile is whatever those laws produce. Nothing
## here decides when a block arrives: they are all released at once and the
## stacking order comes out of the fall and the collisions. That is the whole
## point of the effect — a wall that lands in rows on a schedule reads as an
## animation, and this reads as weight.
##
## "Settled" is therefore a measurement, not a timer: the fall ends when the pile's
## top edge has stopped climbing and enough of the sheet is on screen. The hold
## after that is the beat the effect is for — the wall standing still on screen —
## and it is served before the cover can be lifted, with every block frozen so the
## hold is motionless rather than merely slow.
##
## Three details exist to make the physics behave rather than to decorate it, and
## each is explained where it is used: the transition runs its own physics rate,
## the collider is smaller than the drawn square, and a block freezes into the pile
## only once it has come to rest on something solid.
##
## One thing is deliberately NOT physical: a rigid pile of squares does not tile.
## At the frame the world is handed back, a gap in the pile would show the hotbar
## appearing through it. So a black backing fades in over 0.1 s at the end of the
## hold, under the blocks, giving one provably opaque frame for the swap; it fades
## back out as the pile falls away. It is never visible as a fade, because it
## happens with a black wall already in front of it.
##
## `reduce_motion` (read from the settings menu by menu_layer.gd) drops the bodies
## and does the same steps as a short fade, holding for the same beat.

signal covered
signal revealed

# The sheet: twelve blocks across the screen's width, in square cells, and one row
# more than fills the screen so the pile closes over the top edge rather than
# leaving a strip of world above it.
const GRID_COLS := 12
# One row more than fills the screen. Enough that the sheet covers rather than
# exactly fits, small enough that the pile is not under so much pressure that the
# solver keeps it awake forever.
const EXTRA_ROWS := 1
# Gravity, as a multiple of the project's 2D gravity (980 px/s^2). A block this
# size is a heavy object on a tall screen; at 1x it would drift down like a leaf. It
# is also a collision budget: a screen and a half of drop at this rate ends at about
# 3,100 px/s, which is 52 px per step at 60 Hz and 10 px at the rate this effect runs
# at. The fall has to stay in the range where contacts are resolved, or there is
# nothing to settle.
const GRAVITY_SCALE := 4.4
# Released, not thrown: a wall of blocks this size has no one pushing it, and the
# difference is what makes it read as heavy rather than as a shower. All of the
# speed comes from gravity.
const ENTRY_SPEED := 0.0
const ENTRY_SPEED_VARY := 25.0
# A block the width of a twelfth of the screen turns over slowly. Fast spin is the
# single strongest cue for "small and light", so it is deliberately small here.
const SPIN_MIN := 0.3
const SPIN_MAX := 1.4
# How long the pile must stand still before it may be lifted. This is the
# "settle" the effect is for.
const HOLD_TIME := 0.6
# Rest is measured as DISPLACEMENT, not as reported velocity, and that is the
# important detail. A block buried in a pile reports a few hundred pixels per
# second downward while its position does not change: the solver pushes it back up
# every tick by as much as gravity pulls it down. That is a limit cycle, and a
# velocity test reads it as "still moving" forever. "Has not gone anywhere for
# 0.06 s while resting on something solid" is both what the eye sees and what a
# landed block is. A block that meets it becomes part of the pile: frozen, which in
# the engine is a static body. That is the cascade the effect needs. An awake block
# pressing on frozen ones cannot shiver them, so each landing calms the next, and the
# pile stops as a pile instead of as ninety-six bodies negotiating with the solver.
const STILL_RADIUS := 8.0
const STILL_ANGLE := 0.2
const STILL_TIME := 0.06
# A block may only join the pile with less than this much overlap (pixels).
const CRUSH_LIMIT := 6.0
# What settles a pile is its own surface. The blocks inside keep negotiating with
# the solver, but nothing of that is visible - the wall is pure black and so is the
# backing behind it - so the thing the eye reads as "it has landed" is the top edge:
# the height stops climbing. That is the measurement, and it is why the cover is
# finished by geometry rather than by a clock: the edge has to stand still for
# `TOP_STILL_TIME` AND the wall has to reach the top of the screen.
const TOP_STILL_TIME := 0.3
const TOP_STILL_SLACK := 0.25
# The wall is up when this much of the sheet has reached the screen. Not all of it:
# the last block in is the one the measurement would otherwise wait for.
const COVER_FILL := 0.9
# Physics rate for the transition. At the project's 60 Hz a block landing at 3,100
# px/s moves over fifty pixels between two contact checks - a third of the block - so
# the solver discovers the floor late and resolves the overlap it finds, which is both
# the squash and the kick after it. Five times the rate makes that step ten pixels. It
# is set only while the curtain is up and put back when it is done, and it changes
# nothing about the simulation's rules: same gravity, same masses, same friction,
# just resolved properly.
const COVER_TICKS := 300
# The opaque frame behind the blocks, for the swap.
const BACKING_TIME := 0.1
const REVEAL_BACKING_TIME := 0.12
# Failsafes, not schedule: a pile that will not sleep (a wedged block jittering
# forever) is forced flat rather than holding the player on a black screen.
const COVER_GUARD := 5.0
const REVEAL_GUARD := 1.5

const PHASE_IDLE := 0
const PHASE_COVER := 1
const PHASE_HOLD := 2
const PHASE_COVERED := 4
const PHASE_REVEAL := 5

# The wall is pure black, every block of it. Variation here reads as texture, and
# texture is the one thing a cover must not have.
const CELL_COLOR := Color(0, 0, 0, 1)
# The collider is smaller than the drawn square. Every block is pure black and so is
# the backing behind it, so the strip where two neighbours overlap is not visible -
# while the gap the solver gets is what stops a packed pile from being driven through
# itself. Without it the engine resolves overlaps of a third of a block with the
# velocities that come with that, which is the shiver.
const PHYS_SCALE := 0.86
# ...and the drawn square is a little larger than the cell it falls in, so the wall
# still closes over the screen even though the bodies pack with gaps. Both halves of
# that are invisible for the same reason: the blocks are pure black, so neither an
# overlap nor a hairline gap between two of them can be seen.
# The drawn square has two sizes, because the fall and the wall want opposite things.
# Falling, a block must be smaller than the gap it falls through or the sheet reads as
# one object however the bodies are arranged: the gaps are what make it blocks. Landed,
# the wall must close, or the screen is never filled. So each square swells where it
# lies until the wall is closed. That size is not a constant, because the pile is not:
# it is whatever the squares have to reach to cover the viewport this time, measured
# while they grow, with a ceiling only as a failsafe.
const FALL_SCALE := 0.92
const SEAL_RATE := 2.4
const MAX_SCALE := 1.9
const COVER_TARGET := 0.999
const SEAL_SAMPLE := 0.05
# The lattice that takes over from the pile, and the drop that ends the cover. Both
# are drawn, not simulated: the fall was the effect, and what is left has to be exact.
#
# The drop is per cell, not per wall. A lattice of pure black squares that leaves as
# one sheet is a black rectangle falling - there is nothing in it to see - so each
# cell departs on its own beat, bottom rows first, with a little side drift and spin.
# The wall then comes apart into cubes as it goes, and the gaps between them are the
# world showing through, which is what a reveal is for.
const REVEAL_ACCEL := 3800.0
const REVEAL_ROW_STAGGER := 0.045
const REVEAL_SPREAD := 0.06
const REVEAL_DRIFT := 45.0
const REVEAL_SPIN := 1.1

var _bodies: Array = []
# Where each body was when its stillness window opened, and how long that window
# has run. The engine's own sleep is island-wide: one block still rocking keeps
# every block it touches awake, so a dense pile shivers instead of settling.
var _still_anchor: Dictionary = {}
var _still_time: Dictionary = {}
var _draw_scale := FALL_SCALE
var _sealed := false
var _seal_timer := 0.0
# The lattice: what the wall becomes once the pile has landed, and what drops out.
var _grid_up := false
var _cells: Array = []
# Highest block (smallest y) and how long that height has held.
var _top_y := INF
var _top_settled := 0.0
var _walls: Array = []
var _material: PhysicsMaterial
var _phase := PHASE_IDLE
var _side := 0.0
var _elapsed := 0.0
var _hold := 0.0
var _backing := 0.0
var _reduce_motion := false
var _has_emitted := false
var _reveal_requested := false
var _floor: StaticBody2D
var _saved_ticks := 0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	set_anchors_preset(Control.PRESET_FULL_RECT)
	# No Control parent to anchor against under a CanvasLayer: take the size from
	# the viewport, and follow it if the window changes.
	get_viewport().size_changed.connect(_fit)
	_fit()
	set_process(false)
	set_physics_process(false)
	hide()


func _fit() -> void:
	size = get_viewport().get_visible_rect().size


## A curtain freed mid-transition must not leave the project running at the
## transition's physics rate.
func _exit_tree() -> void:
	_restore_physics_rate()


## Start covering the screen. `reduce_motion` swaps the pile for a short fade.
func play_cover(reduce_motion: bool = false) -> void:
	_clear_world()
	_reduce_motion = reduce_motion
	_phase = PHASE_COVER
	_elapsed = 0.0
	_hold = 0.0
	_backing = 0.0
	_has_emitted = false
	_reveal_requested = false
	_draw_scale = FALL_SCALE
	_sealed = false
	_seal_timer = 0.0
	_grid_up = false
	_cells.clear()
	show()
	if not _reduce_motion:
		_build_world()
		set_physics_rate(COVER_TICKS)
	set_process(true)
	set_physics_process(true)
	queue_redraw()


## Ask to uncover the screen. Honoured once the pile has settled and held.
func play_reveal() -> void:
	_reveal_requested = true
	if _phase == PHASE_COVERED:
		_begin_reveal()


## Jump to the end of the part that is running. Idempotent, safe while idle.
func skip() -> void:
	if _phase == PHASE_IDLE:
		return
	if _phase == PHASE_REVEAL:
		_finish_reveal()
		return
	# Anything before the swap becomes the swap: the lattice goes up at once, and it is
	# opaque by construction.
	_show_grid()
	_phase = PHASE_COVERED
	_emit_covered()


func is_active() -> bool:
	return _phase != PHASE_IDLE


func _unhandled_input(event: InputEvent) -> void:
	if _elapsed < 0.08:
		return
	if _phase != PHASE_COVER and _phase != PHASE_HOLD:
		return
	var pressed := false
	if event is InputEventKey:
		pressed = event.pressed and not event.echo
	elif event is InputEventMouseButton:
		pressed = event.pressed
	elif event is InputEventJoypadButton:
		pressed = event.pressed
	if pressed:
		skip()
		get_viewport().set_input_as_handled()


func _process(delta: float) -> void:
	_elapsed += delta
	if _reduce_motion:
		_process_fade(delta)
	else:
		match _phase:
			PHASE_HOLD:
				_process_hold(delta)
			PHASE_REVEAL:
				_process_reveal(delta)
	queue_redraw()


## The pile is measured on physics ticks, not process frames: this is motion, and a
## frame that renders without a physics step would count as a frame in which
## nothing moved. On a 300 fps renderer that made a falling sheet look settled.
func _physics_process(delta: float) -> void:
	if _reduce_motion or _phase != PHASE_COVER:
		return
	_process_cover(delta)


## Reduce motion: one rect, a hold of the same length, and back again. No bodies
## exist in this mode, so none of the physics phases apply.
func _process_fade(delta: float) -> void:
	match _phase:
		PHASE_COVER:
			_backing = minf(_backing + delta / BACKING_TIME, 1.0)
			if _backing >= 1.0:
				_phase = PHASE_HOLD
				_hold = 0.0
		PHASE_HOLD:
			_hold += delta
			if _hold >= HOLD_TIME:
				_phase = PHASE_COVERED
				_emit_covered()
		PHASE_REVEAL:
			_backing = maxf(_backing - delta / REVEAL_BACKING_TIME, 0.0)
			if _backing <= 0.0:
				_finish_reveal()


## Two things happen per tick: a block that has stopped drifting on something
## solid becomes part of the pile, and the pile as a whole is watched for having
## finished rising.
func _process_cover(delta: float) -> void:
	for body in _bodies:
		if not is_instance_valid(body) or body.freeze:
			continue
		var here := Vector3(body.position.x, body.position.y, body.rotation)
		var anchor: Variant = _still_anchor.get(body)
		if anchor == null:
			_still_anchor[body] = here
			_still_time[body] = 0.0
			continue
		var drift: Vector3 = here - anchor
		if Vector2(drift.x, drift.y).length() > STILL_RADIUS or absf(drift.z) > STILL_ANGLE:
			_still_anchor[body] = here
			_still_time[body] = 0.0
			continue
		var still := float(_still_time.get(body, 0.0)) + delta
		_still_time[body] = still
		# Support is what separates "landed" from "still falling". Touching another
		# block that is itself falling is not support: the sheet would freeze in mid-air
		# the moment its members touched each other.
		if still >= STILL_TIME and _has_support(body) and _overlap_with(body) <= CRUSH_LIMIT:
			body.linear_velocity = Vector2.ZERO
			body.angular_velocity = 0.0
			body.freeze = true
	# The pile's surface is the tell. Its blocks keep squashing into each other while
	# it compacts, but that is not visible through black on black; what is visible is
	# where its top edge is. "Inside" means the block has reached the screen, so a
	# block still hanging in the sky - one wedged against a wall, or the row that has
	# not fallen yet - cannot hold the measurement open or close it early.
	var inside := 0
	var surface := INF
	for body in _bodies:
		if not is_instance_valid(body):
			continue
		if body.position.y + _side * 0.5 >= 0.0:
			inside += 1
			surface = minf(surface, body.position.y)
	var filled := float(inside) / float(_bodies.size())
	if filled < COVER_FILL or surface == INF:
		_top_settled = 0.0
	elif absf(surface - _top_y) <= _side * TOP_STILL_SLACK:
		_top_settled += delta
	else:
		_top_y = surface
		_top_settled = 0.0
	if _elapsed >= COVER_GUARD or (filled >= COVER_FILL and _top_settled >= TOP_STILL_TIME):
		_freeze_bodies()
		_enter_hold()


## The transition's own physics rate, remembered so it can be put back exactly.
func set_physics_rate(ticks: int) -> void:
	if ticks <= 0:
		return
	if _saved_ticks == 0:
		_saved_ticks = Engine.physics_ticks_per_second
	Engine.physics_ticks_per_second = maxi(_saved_ticks, ticks)


func _restore_physics_rate() -> void:
	if _saved_ticks != 0:
		Engine.physics_ticks_per_second = _saved_ticks
		_saved_ticks = 0


func _enter_hold() -> void:
	_phase = PHASE_HOLD
	_hold = 0.0





## The hold is also when the wall closes: every square swells where it landed, and the
## swell stops when the viewport is covered. A rigid pile does not tile, so without
## this the wall would keep a few hairline cracks of world for the whole beat.
func _process_hold(delta: float) -> void:
	if _phase != PHASE_HOLD:
		return
	_hold += delta
	if not _sealed:
		_draw_scale = minf(MAX_SCALE, _draw_scale + SEAL_RATE * delta)
		_seal_timer += delta
		if _seal_timer >= SEAL_SAMPLE:
			_seal_timer = 0.0
			_sealed = _coverage(24, 14) >= COVER_TARGET or _draw_scale >= MAX_SCALE
	# The cover is not handed over until the wall is closed and has stood still for the
	# whole beat: the drop must never begin from a wall the world can be seen through.
	# The last check before the swap is at a finer sampling than the one that drove the
	# growth - a coarse sample can step over a crack - and if it finds one the squares
	# keep swelling instead of handing over.
	if _sealed and _hold >= HOLD_TIME:
		if _draw_scale < MAX_SCALE and _coverage(48, 27) < COVER_TARGET:
			_sealed = false
			return
		_show_grid()
		_phase = PHASE_COVERED
		_emit_covered()


## How much of the viewport the drawn squares cover. Coarse on purpose: it is asked only
## whether the wall has closed, and the squares are swelling continuously, so a sample
## every fiftieth of a second is plenty.
func _coverage(columns: int, rows: int) -> float:
	var half := _side * _draw_scale * 0.5
	var hits := 0
	for row in range(rows):
		for col in range(columns):
			var point := Vector2((float(col) + 0.5) * size.x / float(columns),
				(float(row) + 0.5) * size.y / float(rows))
			for body in _bodies:
				if not is_instance_valid(body):
					continue
				var local: Vector2 = body.transform.affine_inverse() * point
				if absf(local.x) <= half and absf(local.y) <= half:
					hits += 1
					break
	return float(hits) / float(columns * rows)


## The pile has done its job: it was the fall. From here the wall has to be exact, so it
## is replaced by a lattice that tiles the viewport, and the bodies are let go. The swap
## cannot be seen - both are the same pure black, and the lattice covers at least what
## the pile covered - but the drop after it is a clean grid of cubes rather than an
## arbitrary heap.
func _show_grid() -> void:
	_grid_up = true
	_cells.clear()
	var cell := size.x / float(GRID_COLS)
	var rows := int(ceil(size.y / cell))
	for row in range(rows):
		# The bottom row goes first, so the wall is eaten away from underneath rather
		# than dissolving everywhere at once.
		var depth := float(rows - 1 - row)
		for col in range(GRID_COLS):
			_cells.append({
				"x": float(col) * cell,
				"y": float(row) * cell,
				"size": cell,
				"delay": depth * REVEAL_ROW_STAGGER + randf() * REVEAL_SPREAD,
				"drift": randf_range(-REVEAL_DRIFT, REVEAL_DRIFT),
				"vy": 0.0,
				"offset": Vector2.ZERO,
				"angle": 0.0,
				"spin": randf_range(-REVEAL_SPIN, REVEAL_SPIN),
			})
	_clear_world()


func _process_reveal(delta: float) -> void:
	var falling := false
	for cell in _cells:
		if cell.delay > 0.0:
			cell.delay -= delta
			falling = true
			continue
		cell.vy += REVEAL_ACCEL * delta
		cell.offset += Vector2(cell.drift, cell.vy) * delta
		cell.angle += cell.spin * delta
		if cell.y + cell.offset.y < size.y:
			falling = true
	if not falling or _elapsed >= REVEAL_GUARD:
		_finish_reveal()


func _begin_reveal() -> void:
	_reveal_requested = false
	# Reduce Motion has no lattice: its reveal is the fade, and drawing cubes over it
	# would be the very thing the setting is asking not to see.
	if not _reduce_motion:
		if not _grid_up:
			_show_grid()
		else:
			# The lattice was already up for the hold: start it from rest, intact.
			for cell in _cells:
				cell.vy = 0.0
				cell.offset = Vector2.ZERO
				cell.angle = 0.0
	_phase = PHASE_REVEAL
	_elapsed = 0.0
	set_process(true)
	set_physics_process(false)


func _finish_reveal() -> void:
	if _phase == PHASE_IDLE:
		return
	_phase = PHASE_IDLE
	_clear_world()
	_grid_up = false
	_cells.clear()
	_restore_physics_rate()
	_backing = 0.0
	set_process(false)
	set_physics_process(false)
	hide()
	revealed.emit()


func _emit_covered() -> void:
	if _has_emitted:
		return
	_has_emitted = true
	covered.emit()


## True when the body is standing on something that will not move: the floor, a
## wall, or a block that has already landed.
func _has_support(body: RigidBody2D) -> bool:
	for other in body.get_colliding_bodies():
		if other is StaticBody2D:
			return true
		if other is RigidBody2D and other.freeze:
			return true
	return false


## How far the block is inside its worst neighbour, in pixels. Freezing is for a
## block that has come to rest, not for one the pile is still pushing out of
## itself: freeze a crushed block and the crush is permanent, which is how one
## unlucky landing becomes a block a third of the way inside another.
func _overlap_with(body: RigidBody2D) -> float:
	var worst := 0.0
	var half := _side * PHYS_SCALE * 0.5
	var right := Vector2.RIGHT.rotated(body.rotation)
	var down := Vector2.DOWN.rotated(body.rotation)
	for other in body.get_colliding_bodies():
		if not (other is RigidBody2D):
			continue
		var right_b := Vector2.RIGHT.rotated(other.rotation)
		var down_b := Vector2.DOWN.rotated(other.rotation)
		var delta: Vector2 = other.position - body.position
		var depth := INF
		for axis in [right, down, right_b, down_b]:
			var ha: float = (absf(axis.dot(right)) + absf(axis.dot(down))) * half
			var hb: float = (absf(axis.dot(right_b)) + absf(axis.dot(down_b))) * half
			depth = minf(depth, ha + hb - absf(delta.dot(axis)))
		worst = maxf(worst, depth)
	return worst


func _all_settled() -> bool:
	if _bodies.is_empty():
		return false
	for body in _bodies:
		if not is_instance_valid(body):
			continue
		if not body.freeze:
			return false
	return true


func _freeze_bodies() -> void:
	for body in _bodies:
		if is_instance_valid(body):
			body.linear_velocity = Vector2.ZERO
			body.angular_velocity = 0.0
			body.freeze = true


## The sheet and the box it falls into. Released all at once: the order they settle
## in is the physics' answer, not ours.
func _build_world() -> void:
	_side = size.x / float(GRID_COLS)
	_material = PhysicsMaterial.new()
	_material.friction = 0.98
	_material.bounce = 0.0

	# The box: a floor and two sides, tall enough to contain the whole fall and thick
	# enough that nothing can step through. A body arrives at a few thousand pixels
	# per second, which is tens of pixels per tick, so a thin wall is a wall with a
	# gap in it: one block escaped that way and free-fell for the rest of the
	# transition, which is what kept the pile from ever coming to rest. The floor also
	# runs wider than the box, so there is no seam at either bottom corner.
	var half := size.y
	var wall := 600.0
	# The box is half a block wider than the screen at each side. A box exactly the
	# screen's width cannot hold the sheet: rotated squares pack less densely than a
	# grid, so the pile is under compression, and a compressed pile is a pile whose
	# top rows report a few hundred pixels per second downward while the solver
	# pushes them back up - a limit cycle, which is exactly the shiver this effect
	# must not have. The slack is off-screen, so letting the pile spread costs no
	# coverage: it still presses against both walls and passes behind both edges.
	var slack := _side * 0.3
	_add_wall(Vector2(size.x * 0.5, size.y + half), Vector2(size.x + slack * 2.0 + wall * 4.0, half * 2.0), true)
	_add_wall(Vector2(-slack - wall * 0.5, 0.0), Vector2(wall, half * 6.0), false)
	_add_wall(Vector2(size.x + slack + wall * 0.5, 0.0), Vector2(wall, half * 6.0), false)

	var rows_fit := int(ceil(size.y / _side))
	var rows := rows_fit + EXTRA_ROWS
	# The sheet is not a slab, and that is a physical property rather than a styling
	# one: bodies released from the same height with the same velocity fall as one
	# object, exactly, so a tidy grid reads as a wall dropping rather than as blocks
	# landing. Three things break it up. Rows are spaced wider than a block, so there
	# are gaps between them all the way down. Each column starts at its own height, so
	# the columns arrive one after another instead of together. And every block gets a
	# little of its own vertical jitter, so even the members of one row do not cross the
	# top edge in step. None of it is a schedule: they are still all released at the
	# same instant, and the order they land in is the fall's answer.
	# Wider than the DRAWN square, not just wider than the cell: the blocks are drawn
	# at 1.32 of a cell, so any spacing below that still reads as one continuous slab
	# of black even though the bodies are apart.
	var row_gap := _side * 1.95
	var column_drop := []
	for col in range(GRID_COLS):
		column_drop.append(randf_range(0.0, _side * 1.6))
	for row in range(rows):
		for col in range(GRID_COLS):
			# Alternate columns sit half a row lower, so no block falls directly under
			# its neighbour and the sheet is a bond rather than a grid of columns.
			var bond := (float(row_gap) * 0.5) if (col % 2 == 1) else 0.0
			var at := Vector2(
				(float(col) + 0.5) * _side + randf_range(-8.0, 8.0),
				-_side * 0.5 - float(row) * row_gap - bond - float(column_drop[col])
					+ randf_range(-0.18, 0.18) * _side)
			_add_body(at)


func _add_wall(centre: Vector2, extents: Vector2, is_floor: bool) -> void:
	var wall := StaticBody2D.new()
	var shape := CollisionShape2D.new()
	var rect := RectangleShape2D.new()
	rect.size = extents
	shape.shape = rect
	wall.add_child(shape)
	wall.position = centre
	wall.physics_material_override = _material
	add_child(wall)
	_walls.append(wall)
	if is_floor:
		_floor = wall


func _add_body(at: Vector2) -> void:
	var body := RigidBody2D.new()
	body.gravity_scale = GRAVITY_SCALE
	# Almost no air drag. Damping is what a feather has; a heavy block accelerates
	# under gravity and stops when it hits something. Only the spin is damped, because
	# a block rocking in place is the thing that keeps a pile awake.
	body.linear_damp = 0.08
	body.angular_damp = 0.9
	body.contact_monitor = true
	body.max_contacts_reported = 4
	# Shape-cast CCD. A block crosses tens of pixels per tick when it lands, and
	# without this the first contact is discovered after the fact: the block has
	# already sunk a third of a block into the pile, and the correction that follows
	# is the impact that reads as shaking.
	body.continuous_cd = RigidBody2D.CCD_MODE_CAST_SHAPE
	body.physics_material_override = _material
	body.position = at
	body.linear_velocity = Vector2(randf_range(-18.0, 18.0),
		ENTRY_SPEED + randf_range(-ENTRY_SPEED_VARY, ENTRY_SPEED_VARY))
	# Spun, not pre-rotated: no body starts overlapping its neighbour, and the
	# tumble still starts on the first frame.
	body.angular_velocity = randf_range(SPIN_MIN, SPIN_MAX) * (1.0 if randf() < 0.5 else -1.0)
	# Slightly uneven masses: a sheet of identical blocks reads as a pattern, and a
	# pile of them compacts the same way every time. Mass is the one property the
	# engine uses that does not cost a visible difference per block.
	body.mass = randf_range(0.9, 1.1)
	var shape := CollisionShape2D.new()
	var rect := RectangleShape2D.new()
	rect.size = Vector2(_side * PHYS_SCALE, _side * PHYS_SCALE)
	shape.shape = rect
	body.add_child(shape)
	add_child(body)
	_bodies.append(body)


func _clear_world() -> void:
	for body in _bodies:
		if is_instance_valid(body):
			body.queue_free()
	for wall in _walls:
		if is_instance_valid(wall):
			wall.queue_free()
	_bodies.clear()
	_still_anchor.clear()
	_still_time.clear()
	_top_y = INF
	_top_settled = 0.0
	_walls.clear()
	_floor = null


func _draw() -> void:
	if _phase == PHASE_IDLE and _backing <= 0.0:
		return
	if _backing > 0.0:
		draw_rect(Rect2(Vector2.ZERO, size), Color(0, 0, 0, _backing))
	if _reduce_motion:
		return
	if _grid_up:
		_draw_cells()
		return
	for body in _bodies:
		if not is_instance_valid(body):
			continue
		var drawn := _side * _draw_scale
		draw_set_transform(body.position, body.rotation, Vector2.ONE)
		draw_rect(Rect2(Vector2(-drawn, -drawn) * 0.5, Vector2(drawn, drawn)), CELL_COLOR)
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)


## The lattice: twelve square cells across, however many rows the screen needs. Each
## cell is drawn a pixel larger than its cell while it is still in place, so the tiling
## has no seams to show the world through; once it starts falling, that same pixel is
## what puts a gap between it and its neighbours.
func _draw_cells() -> void:
	for cell in _cells:
		var size_px: float = cell.size
		var centre: Vector2 = Vector2(cell.x + size_px * 0.5, cell.y + size_px * 0.5) + cell.offset
		draw_set_transform(centre, cell.angle, Vector2.ONE)
		draw_rect(Rect2(Vector2(-size_px, -size_px) * 0.5,
			Vector2(size_px + 1.0, size_px + 1.0)), CELL_COLOR)
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)
