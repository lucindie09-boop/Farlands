extends SceneTree
## Headless check of the goblin's blow: the reach is a distance in THREE dimensions, and it needs
## a clear line to the face it lands on.
##
## Both halves were wrong in play. The reach was the FLAT distance between the two, so a player
## standing on top of a cliff twenty-five blocks overhead was still "in reach" and took hits
## through the rock; and nothing asked whether anything solid stood between them, so a player
## behind a wall was hit too. A distance with an axis dropped is not a distance, and a blow that
## lands on stone is not a blow.
##
## The world is the spawn probe's stub plus one block ray whose answer this probe sets: what is
## under test is the entity's own arithmetic -- which distance it measures, and what it asks
## before it swings.
##
##   probes/probe_goblin_fight.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_fight.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
const ENEMY_SCRIPT := "res://probes/goblin_probe_enemy.gd"
const FAKE_MANAGER := preload("res://probes/goblin_probe_world.gd")
## Feet on the same ground, one block apart: inside the reach, which is about 1.45.
const GOBLIN_AT := Vector3(1.5, 64.0, 1.5)
const NEAR := Vector3(2.5, 64.0, 1.5)
## Straight up. Twenty-five was the number seen in play; three is the same bug with the ground
## still in sight, so both are checked.
const ABOVE := 3.0
const WAY_ABOVE := 25.0
## One up and one along: 1.41 of three-dimensional distance, which is inside the reach -- a player
## standing on the block in front of a mob is hit by it in the game.
const UP_ON_A_STEP := Vector3(2.5, 65.0, 1.5)

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _world() -> Node:
	var world := Node3D.new()
	world.name = "Main"
	root.add_child(world)
	current_scene = world
	var player := Node3D.new()
	player.name = "Player"
	world.add_child(player)
	var cm := Node.new()
	cm.name = "ChunkManager"
	cm.set_script(FAKE_MANAGER)
	world.add_child(cm)
	return world


## One blow's worth of ticks with the enemy where the probe put it. The goblin's own physics is
## not stepping while this runs -- no frame is awaited between the calls -- so every blow counted
## here is one this probe asked for.
func _blow(goblin: Node3D, enemy: Node3D, at: Vector3) -> int:
	enemy.position = at
	goblin.set("_enemy", enemy)
	goblin.set("_attack_ticks", 0)
	goblin.call("_update_enemy", 20)
	return int(enemy.get("hits"))


func _reaches(goblin: Node3D, enemy: Node3D, at: Vector3) -> bool:
	_blow(goblin, enemy, at)
	return bool(goblin.get("_in_reach"))


func _run() -> void:
	var world := _world()
	var cm := world.get_node("ChunkManager")

	var goblin: Node3D = (load(BODY_SCRIPT) as Script).new()
	goblin.name = "Goblin"
	goblin.position = GOBLIN_AT
	world.add_child(goblin)
	var enemy: Node3D = (load(ENEMY_SCRIPT) as Script).new()
	enemy.name = "Enemy"
	world.add_child(enemy)
	await process_frame

	# The blow itself: a body beside it, at its own height, in the open.
	if _blow(goblin, enemy, NEAR) != 1:
		_fail("no blow landed on a body one block away in the open")
	if int(enemy.get("hits")) != 1:
		_fail("%d blows for one ask" % int(enemy.get("hits")))
	var damage: Array = enemy.get("damage")
	if damage.size() != 1 or float(damage[0]) != 2.0:
		_fail("the blow cost %s half-hearts, expected 2" % [damage])

	# ...a player on the step in front of it: inside the reach in three dimensions.
	if not _reaches(goblin, enemy, UP_ON_A_STEP):
		_fail("a player standing on the block in front of it is out of reach")
	if int(enemy.get("hits")) != 2:
		_fail("it did not hit the player on the step in front of it")

	# ...a player three blocks overhead: nothing in the game hits that.
	var before := int(enemy.get("hits"))
	if _reaches(goblin, enemy, Vector3(GOBLIN_AT.x, GOBLIN_AT.y + ABOVE, GOBLIN_AT.z)):
		_fail("a player %.0f blocks overhead is in reach" % ABOVE)
	if int(enemy.get("hits")) != before:
		_fail("it hit a player %.0f blocks overhead" % ABOVE)

	# ...and twenty-five overhead, which is what was seen in play: the reach used to be the flat
	# distance, so height was not measured at all.
	if _reaches(goblin, enemy, Vector3(GOBLIN_AT.x, GOBLIN_AT.y + WAY_ABOVE, GOBLIN_AT.z)):
		_fail("a player %.0f blocks overhead is in reach: the reach is measuring the flat "
			% WAY_ABOVE + "distance again")
	if int(enemy.get("hits")) != before:
		_fail("it hit a player %.0f blocks overhead" % WAY_ABOVE)

	# A wall between them: in reach, in the open, but not in sight. The world's own ray is what
	# says so, and the blow does not land.
	cm.set("sight_blocked", true)
	before = int(enemy.get("hits"))
	if _reaches(goblin, enemy, NEAR):
		_fail("a player behind a wall is in reach")
	if int(enemy.get("hits")) != before:
		_fail("it hit a player through a wall")

	# ...and with the wall gone it hits again, so the sight test is what stopped it and not
	# something the wall left behind.
	cm.set("sight_blocked", false)
	if not _reaches(goblin, enemy, NEAR):
		_fail("it will not hit a player in the open after a wall was in the way")
	if int(enemy.get("hits")) != before + 1:
		_fail("no blow landed once the wall was gone")

	if ok:
		print("PROBE PASS: the blow reaches %.2f in three dimensions and only through open air -- "
			% 1.45 + "%d blows landed, none through a wall and none at a body overhead"
			% int(enemy.get("hits")))
	quit(0 if ok else 1)
