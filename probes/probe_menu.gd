extends SceneTree
## Checks the title screen and the block-curtain transition.
##
## The curtain is measured in real physics frames, because its whole claim is that
## the ENGINE decides the motion: a sheet of rigid bodies falls, collides, piles up
## and goes to sleep, and only then does the screen hold still and swap. So the
## probe asks the bodies what happened — how many spun, whether any are stacked on
## another, whether any interpenetrate, whether every one of them is asleep — and
## times the hold itself rather than a constant.
##
## The live half boots the real scene against the real world and asks the menu
## layer what it did to the player, the HUD and the settings menu.
##
## Run: bash probes/run_probe.sh probes/probe_menu.gd

# Loaded at run time, not preloaded: a preload compiles before the autoloads exist,
# and a script that names the UIScale singleton cannot compile that early.
var Curtain: GDScript

var ok := true


func _initialize() -> void:
	_run()


func _check(condition: bool, message: String) -> void:
	if condition:
		print("PROBE ok: ", message)
	else:
		ok = false
		print("PROBE FAIL: ", message)


func _run() -> void:
	Curtain = load("res://scripts/ui_block_curtain.gd")
	await _check_curtain()
	await _check_curtain_skip()
	await _check_reduce_motion()
	await _check_live_menu()
	print("PROBE menu ", "passed" if ok else "FAILED")
	quit(0 if ok else 1)


