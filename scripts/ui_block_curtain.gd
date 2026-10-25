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
# How the wall's closure is decided. The cheap stamp says when the squares are close;
# only then is the exact scanline pass run, because that one is the honest answer and it
# costs real time. Two pixels between lines on a 1080p screen: anything narrower than
# that is not a gap a player can see.
const SEAL_LINE_STEP := 2.0
const SEAL_LINE_SLACK := 0.45

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
# Fuses, not schedule, and measured on the wall clock rather than in frames. A pile that
# will not report itself settled - a wedged block, a first entry where the physics is
# running behind the frames - must not be able to strand the transition: the player would
# sit behind a see-through wall until something was pressed. Every one of these ends the
# phase it guards by moving forward, never by giving up, and the wall always finishes as
# the lattice, which covers the screen exactly.
const COVER_FUSE := 4.0
const HOLD_FUSE := 2.0
const REVEAL_GUARD := 1.5

# How long the closed wall stands before it is dropped. The hold above is the beat the
# effect is for; this is the part of it that is guaranteed to be a still, closed wall,
# so the swell that closes the last crack cannot eat the pause.
const SEALED_HOLD := 0.5
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
# A ceiling for the swell, not the goal: it exists so a body flung out of the box cannot
# make the squares grow forever. It is set well past the point where the wall closes, so
# reaching it means something went wrong, not that the screen is as closed as it gets.
const MAX_SCALE := 3.4
# Full coverage, not nearly: the wall keeps swelling until no scanline across it has a
# visible gap, because "99.9% of the screen" is still a crack somewhere.
const COVER_TARGET := 1.0
const SEAL_SAMPLE := 0.06
# The cheap stamp grid, in cells across and down: twelve pixels a cell on a 1080p screen.
# It is a trigger, not the verdict - a grid this coarse can step over a crack - which is
# why the exact scanline pass runs as soon as it reports the wall is worth measuring.
const SEAL_CELLS := 160
const SEAL_ROWS := 90
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
# The skip. A press during the transition does not cut the screen away, it blows it
# apart from where the player pressed: every block takes an impulse away from that
# point, keeps its spin, and is gone inside half a second. It is the same physics the
# fall used - the pile's own bodies are thrown, not a drawn copy of them - and it is
# what makes an early exit feel like a decision rather than a glitch.
const BLAST_SPEED := 2900.0
const BLAST_VARY := 0.28
const BLAST_SPIN := 7.0
const BLAST_DAMP := 1.5
# A thrown block that has left the screen is done; the margin keeps a corner of a
# spinning square from popping out of view a frame after it was called gone.
const BLAST_MARGIN := 8.0

var _bodies: Array = []
# Where each body was when its stillness window opened, and how long that window
# has run. The engine's own sleep is island-wide: one block still rocking keeps
# every block it touches awake, so a dense pile shivers instead of settling.
var _still_anchor: Dictionary = {}
var _still_time: Dictionary = {}
var _draw_scale := FALL_SCALE
var _sealed := false
var _seal_timer := 0.0
# When the wall closed, on the hold's own clock. Negative while it is still swelling.
var _sealed_at := -1.0
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
# Wall clock for the fuses: `_elapsed` only advances while the frame loop runs, which is
# exactly what a stalled transition would stop.
var _started_ms := 0
var _hold_ms := 0
var _hold := 0.0
var _backing := 0.0
var _reduce_motion := false
var _has_emitted := false
var _reveal_requested := false
# Where the skip was pressed, and whether this reveal is that blast. INF is "no press":
# the normal reveal drops the lattice instead.
var _blast_from := Vector2.INF
var _blast := false
var _floor: StaticBody2D
var _saved_ticks := 0
var _saved_steps := 0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	# No anchors: the parent is a CanvasLayer, which is not a Control, so there is no parent
	# rect to anchor against - the full-rect preset would only make the layout overwrite the
	# size that `_fit` sets (and warn about it). The viewport is the authority instead, and
	# `size_changed` keeps it current.
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
	# The wall is built for the rectangle the node has now. That is kept current by the
	# viewport's own resize signal, so a window resized while the title was up is already
	# the size the sheet falls into.
	_reduce_motion = reduce_motion
	_phase = PHASE_COVER
	_elapsed = 0.0
	_started_ms = Time.get_ticks_msec()
	_hold_ms = _started_ms
	_hold = 0.0
	_backing = 0.0
	_has_emitted = false
	_reveal_requested = false
	_draw_scale = FALL_SCALE
	_sealed = false
	_seal_timer = 0.0
	_sealed_at = -1.0
	_grid_up = false
	_blast = false
	_blast_from = Vector2.INF
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


