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
##     destruction -- while the same slot emptied any other way still does.
##
## Run: Godot --headless --path <project> --script res://probes/probe_drop_icon.gd

const BlockIconArt := preload("res://scripts/block_icon_art.gd")

var ok := true
var _hotbar: Control = null
var _player: Node = null
var _dropped: Node = null


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
	if _player == null or _hotbar == null or _dropped == null:
		_fail("main scene is missing Player, HUD/Hotbar or DroppedItems")
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

	# Out of the slot, upward, over the first frames.
	await _frames(4)
	var rose := false
	var angle := 0.0
	if _hotbar._thrown.count() == 1:
		rose = _hotbar._thrown._pieces[0]["centre"].y < start.y - 1.0
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
		if at.y > start.y + 8.0:
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
