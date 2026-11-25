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
	# Anger is what keeps a fight going, and it runs out: every ask here has to be inside it.
	goblin.set("_anger_ticks", 600)
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

	# Closing the last pace: in reach but further out than CHASE_STOP, it walks AT the player rather
	# than standing at the edge of its own arm, which is what a chase looked like before -- a goblin
	# halted a whole reach away and swung at the air in front of its nose.
	goblin.set("_health", 12.0)
	_blow(goblin, enemy, NEAR)
	if not bool(goblin.get("_closing")):
		_fail("in reach at one block it did not close the last pace")
	goblin.set("_job", 0)
	goblin.call("_steer")
	var wish: Vector3 = goblin.get("_wish")
	if Vector2(wish.x, wish.z).length() < 0.5:
		_fail("closing but not walking at the player (wish %s)" % wish)
	_blow(goblin, enemy, Vector3(GOBLIN_AT.x + 0.8, GOBLIN_AT.y, GOBLIN_AT.z))
	if bool(goblin.get("_closing")):
		_fail("still walking into a player it is already against")

	# It runs at TWO hearts, four of its twelve half-hearts: a goblin on two and a half still fights.
	goblin.set("_anger_ticks", 600)
	goblin.set("_health", 5.0)
	_blow(goblin, enemy, NEAR)
	if bool(goblin.get("_fleeing")):
		_fail("it ran with 5 half-hearts left: the run starts at two hearts, not two and a half")
	goblin.set("_health", 4.0)
	_blow(goblin, enemy, NEAR)
	if not bool(goblin.get("_fleeing")):
		_fail("it kept fighting at 4 half-hearts: two hearts is where it runs")
	goblin.set("_health", 12.0)
	goblin.set("_fleeing", false)

	# A chase has a length: a player 40 blocks away is past the follow range of 35, so the target
	# is dropped rather than followed across the world.
	goblin.set("_health", 12.0)
	before = int(enemy.get("hits"))
	_blow(goblin, enemy, Vector3(GOBLIN_AT.x + 40.0, GOBLIN_AT.y, GOBLIN_AT.z))
	if goblin.get("_enemy") != null:
		_fail("still chasing a player 40 blocks away: the leash is 35")
	if int(enemy.get("hits")) != before:
		_fail("hit a player 40 blocks away")

	# ...and its anger times out on its own, which is the other half of forgetting.
	goblin.set("_enemy", enemy)
	goblin.set("_anger_ticks", 1)
	goblin.call("_update_enemy", 20)
	if goblin.get("_enemy") != null:
		_fail("stayed angry after its twenty-to-forty-second clock ran out")

	# A pack is a pack: one goblin hurt by the player wakes every goblin within 32 blocks of it,
	# which is the game's own pigman rule, so a neighbour joins without being touched.
	var player := world.get_node("Player") as Node3D
	player.position = GOBLIN_AT + Vector3(0.6, 0.0, 0.0)
	goblin.add_to_group("goblin")
	var neighbour: Node3D = (load(BODY_SCRIPT) as Script).new()
	neighbour.name = "GoblinNeighbour"
	neighbour.add_to_group("goblin")
	neighbour.position = GOBLIN_AT + Vector3(20.0, 0.0, 0.0)
	world.add_child(neighbour)
	var distant: Node3D = (load(BODY_SCRIPT) as Script).new()
	distant.name = "GoblinDistant"
	distant.add_to_group("goblin")
	distant.position = GOBLIN_AT + Vector3(40.0, 0.0, 0.0)
	world.add_child(distant)
	await process_frame
	goblin.set("_health", 12.0)
	goblin.set("_enemy", null)
	goblin.call("apply_knockback", player.global_position)
	if goblin.get("_enemy") != player:
		_fail("a punched goblin did not turn on the player who hit it")
	if neighbour.get("_enemy") != player:
		_fail("a goblin 20 blocks away did not join the fight its neighbour started")
	if distant.get("_enemy") != null:
		_fail("a goblin 40 blocks away joined a fight 32 blocks outside the shout")

	if ok:
		print("PROBE PASS: the blow reaches %.2f in three dimensions and only through open air -- "
			% 1.45 + "%d blows landed, none through a wall and none at a body overhead"
			% int(enemy.get("hits")))
	quit(0 if ok else 1)