## Jump to the end of the part that is running, throwing the wall away from `from` (in
## viewport coordinates; the screen centre when the press had no position, as a pad's
## has none). Idempotent, safe while idle.
func skip(from: Vector2 = Vector2.INF) -> void:
	if _phase == PHASE_IDLE:
		return
	if _phase == PHASE_REVEAL:
		_finish_reveal()
		return
	_blast = true
	_blast_from = from
	if _phase != PHASE_COVERED:
		_phase = PHASE_COVERED
		_emit_covered()
		if _phase == PHASE_COVERED:
			_begin_reveal()
		return
	_reveal_requested = false
	_begin_reveal()


func is_active() -> bool:
	return _phase != PHASE_IDLE


func _unhandled_input(event: InputEvent) -> void:
	# Wall clock, like the fuses: a click on the row that started the transition arrives
	# within a frame or two, and must not be read as a request to skip it.
	if Time.get_ticks_msec() - _started_ms < 80:
		return
	if _phase != PHASE_COVER and _phase != PHASE_HOLD:
		return
	var pressed := false
	var from := Vector2.INF
	if event is InputEventKey:
		pressed = event.pressed and not event.echo
	elif event is InputEventMouseButton:
		pressed = event.pressed
		# Where the player pressed IS the blast: the wall comes apart around it.
		from = event.position
	elif event is InputEventJoypadButton:
		pressed = event.pressed
	if pressed:
		skip(from)
		get_viewport().set_input_as_handled()


func _process(delta: float) -> void:
	_elapsed += delta
	if _reduce_motion:
		_process_fade(delta)
	elif _phase == PHASE_REVEAL:
		_process_reveal(delta)
	queue_redraw()


## The cover and the hold are driven by physics ticks, not process frames. This is
## motion, and a frame that renders without a physics step would count as a frame in
## which nothing moved; worse, the swell that closes the wall used to run on the render
## loop only, so a transition could sit in the hold with no growth at all and wait to be
## clicked past. Physics is the loop that the pile itself is animated by, so the two
## cannot disagree about time.
func _physics_process(delta: float) -> void:
	if _reduce_motion:
		return
	match _phase:
		PHASE_COVER:
			_process_cover(delta)
		PHASE_HOLD:
			_process_hold(delta)


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
	var waited := float(Time.get_ticks_msec() - _started_ms) / 1000.0
	if waited >= COVER_FUSE or (filled >= COVER_FILL and _top_settled >= TOP_STILL_TIME):
		_freeze_bodies()
		_enter_hold()


## The transition's own physics rate, remembered so it can be put back exactly. The step
## ceiling goes up with it: at 300 Hz a 20 fps frame needs fifteen steps, and the default
## eight would run the whole fall in slow motion - which on a first entry, with the world
## streaming in, is exactly when the frame rate is lowest.
func set_physics_rate(ticks: int) -> void:
	if ticks <= 0:
		return
	if _saved_ticks == 0:
		_saved_ticks = Engine.physics_ticks_per_second
		_saved_steps = Engine.max_physics_steps_per_frame
	Engine.physics_ticks_per_second = maxi(_saved_ticks, ticks)
	Engine.max_physics_steps_per_frame = maxi(_saved_steps, 16)


func _restore_physics_rate() -> void:
	if _saved_ticks != 0:
		Engine.physics_ticks_per_second = _saved_ticks
		_saved_ticks = 0
	if _saved_steps != 0:
		Engine.max_physics_steps_per_frame = _saved_steps
		_saved_steps = 0


func _enter_hold() -> void:
	_phase = PHASE_HOLD
	_hold = 0.0
	_hold_ms = Time.get_ticks_msec()





## The hold is also when the wall closes: every square swells where it landed, and it
## keeps swelling until the whole viewport is covered. A rigid pile does not tile, so this
## is what seals the wall - and it must reach 100%, measured exactly, because a single
## open hairline is a line of world the player sees vanish at the swap.
func _process_hold(delta: float) -> void:
	if _phase != PHASE_HOLD:
		return
	_hold += delta
	if not _sealed:
		_draw_scale = minf(MAX_SCALE, _draw_scale + SEAL_RATE * delta)
		_seal_timer += delta
		if _seal_timer >= SEAL_SAMPLE:
			_seal_timer = 0.0
			# The cheap pass says when the wall is worth measuring properly. Until then
			# there is no point paying for the scanlines: a wall at 70% has gaps everywhere.
			if _coverage(SEAL_CELLS, SEAL_ROWS) >= COVER_TARGET or _draw_scale >= MAX_SCALE:
				var exact := _scanline_coverage(SEAL_LINE_STEP, SEAL_LINE_SLACK)
				_sealed = int(exact.gaps) == 0
	# The lattice takes over the moment the wall is closed: from that frame on the screen
