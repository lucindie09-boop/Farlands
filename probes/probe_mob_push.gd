extends SceneTree
## Headless check that bodies are bodies: two goblins put in one place come apart, a third put in
## the same place does not stack on them, and one put inside the player is pushed off him.
##
## The arithmetic is `src/engine/entity_push.cpp` and is pinned by `tests/test_entity_push.cpp`;
## what that cannot show is the half that makes it a behaviour -- that the walker asks for it once
## a tick, that the push becomes VELOCITY the sim then resolves against the ground, and that a
## body which is exactly inside another does not stay there. So this spawns real bodies in the
## real world and measures where they end up.
##
##   probes/probe_mob_push.gd  (Godot --headless --path <project> --script res://probes/probe_mob_push.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
const W := preload("res://probes/probe_world.gd")
const SILENCED_IDLE := 1000000
## A goblin's own box, which is what the pair maths is given (the same one the resolver moves).
const BODY := Vector3(0.6, 1.8, 0.6)
## How far apart two bodies have to be before their boxes stop intersecting: one body width.
const APART := 0.6
## The pushes are 0.03 to 0.04 a tick, so a pair that starts 0.15 apart needs a few dozen ticks
## to reach a whole width. A pack of three takes longer, because each push is answered once.
const SETTLE_FRAMES := 600
const PLAYER_FRAMES := 400

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _spawn(main: Node, at: Vector3) -> Node3D:
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	body.add_to_group("goblin")
	# Before the node enters the tree: `_ready` resets the sim at the node's own position, and a
	# body that resets at the origin first is a body inside the hill, hurt before the check starts.
	body.position = at
	main.add_child(body)
	body.set("_idle_ticks", SILENCED_IDLE)
	return body


