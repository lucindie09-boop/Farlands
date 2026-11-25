extends SceneTree
## Headless check of what a dead goblin leaves behind.
##
## The table is data (`data/loot_tables.json`) and its own arithmetic is pinned by
## `tests/test_loot_table.cpp`; what that cannot show is the other half — that a real body,
## dying in the real world, hands the roll to the world's dropped-item node and that the three
## items come out as piles a player can see and pick up. So this kills goblins for real: it
## spawns each one on generated ground beside the player, takes its health to zero through the
## same `take_damage` a punch uses, and reads `DroppedItems.get_items()`.
##
## What is asserted is the shape of the table, not the luck: a goblin always leaves one to
## four pebbles, never more than two coins, never more than one ear, and never anything that
## is not one of the three. Over twenty kills every range is exercised, so "always" and
## "sometimes" are both seen, and the 33% ear is checked separately over three hundred rolls
## rather than left to twenty coin flips.
##
## Needs the real world and a generated surface, so it loads Main.tscn and waits for the
## player to land.
##
##   probes/probe_goblin_loot.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_loot.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
## Goblins killed in the world. Each one is a chance to see the zero-coin and the ear sides of
## the table as well as the ordinary case.
const KILLS := 20
## Rolled without a body, to read the ear's rate off enough samples to mean something.
const ROLLS := 300
const SILENCED_IDLE := 1000000
## A drop starts in the body it fell out of and falls from there.
const DROP_REACH := 3.0

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


## The top surface of a column, as the feet position standing on it.
func _surface(cm: Node, around: Vector3) -> Vector3:
	var x := floori(around.x)
	var z := floori(around.z)
	var y := floori(around.y) + 24
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(float(x) + 0.5, float(y + 1), float(z) + 0.5)
		y -= 1
	return Vector3.INF


## Empty the ground between kills through the node's own removal path: freeing the item nodes
## directly leaves their entries in `_items`, and the next `_step_items` writes a freed node.
func _clear(dropped: Node) -> void:
	var items: Array = dropped.get("_items")
	for i in range(items.size() - 1, -1, -1):
		dropped.call("_remove", i)


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	var cm := main.get_node_or_null("ChunkManager")
	var dropped := main.get_node_or_null("DroppedItems")
	if player == null or cm == null or dropped == null:
		print("PROBE FAIL: the world has no Player, ChunkManager or DroppedItems")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break

	var coin := BlockTextures.get_block_id_by_name("silver_coin")
	var pebble := BlockTextures.get_block_id_by_name("pebble")
	var ear := BlockTextures.get_block_id_by_name("goblin_ear")
	print("PROBE item ids silver_coin=%d pebble=%d goblin_ear=%d" % [coin, pebble, ear])
	if coin <= 0 or pebble <= 0 or ear <= 0:
		print("PROBE FAIL: an item name does not resolve, so it cannot be dropped")
		quit(1)
		return
	# The table has to be in the registry: `LootTables.load` runs with the world's configs,
	# and an empty answer here is that load never having happened.
	var first: Array = LootTables.roll("goblin")
	print("PROBE goblin table rolls %d entries before any kill" % first.size())

	var feet := _surface(cm, player.global_position + Vector3(2.0, 0.0, 0.0))
	if feet == Vector3.INF:
		print("PROBE FAIL: no ground beside the player")
		quit(1)
		return

	var with_coin := 0
	var with_ear := 0
	var pebbles_seen := 0
	for which in KILLS:
		var body := Node3D.new()
		body.name = "Goblin"
		body.set_script(load(BODY_SCRIPT))
		body.add_to_group("goblin")
		body.position = feet
		main.add_child(body)
		body.set("_idle_ticks", SILENCED_IDLE)
		for i in 4:
			await process_frame
		var where: Vector3 = body.global_position
		body.call("take_damage", 12.0)
		for i in 4:
			await process_frame

		var piles: Array = dropped.call("get_items")
		var coins := 0
		var pebbles := 0
		var ears := 0
		var names := []
		for pile in piles:
			var id := int(pile["block_id"])
			var count := int(pile["count"])
			var at: Vector3 = pile["position"]
			if id == coin:
				coins += count
			elif id == pebble:
				pebbles += count
			elif id == ear:
				ears += count
			else:
				_fail("kill %d left id %d, which is not one of the three" % [which, id])
			if at.distance_to(where) > DROP_REACH:
				_fail("kill %d left a pile %f blocks away, not where it died"
					% [which, at.distance_to(where)])
			names.append("%d x%d" % [id, count])
		# The body is gone by now (`die` frees it), so its drops are the only evidence left.
		print("PROBE kill %d drops: %s" % [which, ", ".join(names)])
		if pebbles < 1 or pebbles > 4:
			_fail("kill %d left %d pebbles, not 1 to 4" % [which, pebbles])
		if coins > 2:
			_fail("kill %d left %d coins, not 0 to 2" % [which, coins])
		if ears > 1:
			_fail("kill %d left %d ears, not 0 or 1" % [which, ears])
		if coins > 0:
			with_coin += 1
		if ears > 0:
			with_ear += 1
		pebbles_seen += pebbles
		_clear(dropped)
		for i in 2:
			await process_frame

	print("PROBE %d goblins: %d dropped a coin, %d left an ear, %d pebbles in all"
		% [KILLS, with_coin, with_ear, pebbles_seen])
	# One in three has no coin and one in three leaves no ear, so both sides of the table have
	# to show up over twenty kills without the probe demanding a particular one exactly.
	if with_coin < 8:
		_fail("only %d of %d goblins dropped a coin: the count range is not being rolled"
			% [with_coin, KILLS])
	if with_ear < 2:
		_fail("only %d of %d goblins left an ear" % [with_ear, KILLS])

	var ears := 0
	for i in ROLLS:
		for drop in LootTables.roll("goblin"):
			if int(drop["block_id"]) == ear:
				ears += 1
	var rate := float(ears) / float(ROLLS)
	print("PROBE %d rolls of the table: %d ears (%.2f), wanted about 0.33" % [ROLLS, ears, rate])
	if rate < 0.24 or rate > 0.42:
		_fail("the ear's chance came out at %.2f over %d rolls" % [rate, ROLLS])

	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