# is covered by construction. It stays up for a beat before the drop, so the player sees
# a wall standing still rather than a swell running straight into an exit.
	# The fuse is the promise that the player is never left behind a wall with holes in
	# it: if the swell cannot close the screen in time, the exact lattice takes over
	# anyway. Both end in the same place, so there is no state in which the transition is
	# waiting for a click.
	if not _sealed and float(Time.get_ticks_msec() - _hold_ms) / 1000.0 >= HOLD_FUSE:
		_sealed = true
	if _sealed:
		if _sealed_at < 0.0:
			_sealed_at = _hold
		_show_grid()
		if _hold >= HOLD_TIME and _hold - _sealed_at >= SEALED_HOLD:
			_phase = PHASE_COVERED
			# Emitting runs the menu's continuation, which asks for the reveal, and that
			# rewrites the phase: nothing may be touched afterwards.
			_emit_covered()
			return


## A cheap read on whether the wall is close to closed: each square is stamped into the
## cells of a coarse grid inside its own bounding box, so the work is proportional to the
## blocks' area rather than to cells x blocks. It is the driving measurement while the
## squares swell, because it is fast enough to run several times a second - but a sample
## grid can step over a crack, so it is never the one that decides the swap.
func _coverage(columns: int, rows: int) -> float:
	var half := _side * _draw_scale * 0.5
	var cell_w := size.x / float(columns)
	var cell_h := size.y / float(rows)
	var grid := PackedByteArray()
	grid.resize(columns * rows)
	for node in _bodies:
		var body := node as RigidBody2D
		if not is_instance_valid(body):
			continue
		var c := cos(body.rotation)
		var s := sin(body.rotation)
		var at: Vector2 = body.position
		var reach := half * (absf(c) + absf(s))
		var x0 := maxi(int(floor((at.x - reach) / cell_w)), 0)
		var x1 := mini(int(ceil((at.x + reach) / cell_w)), columns)
		var y0 := maxi(int(floor((at.y - reach) / cell_h)), 0)
		var y1 := mini(int(ceil((at.y + reach) / cell_h)), rows)
		for gy in range(y0, y1):
			var py: float = (float(gy) + 0.5) * cell_h - at.y
			for gx in range(x0, x1):
				var index := gy * columns + gx
				if grid[index] == 1:
					continue
				var px: float = (float(gx) + 0.5) * cell_w - at.x
				if absf(px * c + py * s) <= half and absf(py * c - px * s) <= half:
					grid[index] = 1
	var painted := 0
	for value in grid:
		painted += value
	return float(painted) / float(columns * rows)


## Exact coverage, by scanline. A sample grid answers "was this point covered?" and can
## step over a crack between two of its points; this asks the question the eye asks - of
## every horizontal line across the screen, how much of it is not covered by any square -
## and finds every gap wider than the line spacing. Each square's intersection with a
## scanline is one interval in x, so the union is a merge of at most one interval per
## block, which is a native sort plus a walk. `gaps` counts lines that still have an
## uncovered stretch wider than `slack`, so a wall with a hairline somewhere is not
## declared closed.
func _scanline_coverage(step: float, slack: float) -> Dictionary:
	var half := _side * _draw_scale * 0.5
	var bodies: Array[RigidBody2D] = []
	var cosines := PackedFloat32Array()
	var sines := PackedFloat32Array()
	for node in _bodies:
		var body := node as RigidBody2D
		if not is_instance_valid(body):
			continue
		bodies.append(body)
		cosines.append(cos(body.rotation))
		sines.append(sin(body.rotation))
	var width := size.x
	var lines := int(ceil(size.y / step))
	var starts := PackedFloat32Array()
	var ends := PackedFloat32Array()
	var walked := 0.0
	var gaps := 0
	for line in range(lines):
		var y := (float(line) + 0.5) * step
		starts.clear()
		ends.clear()
		for i in range(bodies.size()):
			var at: Vector2 = bodies[i].position
			var dy := y - at.y
			var c := cosines[i]
			var s := sines[i]
			# The square is |(p - centre).u| <= half and |(p - centre).v| <= half for its
			# own axes u, v. Both are linear in x on a fixed line, so each is one interval,
			# and the feasible x is their intersection.
			var lo := -INF
			var hi := INF
			if absf(c) > 1e-6:
				var a := (-half - dy * s) / c
				var b := (half - dy * s) / c
				lo = maxf(lo, minf(a, b))
				hi = minf(hi, maxf(a, b))
			elif absf(dy * s) > half:
				continue
			if absf(s) > 1e-6:
				var a2 := (dy * c - half) / s
				var b2 := (dy * c + half) / s
				lo = maxf(lo, minf(a2, b2))
				hi = minf(hi, maxf(a2, b2))
			elif absf(dy * c) > half:
				continue
			if lo >= hi:
				continue
			starts.append(maxf(lo + at.x, 0.0))
			ends.append(minf(hi + at.x, width))
		if starts.is_empty():
			gaps += 1
			continue
		starts.sort()
		ends.sort()
		# Sweep the sorted boundaries: an opening adds coverage, a closing takes it away.
		var depth := 0
		var reach := 0.0
		var open := 0.0
		var si := 0
		var ei := 0
		while si < starts.size() or ei < ends.size():
			if ei >= ends.size() or (si < starts.size() and starts[si] <= ends[ei]):
				var x := starts[si]
				si += 1
				if depth == 0:
					open = x
				depth += 1
			else:
				var x2 := ends[ei]
				ei += 1
				depth -= 1
				if depth == 0:
					# `reach` is what the squares cover on this line; the rest of the width is
					# the gap the eye would see.
					reach += maxf(x2 - open, 0.0)
		walked += reach
		if width - reach > slack:
			gaps += 1
	return {
		"ratio": walked / (width * float(lines)),
		"gaps": gaps,
		"lines": lines,
	}