func _feet(body: Node3D) -> Vector3:
	var sim: Object = body.get("_sim")
	return sim.get_tick_position() if sim != null else body.global_position


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	var cm := main.get_node_or_null("ChunkManager")
	if player == null or cm == null:
		print("PROBE FAIL: the world has no Player or ChunkManager")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	if not await W.wait_settled(self, cm):
		print("PROBE FAIL: the world never settled (%s)" % str(cm.call("get_stream_snapshot")))
		quit(1)
		return

	# --- A pack in one place ---------------------------------------------------
	# Three goblins inside one body's width of each other: nothing walks (the wander is silenced),
	# so the only thing that can separate them is the push.
	# The ground four blocks over, asked of a spare body: `MobBody.is_solid_at` is the view the
	# bodies move through, and the column's surface is the first standable cell from above rather
	# than the first solid one -- under a roof those are different cells (a ceiling and a floor).
	var ruler := Node3D.new()
	ruler.name = "Ruler"
	ruler.set_script(load(BODY_SCRIPT))
	ruler.position = player.global_position
	main.add_child(ruler)
	ruler.remove_from_group("goblin")
	var rsim: Object = ruler.get("_sim")
	var px := floori(player.global_position.x) + 4
	var pz := floori(player.global_position.z)
	var feet_y := -1
	var padded := false
	if rsim != null:
		# Level ground four blocks over, or failing that the player's own column: he is standing on
		# a floor, so it is the one column with a known answer, and this world's surface is not the
		# same from one run to the next.
		var spot := W.flat_spot(rsim, px, pz, floori(player.global_position.y) + 40, 1, 6)
		if spot != Vector3.INF:
			feet_y = floori(spot.y)
			px = floori(spot.x)
			pz = floori(spot.z)
		else:
			feet_y = W.stand_in_column(rsim, floori(player.global_position.x),
				floori(player.global_position.z), floori(player.global_position.y) + 40)
			px = floori(player.global_position.x)
			pz = floori(player.global_position.z)
		if feet_y < 0:
			# No dry ground to stand on at all -- the world can put its spawn at the bottom of an
			# ocean or insde a cave -- so the probe lays its own floor and proves it.
			feet_y = floori(player.global_position.y) + 1
			padded = await W.pad(self, cm, rsim, px, feet_y - 1, pz, 2, 2, 5)
			if not padded:
				print("PROBE FAIL: no dry ground, and the pad did not take")
				quit(1)
				return
	ruler.queue_free()
	for i in 3:
		await process_frame
	if feet_y < 0:
		print("PROBE FAIL: no ground to stand a pack on at (%d, %d)" % [px, pz])
		quit(1)
		return
	var origin := Vector3(float(px) + 0.5, float(feet_y), float(pz) + 0.5)
	var pack: Array[Node3D] = []
	for offset in [Vector3(0.0, 0.0, 0.0), Vector3(0.15, 0.0, 0.0), Vector3(0.0, 0.0, 0.15)]:
		pack.append(_spawn(main, origin + offset))
	# Settled on the ground first, so a body falling is not mistaken for one being pushed.
	for i in 30:
		await process_frame
	var start_gap := 0.0
	for i in pack.size():
		for j in range(i + 1, pack.size()):
			start_gap = maxf(start_gap, _feet(pack[i]).distance_to(_feet(pack[j])))
	print("PROBE pack starts %.3f apart" % start_gap)

	var frames := 0
	var widest := 0.0
	while frames < SETTLE_FRAMES:
		await process_frame
		frames += 1
		widest = 0.0
		for i in pack.size():
			for j in range(i + 1, pack.size()):
				widest = maxf(widest, _feet(pack[i]).distance_to(_feet(pack[j])))
		if widest >= APART - 0.01:
			break
	print("PROBE pack: %.3f apart after %d frames (needs %.2f)" % [widest, frames, APART])
	if widest < APART - 0.05:
		_fail("three bodies in one place did not come apart (%.3f apart after %d frames)"
			% [widest, frames])
	# ...and nothing was lifted off the ground doing it: the push is horizontal, and a body that
	# is still standing is what the walk expects to find.
	for body in pack:
		var sim: Object = body.get("_sim")
		if sim != null and absf(_feet(body).y - origin.y) > 1.0:
			_fail("a goblin was pushed off its floor (y %.2f, was %.2f)"
				% [_feet(body).y, origin.y])

	# --- A body inside the player ---------------------------------------------
	# The player is a body too. The goblin is put 0.2 off his centre -- the pair maths has no
	# direction at exactly zero, which is the reference's own hole -- and has to be pushed clear
	# of his whole width, without the player's own position being written by this.
	# The player's position NOW, not the one he was read at: this world drops him through the
	# chunks that have not arrived, and a leaner placed against where he used to be is a leaner
	# measuring nothing.
	var settled := 0
	while settled < 300 and not player.is_on_floor():
		await process_frame
		settled += 1
	# A player who is dead, or swimming at the bottom of the ocean the probe's own default-seed
	# world spawns him over, is not a standing body: `PlayerController::push` is a body's answer and
	# a corpse or a swimmer has none. The pair arithmetic is pinned by
	# `tests/test_entity_push.cpp` whatever this says; what this half adds is that the wiring is
	# there, so it is reported as unexercised rather than passed or failed.
	var player_ok := not bool(player.call("is_dead")) and bool(player.call("is_on_floor")) 		and not bool(player.call("is_in_water"))
	if not player_ok:
		print("PROBE note: the player push was NOT exercised (dead=%s on_floor=%s water=%s health=%s at=%s)"
			% [str(player.call("is_dead")), str(player.call("is_on_floor")),
				str(player.call("is_in_water")), str(player.get("health")),
				str(player.global_position)])
	var feet: Vector3 = player.global_position
	var leaner := _spawn(main, feet + Vector3(0.2, 0.0, 0.0))
	# Two bodies spawned on the same frame order; let the leaner settle.
	for i in 30:
		await process_frame
	var gap_start := Vector2(_feet(leaner).x - feet.x, _feet(leaner).z - feet.z).length()
	var player_frames := 0
	var gap := gap_start
	while player_frames < PLAYER_FRAMES:
		await process_frame
		player_frames += 1
		feet = player.global_position
		gap = Vector2(_feet(leaner).x - feet.x, _feet(leaner).z - feet.z).length()
		if gap >= APART - 0.01:
			break
	print("PROBE player: leaner %.3f from the player after %d frames (was %.3f)"
		% [gap, player_frames, gap_start])
	# Why it did not move, in one line: is the walker even allowed to push the player, and does the
	# pair maths see them overlapping?
	var leaner_sim: Object = leaner.get("_sim")
	if leaner_sim != null:
		var pair: Dictionary = Bodies.push_pair(_feet(leaner), BODY, player.global_position, BODY)
		print("PROBE player pair: has_push=%s empty=%s leaner=%s player=%s dy=%.2f"
			% [str(player.has_method("push")), str(pair.is_empty()), str(_feet(leaner)),
				str(player.global_position), _feet(leaner).y - player.global_position.y])
	if player_ok and gap < APART - 0.05:
		_fail("a body inside the player was not pushed off him (%.3f from him after %d frames)"
			% [gap, player_frames])

	leaner.queue_free()
	for body in pack:
		body.queue_free()
	for i in 3:
		await process_frame
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
