extends SceneTree
## Headless integration probe for the wand: menu, ghost and the paste gesture.
##
## Loads the real scene, gives itself the wand, and walks the exact flow a player
## walks â€” middle click for the menu, choose a function, choose a build file, aim
## the ghost, place it â€” asserting at each step that the WORLD agrees rather than
## that a function returned something:
##
##   - the menu is modal (the player is told a UI is open) and lists the files
##   - the ghost comes from the C++ preview: cell count, volume, and the sampled
##     flag for a build too big to draw whole
##   - a build placed through the wand lands exactly the cells the ghost showed
##     (counted in the world, not taken from the return value)
##   - a build with LIQUID in it places the still kind, so what the ghost promised
##     is what appears
##   - the wand's clicks do not mine or place: PlayerController reports the wand
##     held, and break progress stays at zero while it is
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_wand.gd

const CHURCH := "church.schematic"
const BIG := "10179.schematic"

var main: Node
var cm: Node
var player: Node
var wand: Node
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _surface_point(from: Vector3) -> Vector3:
	var x := int(floor(from.x))
	var z := int(floor(from.z))
	var y := int(from.y) + 64
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(x, y + 1, z)
		y -= 1
	return Vector3.INF

func _box_is_air(origin: Vector3i, w: int, h: int, l: int) -> bool:
	for y in range(origin.y, origin.y + h):
		for z in range(origin.z, origin.z + l):
			for x in range(origin.x, origin.x + w):
				if cm.get_block(x, y, z) != 0:
					return false
	return true

func _non_air(min_p: Vector3i, max_p: Vector3i) -> int:
	var count := 0
	for y in range(min_p.y, max_p.y + 1):
		for z in range(min_p.z, max_p.z + 1):
			for x in range(min_p.x, max_p.x + 1):
				if cm.get_block(x, y, z) != 0:
					count += 1
	return count