## The pile has done its job: it was the fall. From here the wall has to be exact, so it
## is replaced by a lattice that tiles the viewport, and the bodies are let go. The swap
## cannot be seen - both are the same pure black, and the lattice covers at least what
## the pile covered - but the drop after it is a clean grid of cubes rather than an
## arbitrary heap.
func _show_grid() -> void:
	if _grid_up:
		return
	_grid_up = true
	# The bodies stay for now - frozen, so they cost nothing - because the swap has to
	# happen on the same frame as the measurement that earned it. They are freed when
	# the curtain finishes.
	_freeze_bodies()
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
				"vel": Vector2.ZERO,
				"offset": Vector2.ZERO,
				"angle": 0.0,
				"spin": randf_range(-REVEAL_SPIN, REVEAL_SPIN),
			})


func _process_reveal(delta: float) -> void:
	var falling := false
	for cell in _cells:
		if cell.delay > 0.0:
			cell.delay -= delta
			falling = true
			continue
		if _blast:
			# Thrown, not dropped: the impulse is away from the press and the drag is what
			# takes the speed out of it, so nothing hangs at the edge of the screen.
			cell.offset += cell.vel * delta
			cell.vel *= maxf(0.0, 1.0 - BLAST_DAMP * delta)
			cell.angle += cell.spin * delta
			if _cell_on_screen(cell):
				falling = true
			continue
		cell.vy += REVEAL_ACCEL * delta
		cell.offset += Vector2(cell.drift, cell.vy) * delta
		cell.angle += cell.spin * delta
		if cell.y + cell.offset.y < size.y:
			falling = true
	if not falling or _elapsed >= REVEAL_GUARD:
		_finish_reveal()


func _cell_on_screen(cell: Dictionary) -> bool:
	var half: float = cell.size * 0.5
	var centre: Vector2 = Vector2(cell.x, cell.y) + cell.offset + Vector2(half, half)
	var reach := half * 1.5
	return centre.x + reach > -BLAST_MARGIN and centre.x - reach < size.x + BLAST_MARGIN \
		and centre.y + reach > -BLAST_MARGIN and centre.y - reach < size.y + BLAST_MARGIN


func _begin_reveal() -> void:
	_reveal_requested = false
	# Reduce Motion has no blocks: its reveal is the fade, and throwing squares over it
	# would be the very thing the setting is asking not to see.
	if not _reduce_motion:
		if _blast:
			_begin_blast()
		elif not _grid_up:
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


## The wall comes apart around the press. It is the lattice that is thrown, not the pile:
## a skip can land while the sheet is still in the air above the screen, and those blocks
## are not what the player is looking at - the lattice is, because it covers the screen by
## construction. Both are pure black, so raising it first costs no visible cut, and the
## blast is always the whole wall rather than a handful of blocks on their way down.
func _begin_blast() -> void:
	var origin := _blast_from
	if origin == Vector2.INF:
		origin = size * 0.5
	_show_grid()
	for cell in _cells:
		var centre: Vector2 = Vector2(cell.x, cell.y) + Vector2(cell.size, cell.size) * 0.5
		var away: Vector2 = centre - origin
		if away.length() < 1.0:
			away = Vector2.RIGHT.rotated(randf() * TAU)
		cell.delay = 0.0
		cell.vy = 0.0
		cell.vel = away.normalized() * BLAST_SPEED * randf_range(1.0 - BLAST_VARY, 1.0 + BLAST_VARY)
		cell.spin = randf_range(-BLAST_SPIN, BLAST_SPIN)


func _finish_reveal() -> void:
	if _phase == PHASE_IDLE:
		return
	_phase = PHASE_IDLE
	_clear_world()
	_grid_up = false
	_blast = false
	_blast_from = Vector2.INF
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
