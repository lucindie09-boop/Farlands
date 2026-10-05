extends SceneTree
## Headless check for stack merging: two drops of one kind lying next to each other
## become one pile.
##
## The merge is a step of the frame itself (dropped_items.gd, merge_pass), and this
## probe asks for that step directly -- it is public for exactly this -- after
## setting two items down where it wants them. What it holds to the script's own
## promises:
##
##   * the counts ADD, up to the inventory's own 64 cap; a pair whose counts do not
##     fit stays two piles, wherever it lies;
##   * the joined pile takes the fuller item's place and the younger of the two
##     clocks, and its body and mesh take the count's own size -- the same size and
##     the same shape row a count dropped at once gets, so a merge and a spawn are
##     one pile and not two;
##   * both motions combine only when BOTH were moving, each weighted by its own
##     count, and a still pile carries a moving stack's motion instead of halving
##     it;
##   * the drawn size pops past the merged size for POP_TIME and settles back onto
##     it, while the body is the merged size from the frame it merges;
##   * the frame pipeline itself merges two drops that land together, with no help
##     from the probe.
##
## Run: Godot --headless --path <project> --script res://probes/probe_item_merge.gd

const EPS := 0.002
const BASE_SCALE := 0.5
const POP_TIME := 0.2
const POP_GROWTH := 0.22
const MAX_STACK := 64

var ok := true
var _dropped: Node = null
var _stone: int = 0


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _same(label: String, got: Vector3, want: Vector3) -> void:
	if absf(got.x - want.x) > EPS or absf(got.y - want.y) > EPS or absf(got.z - want.z) > EPS:
		_fail("%s is %s, expected %s" % [label, got, want])


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	for i in range(5):
		await process_frame
	_dropped = main.get_node_or_null("DroppedItems")
	var cm: Node = main.get_node_or_null("ChunkManager")
	if _dropped == null or cm == null:
		_fail("main scene is missing DroppedItems or ChunkManager")
		quit(1)
		return
	_stone = BlockTextures.get_block_id_by_name("stone")
	if _stone <= 0:
		_fail("stone did not resolve by name")
		quit(1)
		return

	_check_join()
	_check_refusals()
	_check_motion()
	_check_pop()
	await _check_pipeline(main)

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)


## One item set down by hand: spawn puts a real drop in the world, and this only
## moves it where the test wants it and gives it the state to merge from. The merge
## pass reads the item dictionaries, never the solver's arrays, so nothing has to be
## stepped for this to be fair -- and nothing here is stepped.
func _drop(count: int, at: Vector3, velocity: Vector3, spin: Vector3, age: float) -> Dictionary:
	_dropped.spawn(_stone, count, at, Vector3(0.0, 0.0, 1.0))
	var item: Dictionary = _dropped._items[_dropped._items.size() - 1]
	item["position"] = at
	item["velocity"] = velocity
	item["spin"] = spin
	item["age"] = age
	item["asleep"] = false
	item["node"].global_position = at
	return item


func _clear() -> void:
	for i in range(_dropped._items.size() - 1, -1, -1):
		_dropped._remove(i)


func _count() -> int:
	return _dropped._items.size()


## The scale on the node's own mesh: the drawn size, measured off the instance the
## shader sees rather than off the item.
func _mesh_scale(item: Dictionary) -> Vector3:
	var node: Node3D = item["node"]
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance == null:
		_fail("an item has no mesh to measure")
		return Vector3.ZERO
	return mesh_instance.scale


