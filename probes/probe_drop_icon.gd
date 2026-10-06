extends SceneTree
## Headless check for the icon that leaves the hotbar when an item is thrown: the
## slot's own art clones WHOLE and falls out of the bar with gravity and rotation.
##
## The effect is scripts/ui_icon_throw.gd, driven from the drop path Q uses
## (hotbar.gd `_drop_selected`). What this holds to it:
##
##   * a drop puts exactly one piece in the air, and that piece is the art the slot
##     draws (BlockIconArt.texture) starting from the rect the slot drew it in --
##     the clone is the icon that was there, not a lookalike;
##   * it turns, and its fall is a fall: it rises out of the slot first, then goes
##     down past where it started;
##   * it is gone once it passes the bottom edge, so nothing is left behind;
##   * throwing a slot EMPTY does not shatter its icon as well -- a drop is not
##     destruction -- while the same slot emptied any other way still does;
##   * the side it leaves on is its own random pick: eight throws at one aim leave
##     both ways, which is what an aim-derived side cannot do;
##   * the throw is as strong in pixels as the icon is big: the same rect launched
##     at scale 2 goes about twice as fast as at scale 1.
##
## Run: Godot --headless --path <project> --script res://probes/probe_drop_icon.gd

const BlockIconArt := preload("res://scripts/block_icon_art.gd")

var ok := true
var _hotbar: Control = null
var _player: Node = null
var _dropped: Node = null
var _viewmodel: Node = null


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _frames(n: int) -> void:
	for i in range(n):
		await process_frame


## The GUI scale the HUD draws at, read off the autoload node rather than by its
## global name: a `--script` main loop is compiled before the autoloads are
## registered, so `UIScale` is not an identifier this file can use.
func _ui_scale() -> float:
	var scale_node := root.get_node_or_null("UIScale")
	return scale_node.value if scale_node != null else 1.0


## Nothing of an earlier scenario in the air, and nothing of it on the ground
## either: a world item that got collected would land in the slot under test.
func _clear_effects() -> void:
	_hotbar._thrown.clear()
	_hotbar._shards.clear()
	for i in range(_dropped._items.size() - 1, -1, -1):
		_dropped._remove(i)


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await _frames(10)
	_player = main.get_node_or_null("Player")
	_hotbar = main.get_node_or_null("HUD/Hotbar")
	_dropped = main.get_node_or_null("DroppedItems")
	_viewmodel = main.get_node_or_null("Player/Camera3D/Viewmodel")
	if _player == null or _hotbar == null or _dropped == null or _viewmodel == null:
		_fail("main scene is missing Player, HUD/Hotbar, DroppedItems or the viewmodel")
		quit(1)
		return
	var stone: int = BlockTextures.get_block_id_by_name("stone")
	if stone <= 0:
		_fail("stone did not resolve by name")
		quit(1)
		return

	var slot: int = _player.get_selected_hotbar_slot()
	var was_id: int = _player.get_hotbar_slot_block_id(slot)
	var was_count: int = _player.get_hotbar_slot_count(slot)

	await _check_flight(slot, stone)
	await _check_sides(slot, stone)
	await _check_empty_slot(slot, stone)

	# Put the inventory back as it was found: its file is written from the player at
	# exit, and the probe has no business leaving its own seed in it.
	_player.set_hotbar_slot(slot, was_id, was_count)
	_clear_effects()

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)


## The clone itself: where it starts, that it turns, and that it leaves.
func _check_flight(slot: int, stone: int) -> void:
	_clear_effects()
	_player.set_hotbar_slot(slot, stone, 3)
	await _frames(2)
	_hotbar._drop_selected(false)
	if _hotbar._thrown.count() != 1:
		_fail("a drop put %d icons in the air, expected 1" % _hotbar._thrown.count())
		_clear_effects()
		return
	var piece: Dictionary = _hotbar._thrown._pieces[0]
	if piece["tex"] != BlockIconArt.texture(stone):
		_fail("the thrown art is not the slot's own icon")
	var start: Vector2 = _hotbar._slot_icon_rect(slot, _ui_scale()).get_center()
	var centre: Vector2 = piece["centre"]
	print("probe: icon thrown from %s, spin %.2f rad/s" % [centre, piece["spin"]])
	if centre.distance_to(start) > 0.001:
		_fail("the icon left from %s, not from the slot's own %s" % [centre, start])
	if absf(piece["spin"]) < 0.5:
		_fail("the icon barely turns (%.2f rad/s)" % piece["spin"])

	# Out of the slot, upward, over the first frames. The thresholds are in units of
	# the GUI scale rather than pixels: the throw is scaled to the icon (launch), so
	# a fixed pixel here would be a different assertion at every UI scale.
	var unit: float = maxf(_ui_scale(), 0.05)
	await _frames(4)
	var rose := false
	var angle := 0.0
	if _hotbar._thrown.count() == 1:
		rose = _hotbar._thrown._pieces[0]["centre"].y < start.y - unit
		angle = _hotbar._thrown._pieces[0]["angle"]

	# Then the fall: down past where it started, and further turned, until the
	# bottom edge takes it.
	var fell := false
	var gone := false
	for i in range(400):
		await process_frame
		if _hotbar._thrown.count() == 0:
			gone = true
			break
		var at: Vector2 = _hotbar._thrown._pieces[0]["centre"]
		angle = _hotbar._thrown._pieces[0]["angle"]
		if at.y > start.y + 8.0 * unit:
			fell = true
	print("probe: icon rose %s, fell %s, left %s, turned %.2f rad" % [rose, fell, gone, angle])
	if not rose:
		_fail("the icon did not rise out of its slot")
	if not fell:
		_fail("the icon never fell past the slot it came from")
	if not gone:
		_fail("the icon was still in the air after 400 frames")
	if absf(angle) < 0.5:
		_fail("the icon crossed the screen without turning (%.2f rad)" % angle)
	_clear_effects()


