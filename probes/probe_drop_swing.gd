extends SceneTree
## Headless check for what the hand does when an item is thrown out of it: the
## viewmodel punches.
##
## Q drops through hotbar.gd (_drop_selected), the real path the key uses: the
## slot is written back one smaller through the C++ inventory, the item is thrown
## into the world, and the viewmodel's FULL punch swing is kicked -- the same one
## a left click makes, not the weaker place stroke -- because nothing was put down,
## the item left the hand.
##
## Checks:
##   * one drop: the swing timer is at 1 and the place timer is still 0;
##   * the world gained one item of the dropped block and the slot went down by one;
##   * Ctrl+Q (the whole stack) punches again once the first swing has played out,
##     and throws what was left in the slot;
##   * the slot is put back as it was found, so the run leaves the inventory alone
##     (the inventory file is written from the player at exit).
##
## Run: Godot --headless --path <project> --script res://probes/probe_drop_swing.gd

const EPS := 0.001

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _frames(n: int) -> void:
	for i in range(n):
		await process_frame


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await _frames(10)
	var player: Node = main.get_node_or_null("Player")
	var hotbar: Node = main.get_node_or_null("HUD/Hotbar")
	var viewmodel: Node = main.get_node_or_null("Player/Camera3D/Viewmodel")
	var dropped: Node = main.get_node_or_null("DroppedItems")
	if player == null or hotbar == null or viewmodel == null or dropped == null:
		_fail("main scene is missing Player, HUD/Hotbar, Viewmodel or DroppedItems")
		quit(1)
		return
	var stone: int = BlockTextures.get_block_id_by_name("stone")
	if stone <= 0:
		_fail("stone did not resolve by name")
		quit(1)
		return

	var slot: int = player.get_selected_hotbar_slot()
	var was_id: int = player.get_hotbar_slot_block_id(slot)
	var was_count: int = player.get_hotbar_slot_count(slot)
	# Seed the selected slot, whatever was in it: the drop needs something to throw.
	player.set_hotbar_slot(slot, stone, 5)
	await _frames(2)

	# --- one unit out, with a punch -------------------------------------------
	var before: int = dropped._items.size()
	hotbar._drop_selected(false)
	var item: Dictionary = dropped._items[dropped._items.size() - 1]
	print("probe: one drop -> swing %.2f place %.2f item x%d, slot left %d"
		% [viewmodel._swing, viewmodel._swing_place, item["count"],
			player.get_hotbar_slot_count(slot)])
	if absf(viewmodel._swing - 1.0) > EPS:
		_fail("a drop left the punch swing at %.3f, expected 1" % viewmodel._swing)
	if viewmodel._swing_place > EPS:
		_fail("a drop played the place stroke (%.3f) instead of the punch" % viewmodel._swing_place)
	if dropped._items.size() != before + 1:
		_fail("a drop left %d items, expected one more than %d" % [dropped._items.size(), before])
	if item["count"] != 1 or item["block_id"] != stone:
		_fail("the thrown item is %d x%d, expected one stone" % [item["block_id"], item["count"]])
	if player.get_hotbar_slot_count(slot) != 4:
		_fail("the slot holds %d, expected 4 after one drop" % player.get_hotbar_slot_count(slot))

	# --- Ctrl+Q: the whole stack, another punch -------------------------------
	# The first swing has to play out first, or the second kick lands on the same
	# one -- the timer would read 1 however the drop behaved.
	dropped._remove(dropped._items.size() - 1)
	# Waited on the timer, not a frame count: a headless tree runs frames as fast as
	# it can, so a fixed count is a different number of seconds there than in game.
	for i in range(60):
		await process_frame
		if viewmodel._swing <= EPS:
			break
	if viewmodel._swing > EPS:
		_fail("the punch swing did not play out (%.3f left)" % viewmodel._swing)
	hotbar._drop_selected(true)
	var whole: Dictionary = dropped._items[dropped._items.size() - 1]
	print("probe: whole stack -> swing %.2f item x%d, slot left %d"
		% [viewmodel._swing, whole["count"], player.get_hotbar_slot_count(slot)])
	if absf(viewmodel._swing - 1.0) > EPS:
		_fail("the whole-stack drop did not punch (swing %.3f)" % viewmodel._swing)
	if whole["count"] != 4:
		_fail("the whole-stack drop threw x%d, expected x4" % whole["count"])
	if player.get_hotbar_slot_count(slot) != 0:
		_fail("the slot still holds %d after Ctrl+Q" % player.get_hotbar_slot_count(slot))

	# The slot goes back as it was found, and the thrown items go with the run.
	player.set_hotbar_slot(slot, was_id, was_count)
	for i in range(dropped._items.size() - 1, -1, -1):
		dropped._remove(i)

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)