## Two singles make a pair, three make one heap, and a merge is the same pile a
## count of that many dropped at once is.
func _check_join() -> void:
	_clear()
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 5.0)
	_drop(1, Vector3(0.7, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	# The older single is the one that stays, so the second is what joins it.
	var joined: Node3D = _dropped._items[1]["node"]
	_dropped.merge_pass()
	if _count() != 1:
		_fail("two singles did not merge (still %d items)" % _count())
		_clear()
		return
	var item: Dictionary = _dropped._items[0]
	print("probe: 1+1 -> count %d at %s size %s mesh %s age %.2f"
		% [item["count"], item["position"], item["size"], _mesh_scale(item).x, item["age"]])
	if item["count"] != 2:
		_fail("merged count is %d, expected 2" % item["count"])
	_same("merged position", item["position"], Vector3(0.0, 60.0, 0.0))
	_same("merged size", item["size"], Vector3.ONE * (BASE_SCALE * 1.25))
	if absf(item["age"]) > EPS:
		_fail("merged age is %.3f, expected the younger clock (0)" % item["age"])
	_same("merged mesh scale", _mesh_scale(item), Vector3.ONE * (BASE_SCALE * 1.25))
	# The joined item's node goes the same frame -- queue_free is the frame's own
	# end, so being queued is what the pass can promise and what is asked here.
	if not joined.is_queued_for_deletion():
		_fail("the joined item's node was not released")
	# A count dropped at once is the same pile: the same size, the same shape row.
	var fresh := _drop(2, Vector3(0.0, 60.0, 10.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_same("merged size against a fresh count's", item["size"], fresh["size"])
	_same("merged half against a fresh count's", item["half"], fresh["half"])
	_same("merged reach against a fresh count's", Vector3.ONE * item["radius"], Vector3.ONE * fresh["radius"])
	if item["shape"] != fresh["shape"]:
		_fail("a merged x2 uses shape row %d and a fresh x2 row %d -- one pile, two answers"
			% [item["shape"], fresh["shape"]])
	_clear()

	# Three in a heap become ONE pile in the pass: the survivor takes on each one it
	# has grown big enough to reach.
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(0.7, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(0.7, 60.0, 0.7), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 1:
		_fail("three singles did not become one pile (still %d items)" % _count())
	else:
		var heap: Dictionary = _dropped._items[0]
		print("probe: 1+1+1 -> count %d at %s" % [heap["count"], heap["position"]])
		if heap["count"] != 3:
			_fail("heap count is %d, expected 3" % heap["count"])
		_same("heap position", heap["position"], Vector3(0.0, 60.0, 0.0))
	_clear()


## The pairs that stay pairs: too far apart, too far up, past the cap, or over it.
func _check_refusals() -> void:
	_clear()
	# Apart: side by side with a metre of air between the boxes.
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(2.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 2:
		_fail("two items 2 apart merged; the merge gap is wider than a block")
	_clear()
	# A storey apart: stacked, but with more than the drop gap of air between them.
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(0.0, 60.8, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 2:
		_fail("two items a storey apart merged; a drop must land on a pile to join it")
	_clear()
	# ...and a block coming down onto a pile does join it: 0.7 up is inside the gap.
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(0.0, 60.7, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 1:
		_fail("a block landing on a pile did not merge (still %d items)" % _count())
	_clear()
	# The cap: 60 + 4 is a full stack exactly, and a full stack takes no more.
	_drop(60, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(4, Vector3(0.8, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 1:
		_fail("60+4 did not merge into a full stack (still %d items)" % _count())
	elif _dropped._items[0]["count"] != MAX_STACK:
		_fail("60+4 is %d, expected %d" % [_dropped._items[0]["count"], MAX_STACK])
	_drop(1, Vector3(1.2, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 2:
		_fail("a full stack took one more item")
	_clear()
	# Counts that do not fit at all: 40 + 32.
	_drop(40, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(32, Vector3(0.8, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	if _count() != 2:
		_fail("40+32 merged past the stack cap")
	_clear()


## What the motions do when two piles meet.
func _check_motion() -> void:
	_clear()
	# Both moving: each contributes its own count's share, so the pair carries on
	# between the two throws.
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3(2.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0), 0.0)
	_drop(3, Vector3(0.7, 60.0, 0.0), Vector3(0.0, 0.0, 8.0), Vector3(0.0, 0.0, 3.0), 0.0)
	_dropped.merge_pass()
	if _count() != 1:
		_fail("two moving stacks did not merge")
		_clear()
		return
	var item: Dictionary = _dropped._items[0]
	print("probe: 1@(2,0,0) + 3@(0,0,8) -> count %d vel %s spin %s"
		% [item["count"], item["velocity"], item["spin"]])
	if item["count"] != 4:
		_fail("the moving pair is count %d, expected 4" % item["count"])
	_same("combined velocity", item["velocity"], Vector3(0.5, 0.0, 6.0))
	_same("combined spin", item["spin"], Vector3(0.0, 0.25, 2.25))
	# The fuller item stays, and it was the count-3 one.
	_same("combined position", item["position"], Vector3(0.7, 60.0, 0.0))
	_clear()

	# One moving: the mover carries its motion into the still pile, which does not
	# halve it.
	_drop(4, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(0.7, 60.0, 0.0), Vector3(6.0, 0.0, 0.0), Vector3(0.0, 2.0, 0.0), 0.0)
	_dropped.merge_pass()
	if _count() != 1:
		_fail("a moving stack did not merge into a still pile")
		_clear()
		return
	item = _dropped._items[0]
	print("probe: 4@rest + 1@6 -> count %d vel %s spin %s"
		% [item["count"], item["velocity"], item["spin"]])
	if item["count"] != 5:
		_fail("the still+moving pile is count %d, expected 5" % item["count"])
	_same("carried velocity", item["velocity"], Vector3(6.0, 0.0, 0.0))
	_same("carried spin", item["spin"], Vector3(0.0, 2.0, 0.0))
	_clear()


## The pop: the body is the merged size at once, the mesh overshoots it half way
## through and lands back on it.
func _check_pop() -> void:
	_clear()
	_drop(1, Vector3(0.0, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(1, Vector3(0.7, 60.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	_dropped.merge_pass()
	var item: Dictionary = _dropped._items[0]
	var settled := BASE_SCALE * 1.25
	if item["pop"] != POP_TIME:
		_fail("a merge left pop at %.3f, expected %f" % [item["pop"], POP_TIME])
	_same("merged body", item["size"], Vector3.ONE * settled)
	_same("drawn size before the pop is drawn", _mesh_scale(item), Vector3.ONE * settled)
	# Half way through the pop the mesh is at its peak...
	_dropped._draw_item(item, POP_TIME * 0.5)
	print("probe: pop half way -> mesh %s (settled %s)" % [_mesh_scale(item).x, settled])
	_same("drawn size at the pop's peak", _mesh_scale(item), Vector3.ONE * (settled * (1.0 + POP_GROWTH)))
	_same("body during the pop", item["size"], Vector3.ONE * settled)
	# ...and the next draw lands it back on the body's size.
	_dropped._draw_item(item, POP_TIME)
	if item["pop"] != 0.0:
		_fail("the pop is over but %.3f seconds of it are left" % item["pop"])
	_same("drawn size after the pop", _mesh_scale(item), Vector3.ONE * settled)
	_same("body after the pop", item["size"], Vector3.ONE * settled)
	_clear()


## The same merge through the frame itself: two drops set down inside a hand's
## width of each other on ground the world has loaded are one pile a few frames
## later, with the probe never calling merge_pass.
func _check_pipeline(main: Node) -> void:
	_clear()
	var player := main.get_node_or_null("Player") as Node3D
	var at := Vector3(0.0, 60.0, 0.0)
	if player != null:
		at = player.global_position + Vector3(0.0, 0.4, 0.0)
	_drop(2, at, Vector3.ZERO, Vector3.ZERO, 0.0)
	_drop(2, at + Vector3(0.4, 0.0, 0.0), Vector3.ZERO, Vector3.ZERO, 0.0)
	for i in range(10):
		await process_frame
		if _count() == 1:
			break
	if _count() != 1:
		_fail("two drops landing together did not merge in the frame (%d items)" % _count())
		_clear()
		return
	var item: Dictionary = _dropped._items[0]
	print("probe: pipeline merged to count %d at %s size %s"
		% [item["count"], item["position"], item["size"]])
	if item["count"] != 4:
		_fail("the pipeline merged count is %d, expected 4" % item["count"])
	_same("pipeline merged size", item["size"], Vector3.ONE * (BASE_SCALE * 1.5))
	# ...and the drawn size settles onto the merged size once the pop is over.
	for i in range(30):
		await process_frame
		if item["pop"] <= 0.0:
			break
	if item["pop"] > 0.0:
		_fail("the merge pop never finished")
	else:
		_same("pipeline drawn size", _mesh_scale(item), item["size"])
	_clear()