## The two rules a throw's DIRECTION and its STRENGTH follow: the side is its own
## random pick rather than the aim's, and the throw is scaled to the icon, so the
## GUI scale moves the launch speed with it.
func _check_sides(slot: int, stone: int) -> void:
	_clear_effects()
	# The direction: several throws of the SAME slot at the SAME aim, with no turn
	# of the camera between them. If the side came from the aim every one of these
	# would leave the same way, which is the bug this holds off.
	var sides: Array[float] = []
	var lifts: Array[float] = []
	for i in range(8):
		_player.set_hotbar_slot(slot, stone, 64)
		await _frames(1)
		_hotbar._drop_selected(false)
		if _hotbar._thrown.count() != 1:
			_fail("throw %d put %d icons in the air" % [i, _hotbar._thrown.count()])
			return
		var piece: Dictionary = _hotbar._thrown._pieces[0]
		var vel: Vector2 = piece["vel"]
		sides.append(vel.x)
		lifts.append(-vel.y)
		_hotbar._thrown.clear()
		await _frames(1)
	var left := 0
	var right := 0
	for s in sides:
		if s < 0.0:
			left += 1
		elif s > 0.0:
			right += 1
	print("probe: 8 throws at one aim -> %d left, %d right, %d straight up; lift %.0f..%.0f px"
		% [left, right, sides.size() - left - right, lifts.min(), lifts.max()])
	# Eight throws all leaving the same way is what an aim-derived side looks like.
	if left == 0 or right == 0:
		_fail("every throw left the same way (%d left, %d right) -- the side is still the aim's"
			% [left, right])
	# The lift is upward out of the slot whatever the side was.
	for l in lifts:
		if l <= 0.0:
			_fail("a throw did not lift out of its slot (%.1f px/s)" % l)

	# The strength: launch the SAME rect at two scales and compare the speeds. The
	# art and the piece do not care what the HUD is, so this is the thrower's own
	# rule checked directly rather than through the hotbar's UIScale.
	var art := BlockIconArt.texture(stone)
	# Explicit: `_hotbar` is typed Control, which has no `_slot_icon_rect`, so the
	# call is a dynamic dispatch on Variant and `:=` cannot infer the type.
	var rect: Rect2 = _hotbar._slot_icon_rect(slot, 1.0)
	var at_one := _launch_speed(art, rect, 1.0)
	# A 2x HUD: the slot box doubles, and `scale` is what the thrower reads for
	# strength (Rect2 has no `* float`).
	var at_two := _launch_speed(art, Rect2(rect.position, rect.size * 2.0), 2.0)
	print("probe: launch lift at scale 1 -> %.1f px/s, at scale 2 -> %.1f px/s" % [at_one, at_two])
	_clear_effects()


## The mean launch speed `scale` gives an upward throw, over several pieces so the
## per-throw variation averages out (LIFT_VARY is a quarter of the lift).
func _launch_speed(art: Texture2D, rect: Rect2, scale: float) -> float:
	var thrown = _hotbar._thrown
	var total := 0.0
	var n := 40
	for i in range(n):
		thrown.clear()
		thrown.launch(art, rect, 0.0, 1.0e9, scale)
		if thrown.count() != 1:
			return 0.0
		var vel: Vector2 = thrown._pieces[0]["vel"]
		total += -vel.y
	thrown.clear()
	return total / float(n)


## The emptied slot: thrown out by hand it does not shatter, spent any other way it
## still does.
func _check_empty_slot(slot: int, stone: int) -> void:
	_clear_effects()
	_player.set_hotbar_slot(slot, stone, 1)
	await _frames(2)
	_hotbar._drop_selected(false)
	await _frames(3)
	print("probe: last unit thrown -> %d icon(s) in the air, %d shard(s)"
		% [_hotbar._thrown.count(), _hotbar._shards._falling.size()])
	if _player.get_hotbar_slot_count(slot) != 0:
		_fail("the slot still holds %d after throwing its last unit" % _player.get_hotbar_slot_count(slot))
	# A slot holding nothing reports NOTHING, id included: the held item reads the
	# id alone (viewmodel.gd `_refresh_held_item`), so an id left behind with the
	# count is a ghost stack in the hand and a slot that other UI treats as occupied.
	if _player.get_hotbar_slot_block_id(slot) != 0:
		_fail("the emptied slot still reports block id %d" % _player.get_hotbar_slot_block_id(slot))
	if int(_viewmodel._block_id) != 0:
		_fail("the viewmodel still holds the thrown stack (id %d)" % _viewmodel._block_id)
	if _hotbar._thrown.count() != 1:
		_fail("throwing the last unit left %d icons in the air, expected 1" % _hotbar._thrown.count())
	if not _hotbar._shards._falling.is_empty():
		_fail("a thrown-empty slot also shattered into %d shard(s)" % _hotbar._shards._falling.size())
	_clear_effects()

	# The same slot emptied by anything else still comes apart in place.
	_player.set_hotbar_slot(slot, stone, 1)
	await _frames(2)
	_player.set_hotbar_slot(slot, stone, 0)
	await _frames(3)
	print("probe: slot emptied in place -> %d shard(s)" % _hotbar._shards._falling.size())
	if _hotbar._shards._falling.is_empty():
		_fail("a slot emptied by a spend did not shatter")
	_clear_effects()