## Waits for the wand's chosen build to land on the worker thread, reporting the
## longest frame the MAIN thread saw while it was being decoded. That number is
## the whole point of the build store: a 380 ms inflate must not be 380 ms of
## frozen frames.
func _wait_for_build(wand_node: Node) -> float:
	var frames := 0
	var worst := 0.0
	while frames < 3000 and not wand_node._build_ready():
		var t0 := Time.get_ticks_usec()
		await process_frame
		worst = maxf(worst, float(Time.get_ticks_usec() - t0) / 1000.0)
		frames += 1
	print("probe: %s was read in %d frames; the longest frame was %.1f ms"
		% [wand_node.get_file(), frames, worst])
	return worst

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)

	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	wand = main.get_node_or_null("HUD/Wand")
	if cm == null or player == null or wand == null:
		_fail("ChunkManager / Player / HUD/Wand missing (is the Wand node in main.tscn?)")
		quit(1)
		return

	# The wand item has to exist as an item for any of this to mean anything.
	var wand_id := BlockTextures.get_block_id_by_name("wand")
	if wand_id <= 0 or not BlockTextures.is_item(wand_id):
		_fail("the wand item did not resolve (data/items.json)")
		quit(1)
		return

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited < 30:
			continue
		if player.is_on_floor() and _surface_point(player.global_position) != Vector3.INF:
			break
	print("probe: player landed after %d frames at %s" % [waited, player.global_position])

	# Hold the wand in slot 0, which is the slot that starts selected.
	# The saved inventory can land a frame or two after the scene builds, so the
	# slot is set until it sticks rather than once (a probe that gives itself the
	# item before the load finishes tests nothing and says so confusingly).
	# The selected slot is whatever the player last had, so put the wand in slot 0
	# AND select it: filling a slot the player is not looking at leaves the hand
	# empty and every "is the wand held" answer false.
	for _attempt in 30:
		player.set_hotbar_slot(0, wand_id, 1)
		player.select_hotbar_slot(0)
		await process_frame
		if int(player.get_selected_block()) == wand_id:
			break
	print("probe: selected hotbar slot %d holds %d" % [player.get_selected_hotbar_slot(), int(player.get_selected_block())])
	if int(player.get_selected_block()) != wand_id:
		_fail("the wand is not the selected item (%d vs %d)"
			% [int(player.get_selected_block()), wand_id])
	if not player.is_wand_held():
		_fail("PlayerController does not report the wand as held")
	else:
		print("probe: wand id=%d is held and reported as a wand" % wand_id)

	# --- the menu, from the middle click ------------------------------------
	if wand.is_menu_open():
		_fail("the wand menu started open")
	wand._on_menu_pressed()
	if not wand.is_menu_open():
		_fail("the middle click did not open the wand menu")
	elif not player.is_wand_menu_open():
		_fail("the open menu did not tell the player a UI is open (it would walk)")
	else:
		print("probe: middle click opened the menu and made it modal")

	# The function page offers every function, and the file page lists the folder.
	var functions: Array = wand.FUNCTIONS
	if functions.is_empty():
		_fail("the wand has no functions")
	for entry in functions:
		if not wand.set_function(String(entry["id"])):
			_fail("function %s is listed but cannot be selected" % entry["id"])
	if wand.get_function() != "paste":
		_fail("the selected function is %s, expected paste" % wand.get_function())
	var files: Array = (preload("res://scripts/schematic_files.gd") as GDScript).list()
	if files.is_empty():
		_fail("no build files found, so the menu has nothing to choose")
	elif not CHURCH in files:
		_fail("%s is missing from the build files the menu lists" % CHURCH)
	else:
		print("probe: the menu lists %d build files" % files.size())

	if not wand.choose_file(CHURCH):
		_fail("the wand could not read %s" % CHURCH)
	if wand.get_file() != CHURCH:
		_fail("the wand's chosen file is %s, expected %s" % [wand.get_file(), CHURCH])
	wand.close_menu()
	if wand.is_menu_open() or player.is_wand_menu_open():
		_fail("closing the menu did not hand the game back")
	# The file is decoded and planned on a worker thread, so the ghost waits for it
	# rather than blocking the frame that asked for it.
	var worst_frame := await _wait_for_build(wand)
	if worst_frame >= 100.0:
		_fail("the main thread stalled %.1f ms waiting for a build" % worst_frame)
	if not wand._build_ready():
		_fail("%s never finished being read" % CHURCH)

	# --- the ghost: right click in the world --------------------------------
	var base := _surface_point(player.global_position)
	var origin := Vector3i.ZERO
	var found := false
	for offset in [Vector2i(0, 0), Vector2i(-18, 0), Vector2i(18, 0), Vector2i(0, -18),
			Vector2i(0, 18), Vector2i(-18, -18), Vector2i(18, 18)]:
		var spot := _surface_point(Vector3(base.x + offset.x, base.y, base.z + offset.y))
		if spot == Vector3.INF:
			continue
		for lift in [2, 6, 12, 20, 30]:
			var candidate := Vector3i(int(spot.x) - 14, int(spot.y) + lift, int(spot.z) - 16)
			if _box_is_air(candidate, 28, 36, 32):
				origin = candidate
				found = true
				break
		if found:
			break
	if not found:
		_fail("no clear air to aim the ghost at")
		quit(1)
		return

	var info: Dictionary = wand.anchor_preview_at(origin)
	if not info.get("ok", false):
		_fail("aiming the ghost failed: %s" % info.get("error", "?"))
		quit(1)
		return
	var planned := int(info.get("planned", 0))
	var shown := int(info.get("cells_returned", 0))
	if not wand.has_preview():
		_fail("the wand says it has no preview after aiming one")
	if planned <= 0 or shown != planned:
		_fail("the ghost plans %d cells but carries %d (nothing should be sampled at this size)"
			% [planned, shown])
	if bool(info.get("cells_sampled", false)):
		_fail("a small build was sampled, so the ghost would be a dotted lie")
	var lo: Vector3i = info.get("min", origin)
	var hi: Vector3i = info.get("max", origin)
	if lo.x < origin.x or hi.x < lo.x or hi.y < lo.y or hi.z < lo.z:
		_fail("the ghost's volume %s..%s does not contain the origin %s" % [lo, hi, origin])
	print("probe: ghost planned %d cells, volume %s..%s â€” %s"
		% [planned, lo, hi, wand.preview_summary()])

	# --- left click: what the ghost showed is what lands ---------------------
	var placed: Dictionary = wand.confirm_paste()
	if not placed.get("ok", false):
		_fail("the wand's paste failed: %s" % placed.get("error", "?"))
	elif int(placed.get("cells", 0)) != planned:
		_fail("the wand placed %d of the %d cells the ghost showed"
			% [int(placed.get("cells", 0)), planned])
	elif wand.has_preview():
		_fail("the ghost survived the paste")
	else:
		var in_box := _non_air(lo, hi)
		if in_box != planned:
			_fail("the box holds %d non-air blocks after placing %d cells"
				% [in_box, planned])
		else:
			print("probe: left click placed %d cells, and the box holds exactly that" % in_box)
	cm.undo_paste()

	# --- a build too big to draw whole is sampled, and says so --------------
	if wand.choose_file(BIG):
		# The big build is the one that must not stall a frame: 382 ms of inflate
		# plus a 60 ms plan, all of it off the main thread now.
		var big_worst := await _wait_for_build(wand)
		if big_worst >= 100.0:
			_fail("the main thread stalled %.1f ms while %s was decoded" % [big_worst, BIG])
		# The anchor is the build's CONTENT corner, not its box corner: this file
		# holds nothing until x 90 and z 114 of its own box, which is exactly why a
		# paste used to land a hundred blocks from where the crosshair was.
		var big_bytes := FileAccess.get_file_as_bytes(
			(preload("res://scripts/schematic_files.gd") as GDScript).resolve(BIG))
		var big_info: Dictionary = cm.inspect_schematic(big_bytes, {})
		var margin: Vector3i = big_info.get("margin", Vector3i.ZERO)
		if margin != Vector3i(90, 0, 114):
			_fail("%s reports its content margin as %s, expected (90, 0, 114)" % [BIG, margin])
		var big: Dictionary = wand.anchor_preview_at(origin)
		if not big.get("ok", false):
			_fail("aiming the big build failed: %s" % big.get("error", "?"))
		else:
			var big_planned := int(big.get("planned", 0))
			var big_shown := int(big.get("cells_returned", 0))
			if big_shown >= big_planned:
				_fail("the big build's ghost carried all %d cells; the cap did not hold"
					% big_shown)
			elif Vector3i(big.get("min", origin)) != origin:
				_fail("the big build's ghost starts at %s, not at the anchor %s"
					% [big.get("min"), origin])
			elif not bool(big.get("cells_sampled", false)):
				_fail("the big build's ghost was capped but not reported as sampled")
			elif big_shown > 20000:
				_fail("the big build's ghost carried %d cells, above the 20,000 cap" % big_shown)
			else:
				print("probe: big build ghost sampled %d of %d cells" % [big_shown, big_planned])
		wand.cancel_preview()
		if wand.has_preview():
			_fail("cancelling left the ghost up")
	else:
		print("probe: note: %s is not in the schematics folder, sampling was not checked" % BIG)

	# --- the wand never mines ------------------------------------------------
	# update_break_progress runs every frame; with the wand held nothing may
	# accumulate, or a click meant for the build would chew the terrain behind it.
	var progress := float(player.get_break_state().get("progress", 0.0))
	if progress != 0.0:
		_fail("break progress moved while the wand was held (%.3f)" % progress)
	else:
		print("probe: no break progress accumulates while the wand is held")

	if ok:
		print("PROBE PASS")
	quit(0 if ok else 1)
