extends SceneTree
## Can the wooden tools actually be crafted, out of every plank type?
##
## The wooden recipes are the first ones whose key entry is a LIST of acceptable
## ingredients rather than one name, so this drives the real RecipeBook through
## both ClassDB bindings — match_recipe (the output preview, gated on
## availability) and craft_recipe (the consumption) — once per plank type.
##
## The negative controls are the point of the design: a tool is always ONE wood
## type throughout, so a layout mixing oak and pine planks must match NOTHING
## (expansion is per symbol, not per cell); a layout short one plank must not
## preview; and the 3-cell-a-side patterns must not fit the inventory's 2x2 grid.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_craft.gd

const PLANKS := ["oak_planks", "pine_planks", "spruce_planks", "holly_planks"]

var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _id(name: String) -> int:
	return BlockTextures.get_block_id_by_name(name)

# --- grid builders ---------------------------------------------------------
# A 3x3 pickaxe: three planks across the top, two sticks down the middle.
# `plank_counts` lets a caller leave a plank slot empty on purpose.
func _pickaxe_grid(planks: Array, plank_counts := [1, 1, 1]) -> Array:
	var cells := PackedInt32Array([0, 0, 0, 0, 0, 0, 0, 0, 0])
	var counts := PackedInt32Array([0, 0, 0, 0, 0, 0, 0, 0, 0])
	for i in [0, 1, 2]:
		cells[i] = planks[i]
		counts[i] = plank_counts[i]
	for i in [4, 7]:
		cells[i] = _id("stick")
		counts[i] = 1
	return [cells, counts]

# A 3x3 hammer: the stone hammer's shape with the head material swapped.
func _hammer_grid(planks: Array) -> Array:
	var cells := PackedInt32Array([0, 0, 0, 0, 0, 0, 0, 0, 0])
	var counts := PackedInt32Array([0, 0, 0, 0, 0, 0, 0, 0, 0])
	cells[1] = planks[0]; counts[1] = 1
	cells[4] = _id("stick"); counts[4] = 1
	cells[5] = planks[1]; counts[5] = 1
	cells[6] = _id("stick"); counts[6] = 1
	return [cells, counts]

# --- checks ----------------------------------------------------------------
# Lays a shape out, requires the match to name `want`, then crafts it for real
# and requires the grid to come out empty (both units of every ingredient).
func _check(player: Node3D, label: String, grid: Array, want: int) -> void:
	var cells: PackedInt32Array = grid[0]
	var counts: PackedInt32Array = grid[1]

	var match: Dictionary = player.match_recipe(cells, counts)
	if not match.get("ok", false):
		_fail("%s: the layout matched no recipe at all" % label)
		return
	if int(match.get("block_id", 0)) != want or int(match.get("count", 0)) != 1:
		_fail("%s: matched %d x block %d, expected 1 x %d"
			% [label, int(match.get("count", 0)), int(match.get("block_id", 0)), want])
		return

	var crafted: Dictionary = player.craft_recipe(cells, counts)
	if not crafted.get("ok", false):
		_fail("%s: craft_recipe refused a layout match_recipe accepted" % label)
		return
	if int(crafted.get("block_id", 0)) != want:
		_fail("%s: crafting produced block %d, expected %d"
			% [label, int(crafted.get("block_id", 0)), want])
		return
	var left: PackedInt32Array = crafted.get("new_counts", PackedInt32Array())
	var left_total := 0
	for c in left:
		left_total += c
	if left_total != 0:
		_fail("%s: crafting left %d ingredients behind in the grid" % [label, left_total])
		return
	print("probe: %-28s -> 1 x %-5d (all ingredients consumed)" % [label, want])

# Requires the layout to match NOTHING.
func _check_rejected(player: Node3D, label: String, grid: Array) -> void:
	var match: Dictionary = player.match_recipe(grid[0], grid[1])
	if match.get("ok", false):
		_fail("%s: matched block %d, expected no match"
			% [label, int(match.get("block_id", 0))])
		return
	print("probe: %-28s -> no match, as expected" % label)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame

	var player: Node3D = main.get_node_or_null("Player")
	if player == null:
		_fail("Player (the recipe bindings' host) missing")
		quit(1)
		return

	var planks: Array[int] = []
	for name in PLANKS:
		var pid := _id(name)
		if pid <= 0:
			_fail("%s did not resolve" % name)
			quit(1)
			return
		planks.append(pid)

	var pickaxe := _id("wooden_pickaxe")
	var hammer := _id("wooden_hammer")
	var log := _id("oak_log")
	if pickaxe <= 0 or hammer <= 0:
		_fail("wooden_pickaxe / wooden_hammer did not resolve (items.json entry missing?)")
		quit(1)
		return
	if log <= 0:
		_fail("oak_log did not resolve")
		quit(1)
		return

	# Every plank type builds both wooden tools, all of the same wood.
	for i in planks.size():
		var three: Array = [planks[i], planks[i], planks[i]]
		_check(player, "wooden_pickaxe from " + PLANKS[i], _pickaxe_grid(three), pickaxe)
		_check(player, "wooden_hammer from " + PLANKS[i],
			_hammer_grid([planks[i], planks[i]]), hammer)

	# One wood type throughout: a mixed head is not a recipe.
	_check_rejected(player, "mixed oak/pine pickaxe",
		_pickaxe_grid([planks[0], planks[1], planks[0]]))
	_check_rejected(player, "mixed oak/pine hammer",
		_hammer_grid([planks[0], planks[1]]))

	# Logs are not planks.
	_check_rejected(player, "pickaxe made of oak_log", _pickaxe_grid([log, log, log]))

	# The availability gate still sees the expanded ingredients: drop one plank
	# from the grid and the preview must disappear (a stale preview would let the
	# player craft something they cannot afford).
	_check_rejected(player, "pickaxe one plank short",
		_pickaxe_grid([planks[0], planks[0], planks[0]], [1, 1, 0]))

	# 3 cells a side does not fit the inventory's 2x2 grid (4 cells).
	var small_ids := PackedInt32Array([planks[0], planks[0], _id("stick"), _id("stick")])
	var small_counts := PackedInt32Array([1, 1, 1, 1])
	var small: Dictionary = player.match_recipe(small_ids, small_counts)
	if small.get("ok", false):
		_fail("a 2x2 grid matched the wooden pickaxe, which is a crafting-table recipe")
	else:
		print("probe: %-28s -> no match, as expected" % "wooden_pickaxe in the 2x2 grid")

	# The stone hammer (a plain single-name key) must still work after the loader
	# changed: expansion may not have broken the recipes that list one ingredient.
	_check(player, "stone_hammer (single-name key)",
		_hammer_grid([_id("cobblestone"), _id("cobblestone")]), _id("stone_hammer"))

	print("PROBE PASS" if ok else "PROBE FAIL")
	quit(0 if ok else 1)
