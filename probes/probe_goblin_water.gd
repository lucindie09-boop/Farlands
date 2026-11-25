extends SceneTree
## Headless check of the two halves of "water and heights": a goblin shoved into a pond gets out
## of it, and a goblin dropped off a ledge loses half-hearts.
##
## The planner refuses liquid columns outright -- a route may not even BEGIN in one -- so a body
## in water has no route to steer. The game's answer is physical, not a route: a mob floats and
## paddles at the nearest ground it can see. That is what `_swim` is, and it cannot be checked by
## arithmetic in the pure test binary, because it is a body being moved through a world.
##
## So this builds its own pond in the real world: a five-by-five box, two blocks of water deep,
## floored and walled in stone a few blocks from the player, with the wall's top at the water
## line (which is the shape a pond cut into level ground has, and the one a swimmer climbs out
## of). A goblin is put on the bottom of it and watched for ten seconds. Every cell the probe
## writes is snapshotted and put back at the end.
##
## The third check is the one a player meets first: chasing something that is standing IN the
## water. Water is slow, not dangerous, so a chase wades and swims after its target instead of
## stopping at the shore. The planner cannot do it -- it refuses to enter liquid -- so the body
## steers straight at the target, and a stand-in prey (a node with health, since the walker only
## needs a position to chase) is put in the middle of the pond to be chased.
##
##   probes/probe_goblin_water.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_water.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
const SILENCED_IDLE := 1000000
## The pond's footprint, in columns, and how many cells of water are in it. Two deep is over the
## body's own head at the bottom, and leaves the wall's top at the water line.
const POND := 5
const DEPTH := 2
## The drop for the fall half. `SAFE_FALL_DISTANCE` is 3, so ten blocks owe floor(10 - 3) = 7 of
## the body's 12 half-hearts. The tolerance is a block's worth: the last tick of a landing can be
## short by up to one, and which side of an integer the sum lands on is not what is under test.
const DROP := 10.0
const SAFE_FALL := 3.0
const MAX_HEALTH := 12.0
const SWIM_FRAMES := 900
const FALL_FRAMES := 400
## The chase half: `CHASE_REACH` in the walker is 1.45 in three dimensions, and the goblin has to
## close to it across the pond. `CHASE_ANGER` is in game ticks (20 a second), well past the frames
## it is given.
const CHASE_FRAMES := 600
const CHASE_ANGER := 1200
const CHASE_NEAR := 1.6
## A prey the walker can chase: it wants a position, and health to swing at. Nothing else.
const PREY_SCRIPT := """extends Node3D
var health := 20.0
func get_health() -> float:
\treturn health
func set_health(value: float) -> void:
\thealth = value
"""

var ok := true
## Every cell this probe changed, so the world is put back exactly as it was found.
var _writes := []


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


## The top surface of a column, as the feet position standing on it. Scanned from a little above
## the `hint` rather than from the top of the world.
func _surface(cm: Node, x: int, z: int, hint: int) -> Vector3:
	var y := hint + 3
	while y > hint - 24 and y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(float(x) + 0.5, float(y + 1), float(z) + 0.5)
		y -= 1
	return Vector3.INF


func _write(cm: Node, x: int, y: int, z: int, id: int) -> void:
	_writes.append([x, y, z, int(cm.get_block(x, y, z))])
	cm.set_block(x, y, z, id)