## The cover: real bodies, real collisions, a real settle, then hold and swap.
func _check_curtain() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	# One frame before sizing: `_ready` runs on the first idle frame, and it sets the
	# node's own size from the viewport. Sizing and playing before it runs is a race
	# the probe would lose silently, with the whole process loop switched off.
	await process_frame
	curtain.size = Vector2(1920, 1080)

	_check(Curtain.CELL_COLOR == Color(0, 0, 0, 1), "the wall is pure black")

	var hits := [0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.play_cover(false)
	var side: float = 1920.0 / float(Curtain.GRID_COLS)
	var rows: int = int(ceil(1080.0 / side)) + int(Curtain.EXTRA_ROWS)
	_check(curtain._bodies.size() == int(Curtain.GRID_COLS) * rows,
		"the sheet is %d x %d bodies (%d)" % [Curtain.GRID_COLS, rows, curtain._bodies.size()])
	_check(not curtain._bodies.is_empty() and curtain._bodies[0] is RigidBody2D,
		"the blocks are rigid bodies")

	# Tumble, sampled while they are still in the air.
	for i in range(10):
		await physics_frame
	var spun := 0
	for body in curtain._bodies:
		if absf(body.rotation) > 0.02:
			spun += 1
	_check(spun > curtain._bodies.size() / 3,
		"the blocks tumble as they fall (%d of %d)" % [spun, curtain._bodies.size()])

	# The pile settles when the engine says so, not when a timer says so.
	var started := Time.get_ticks_msec()
	while curtain._phase == Curtain.PHASE_COVER and Time.get_ticks_msec() - started < 10000:
		await physics_frame
	var settle_seconds := float(Time.get_ticks_msec() - started) / 1000.0
	_check(curtain._phase == Curtain.PHASE_HOLD, "the pile settles")
	_check(settle_seconds >= 0.3 and settle_seconds < 5.0,
		"the fall decided the timing: %.2f s to settle" % settle_seconds)
	_check(curtain._all_settled(), "every body has landed at the settle")

	# Collision, measured rather than assumed: at least one block is resting on
	# another, and no two are driven into each other. The overlap test is a real
	# separating-axis check, because the pile is free: two blocks side by side in the
	# same row are legitimately level, so "close in y" says nothing on its own.
	var stacked := 0
	var deepest := -INF
	for i in range(curtain._bodies.size()):
		var a: RigidBody2D = curtain._bodies[i]
		for j in range(i + 1, curtain._bodies.size()):
			var b: RigidBody2D = curtain._bodies[j]
			var overlap := _overlap_depth(a, b, side * float(Curtain.PHYS_SCALE))
			deepest = maxf(deepest, overlap)
			if absf(a.position.x - b.position.x) <= side * 0.5 and overlap <= 0.0:
				var dy := absf(a.position.y - b.position.y)
				if dy > side * 0.8 and dy < side * 1.2:
					stacked += 1
	_check(stacked > 0, "blocks rest on blocks (%d stacked pairs)" % stacked)
	# A hard landing is allowed to squash the solver a little - the blocks are black
	# on black, so a few pixels of overlap is invisible - but not to merge them: more
	# than a sixth of a block means the pile is being driven through itself.
	var collider := side * float(Curtain.PHYS_SCALE)
	_check(deepest <= collider * 0.15,
		"no two blocks are driven into each other (deepest %.1f px of %.0f)" % [deepest, collider])

	# The hold: the pile must stand still while the wall closes.
	var settled_at := Time.get_ticks_msec()
	var standing := []
	for body in curtain._bodies:
		standing.append(body.position)
	for i in range(12):
		await physics_frame
	_check(curtain._phase == Curtain.PHASE_HOLD, "still holding after the settle")
	var moved := 0.0
	for i in range(curtain._bodies.size()):
		moved = maxf(moved, (curtain._bodies[i].position - standing[i]).length())
	_check(moved < 0.5, "no block moves during the hold (worst %.2f px)" % moved)

	# The squares swell until the viewport is covered, so the size is measured rather
	# than assumed: it is whatever this pile needed.
	var seal_at := Time.get_ticks_msec()
	while not curtain._sealed and Time.get_ticks_msec() - seal_at < 1500:
		await physics_frame
	var sealed_in := float(Time.get_ticks_msec() - settled_at) / 1000.0
	_check(curtain._sealed and sealed_in <= 0.45,
		"the wall closes while it holds (%.2f s in, squares at %.2f)" % [sealed_in, curtain._draw_scale])

	# Filling the screen, measured after closing: sample the viewport and count what the
	# drawn squares cover. They are smaller while they fall - that is what makes the
	# sheet read as blocks - and are judged here in the state the player holds.
	var drawn: float = side * curtain._draw_scale * 0.5
	var filled := 0
	var samples := 0
	for gy in range(27):
		for gx in range(48):
			var point := Vector2((float(gx) + 0.5) * 40.0, (float(gy) + 0.5) * 40.0)
			samples += 1
			for body in curtain._bodies:
				var local: Vector2 = body.transform.affine_inverse() * point
				if absf(local.x) <= drawn and absf(local.y) <= drawn:
					filled += 1
					break
	var fill_ratio := float(filled) / float(samples)
	_check(fill_ratio >= 0.999, "the wall fills the screen (%.2f%% covered)" % (fill_ratio * 100.0))

	while hits[0] == 0 and Time.get_ticks_msec() - settled_at < 4000:
		await physics_frame
	var hold_seconds := float(Time.get_ticks_msec() - settled_at) / 1000.0
	_check(hits[0] == 1, "the cover emits covered once (%d)" % hits[0])
	_check(hold_seconds >= 0.5, "the wall stood still for %.2f s before the swap" % hold_seconds)
	_check(curtain._grid_up, "the pile is replaced by a lattice before the drop")
	_check(curtain._bodies.is_empty(), "the lattice lets the pile's bodies go")

	# Reveal: the lattice drops out of frame.
	var revealed := [0]
	curtain.revealed.connect(func(): revealed[0] += 1)
	curtain.play_reveal()
	var reveal_at := Time.get_ticks_msec()
	while revealed[0] == 0 and Time.get_ticks_msec() - reveal_at < 4000:
		await physics_frame
	_check(revealed[0] == 1, "reveal emits revealed once (%d)" % revealed[0])
	_check(float(Time.get_ticks_msec() - reveal_at) / 1000.0 <= 2.0,
		"the pile falls out of frame (%.2f s)" % (float(Time.get_ticks_msec() - reveal_at) / 1000.0))
	_check(not curtain.visible and curtain._bodies.is_empty(),
		"the curtain hides itself and frees the bodies")
	curtain.queue_free()


## Separating-axis overlap of two rotated squares: how far one is inside the
## other, in pixels. Negative means they are apart.
func _overlap_depth(a: RigidBody2D, b: RigidBody2D, side: float) -> float:
	var right_a := Vector2.RIGHT.rotated(a.rotation)
	var down_a := Vector2.DOWN.rotated(a.rotation)
	var right_b := Vector2.RIGHT.rotated(b.rotation)
	var down_b := Vector2.DOWN.rotated(b.rotation)
	var axes := [right_a, down_a, right_b, down_b]
	var delta := b.position - a.position
	var depth := INF
	for axis in axes:
		var ra: float = (absf(axis.dot(right_a)) + absf(axis.dot(down_a))) * side * 0.5
		var rb: float = (absf(axis.dot(right_b)) + absf(axis.dot(down_b))) * side * 0.5
		depth = minf(depth, ra + rb - absf(delta.dot(axis)))
	return depth


## Skip presents the opaque frame at once and is idempotent.
func _check_curtain_skip() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	await process_frame
	curtain.size = Vector2(1280, 720)
	var hits := [0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.play_cover(false)
	curtain.skip()
	_check(hits[0] == 1, "skip emits covered at once (%d)" % hits[0])
	_check(curtain._grid_up, "skip presents the lattice at once")
	_check(curtain._grid_up and curtain._bodies.is_empty(),
		"skip puts the lattice up and lets the bodies go")
	curtain.skip()
	_check(hits[0] == 1, "a second skip does not re-emit covered (%d)" % hits[0])
	curtain.queue_free()


## Reduce motion draws no bodies at all and fades both ways, holding the same beat.
func _check_reduce_motion() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	await process_frame
	curtain.size = Vector2(1920, 1080)
	var hits := [0, 0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.revealed.connect(func(): hits[1] += 1)
	curtain.play_cover(true)
	var started := Time.get_ticks_msec()
	while hits[0] == 0 and Time.get_ticks_msec() - started < 3000:
		await physics_frame
	_check(hits[0] == 1 and curtain._bodies.is_empty(),
		"reduce motion covers by fading, with no bodies")
	_check(float(Time.get_ticks_msec() - started) / 1000.0 >= 0.5,
		"reduce motion holds the same beat")
	curtain.play_reveal()
	started = Time.get_ticks_msec()
	while hits[1] == 0 and Time.get_ticks_msec() - started < 3000:
		await physics_frame
	_check(hits[1] == 1, "reduce motion reveals by fading back")
	curtain.queue_free()


## The live half: boot the real scene, then drive the menu the way a player does.
func _check_live_menu() -> void:
	var main: Node = (load("res://Main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	current_scene = main
	await process_frame
	await process_frame

	var menu: Node = root.get_node_or_null("MenuLayer")
	_check(menu != null, "the menu layer autoload exists")
	if menu == null:
		return
	await process_frame
	await process_frame

	var player: Node = main.get_node("Player")
	var hotbar: Node = main.get_node("HUD/Hotbar")
	var title: Control = menu._title
	_check(title != null and title.visible, "the title screen is up at boot")
	_check(player.is_settings_open(), "the title freezes the player")
	_check(not hotbar.visible, "the title hides the hotbar")
	var viewport: Vector2 = root.get_visible_rect().size
	_check(title.size == viewport,
		"the title fills the viewport (%.0f x %.0f of %.0f x %.0f)" % [
			title.size.x, title.size.y, viewport.x, viewport.y])

	var primary := String(title._buttons[0].text)
	var has_world: bool = title.has_world()
	_check(primary == ("CONTINUE" if has_world else "PLAY"),
		"the primary action names the save state: %s (world=%s)" % [primary, has_world])
	var live_seed = menu._current_seed()
	_check(String(title._footer.text).contains("seed %d" % int(live_seed)) if live_seed != null
		else String(title._footer.text).contains("new world"),
		"the footer names the world: %s" % title._footer.text)
	_check(title._buttons[0].has_focus(), "the primary action holds focus on open")

	# Quit is two presses: the first arms the row, and it disarms on its own.
	title._on_quit()
	_check(String(title._buttons[2].text).begins_with("QUIT?"),
		"the first Quit press only arms it: %s" % title._buttons[2].text)
	title._process(3.0)
	_check(String(title._buttons[2].text) == "QUIT", "an armed Quit disarms itself")

	# Settings hands the screen over and comes back to the title.
	menu._on_settings()
	await process_frame
	await process_frame
	var settings: Node = menu._settings_menu
	_check(settings != null and settings.is_open, "Settings opens the existing menu")
	_check(not title.visible, "the title steps back behind Settings")
	if settings != null:
		settings._close()
	await process_frame
	await process_frame
	_check(title.visible, "closing Settings returns to the title")
	_check(player.is_settings_open(), "the player is still frozen after Settings")

	# Reduce Motion is the transition's accessibility path: the settings menu owns
	# the switch, this layer reads the file. Round-trip it through a real save.
	settings._reduce_motion = true
	settings._save_settings()
	_check(menu._reduce_motion(), "Reduce Motion round-trips through settings.cfg")
	settings._reduce_motion = false
	settings._save_settings()
	_check(not menu._reduce_motion(), "Reduce Motion turns back off")

	# Start: cover, release, reveal.
	menu._on_start()
	var started := Time.get_ticks_msec()
	while menu._busy and Time.get_ticks_msec() - started < 12000:
		await process_frame
	_check(not menu._busy, "the transition finished")
	_check(not player.is_settings_open(), "Play hands the player control back")
	_check(hotbar.visible, "Play restores the hotbar")
	_check(not title.visible, "the title is gone after Play")
	_check(not menu._curtain.visible, "the curtain is gone after the reveal")
	_check_quit_wired()
	# Free the menu's own nodes before quitting as well: they live under the
	# autoload, and a quit from a coroutine leaves a CanvasLayer's children to be
	# reported as leaked at exit, which looks like a defect in the probe.
	menu.queue_free()
	main.queue_free()
	await process_frame
	await process_frame


## Quit is the one action not exercised above: assert the wiring exists instead of
## clicking it, which would end the probe before it reported.
func _check_quit_wired() -> void:
	var menu: Node = root.get_node_or_null("MenuLayer")
	_check(menu._title.quit_pressed.get_connections().size() > 0, "Quit is wired to the layer")