func _restore(cm: Node) -> void:
	for i in range(_writes.size() - 1, -1, -1):
		var w = _writes[i]
		cm.set_block(int(w[0]), int(w[1]), int(w[2]), int(w[3]))
	_writes.clear()


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var cm := main.get_node_or_null("ChunkManager")
	var player := main.get_node_or_null("Player") as Node3D
	if cm == null or player == null:
		print("PROBE FAIL: the world has no ChunkManager or Player")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break

	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	body.add_to_group("goblin")
	main.add_child(body)
	body.set("_idle_ticks", SILENCED_IDLE)
	var sim: Object = body.get("_sim")
	if sim == null:
		print("PROBE FAIL: no body sim")
		quit(1)
		return

	# --- The fall ---------------------------------------------------------------
	var ground := _surface(cm, floori(player.global_position.x), floori(player.global_position.z),
		floori(player.global_position.y))
	if ground == Vector3.INF:
		print("PROBE FAIL: no ground under the player")
		quit(1)
		return
	sim.call("reset", ground)
	for i in 6:
		await process_frame
	if absf(float(body.call("get_health")) - MAX_HEALTH) > 0.01:
		_fail("a goblin set down on the ground is already hurt")
	sim.call("reset", ground + Vector3(0.0, DROP, 0.0))
	var fell := 0
	var airborne := false
	while fell < FALL_FRAMES:
		await process_frame
		fell += 1
		if not bool(sim.call("is_on_ground")):
			airborne = true
		elif airborne:
			break
	for i in 4:
		await process_frame
	var health := float(body.call("get_health"))
	var owed := floorf(DROP - SAFE_FALL)
	print("PROBE fell %.0f blocks: health %.0f, %.0f of the 12 owed" % [DROP, health, owed])
	if health >= MAX_HEALTH:
		_fail("a %.0f-block fall cost it nothing" % DROP)
	elif absf((MAX_HEALTH - health) - owed) > 1.5:
		_fail("a %.0f-block fall cost %.0f half-hearts, not %.0f"
			% [DROP, MAX_HEALTH - health, owed])

	# --- The pond --------------------------------------------------------------
	# A sealed box: stone floor under the water, stone walls two deep whose TOP is at the water
	# line, and the air above it cleared so nothing else is in the way.
	var water = cm.BLOCK_WATER
	var stone := BlockTextures.get_block_id_by_name("stone")
	if stone <= 0:
		_fail("no stone block to build the pond with")
		quit(1)
		return
	var base_y := floori(ground.y)
	var cx := floori(ground.x) + 7
	var cz := floori(ground.z)
	var half := POND >> 1
	for dx in range(-half - 1, half + 2):
		for dz in range(-half - 1, half + 2):
			var x := cx + dx
			var z := cz + dz
			for ly in range(base_y + 2, base_y + 6):
				_write(cm, x, ly, z, 0)
			if maxi(absi(dx), absi(dz)) > half:
				_write(cm, x, base_y + 1, z, stone)
				_write(cm, x, base_y, z, stone)
			else:
				_write(cm, x, base_y - 1, z, stone)
				_write(cm, x, base_y, z, water)
				_write(cm, x, base_y + 1, z, water)
	for i in 4:
		await process_frame
	print("PROBE pond: %d x %d, %d deep, floor at %d, wall top at %d, around (%d, %d)"
		% [POND, POND, DEPTH, base_y - 1, base_y + 2, cx, cz])

	# On the bottom of it, which is the case the planner cannot answer at all.
	sim.call("reset", Vector3(float(cx) + 0.5, float(base_y), float(cz) + 0.5))
	for i in 6:
		await process_frame
	var started_wet := bool(sim.call("is_in_water"))
	print("PROBE dropped in: in_water=%s at %s"
		% [str(started_wet), str(sim.call("get_tick_position"))])
	if not started_wet:
		_fail("the goblin did not go into the pond")

	var out := false
	var frames := 0
	while frames < SWIM_FRAMES:
		await process_frame
		frames += 1
		var at: Vector3 = sim.call("get_tick_position")
		var wet := bool(sim.call("is_in_water"))
		# Out is where a shore is: dry, STANDING, and at the wall's own top level. It does not have
		# to be clear of the footprint to count -- a body standing on the inner lip of the wall is
		# on the rim, and its box overlaps the water column while its feet are on the stone.
		var ashore := at.y >= float(base_y + 2) - 0.01
		if frames % 40 == 0:
			print("PROBE swim %d: at=%s wet=%s ground=%s"
				% [frames, str(at), str(wet), str(sim.call("is_on_ground"))])
		if not wet and ashore and bool(sim.call("is_on_ground")):
			out = true
			print("PROBE got out after %d frames at %s" % [frames, str(at)])
			break
	if not out:
		_fail("it never left the pond (at %s, wet=%s)"
			% [str(sim.call("get_tick_position")), str(sim.call("is_in_water"))])

	# --- The chase across water ------------------------------------------------
	# The player's own report: a goblin that would rather walk a dozen blocks round a two-block pond
	# than wade two blocks through it. Water is slow, not dangerous, so the planner prices a wet step
	# above a dry one and allows it, and the swim follows that route cell by cell. The shape here is
	# chosen so there is nothing to prefer: an island is built in the middle of the pond with a cell
	# of water all the way round it, and the prey stands on it. Every way across is wet.
	var island := 3
	var ihalf := island >> 1
	for dx in range(-ihalf, ihalf + 1):
		for dz in range(-ihalf, ihalf + 1):
			_write(cm, cx + dx, base_y, cz + dz, stone)
			_write(cm, cx + dx, base_y + 1, cz + dz, stone)
	for i in 4:
		await process_frame
	var prey_script := GDScript.new()
	prey_script.source_code = PREY_SCRIPT
	prey_script.reload()
	var prey := Node3D.new()
	prey.name = "Prey"
	prey.set_script(prey_script)
	main.add_child(prey)
	prey.global_position = Vector3(float(cx) + 0.5, float(base_y + 2), float(cz) + 0.5)
	# On the rim itself: dry, a clear look across the water, and no way onto the island but through
	# it. That column is the stone the probe built, so it is the one place a body is certainly
	# standing on something rather than inside the natural ground.
	var start := Vector3(float(cx + half + 1) + 0.5, float(base_y + 2), float(cz) + 0.5)
	sim.call("reset", start)
	body.set("_enemy", prey)
	body.set("_anger_ticks", CHASE_ANGER)
	var chase := 0
	var waded := false
	var closed := false
	var struck := false
	while chase < CHASE_FRAMES:
		await process_frame
		chase += 1
		var at: Vector3 = sim.call("get_tick_position")
		if bool(sim.call("is_in_water")):
			waded = true
		if at.distance_to(prey.global_position) <= CHASE_NEAR and not closed:
			closed = true
			print("PROBE chase: reached the prey at %s after %d frames" % [str(at), chase])
		if float(prey.call("get_health")) < 20.0:
			struck = true
			break
	print("PROBE chase across: from %s, waded=%s, closed=%s, struck=%s after %d frames, at %s"
		% [str(start), str(waded), str(closed), str(struck), chase,
			str(sim.call("get_tick_position"))])
	if not waded:
		_fail("the chase did not cross the water to a target it could see across it")
	elif not closed:
		_fail("the chase crossed the water but never reached the prey on the island")
	elif not struck:
		_fail("it never landed a blow on the prey it crossed the water for")

	# --- The chase after a target standing IN the water -------------------------
	# The other half, and the one a route cannot answer: the goal's ring anchors on DRY ground, so a
	# player swimming resolves to the bank. The walker steers straight at them instead, wades in
	# after them and swings -- and the look is the same ray the reach is measured through, so water
	# does not make the body untouchable.
	prey.global_position = Vector3(float(cx - half) + 0.5, float(base_y + 1), float(cz) + 0.5)
	var before := float(prey.call("get_health"))
	# Back to the far rim, so the way to it is the water and not the island's edge.
	sim.call("reset", start)
	body.set("_anger_ticks", CHASE_ANGER)
	var wet_chase := 0
	var in_water := false
	while wet_chase < CHASE_FRAMES:
		await process_frame
		wet_chase += 1
		if bool(sim.call("is_in_water")):
			in_water = true
		if float(prey.call("get_health")) < before:
			break
	print("PROBE chase in water: in_water=%s after %d frames, prey health %s of %s"
		% [str(in_water), wet_chase, str(prey.call("get_health")), str(before)])
	if not in_water:
		_fail("the chase did not enter the water after a target standing in it")
	elif float(prey.call("get_health")) >= before:
		_fail("a target standing in water was never struck")
	body.set("_enemy", null)
	main.remove_child(prey)
	prey.free()

	# The pond goes back the way it was found.
	_restore(cm)
	print("PROBE health after all of it: %.0f" % float(body.call("get_health")))
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
