extends SceneTree
## Diagnostic: can a spawned mob body stand up in the real world, and what does the world look
## like when it does? The mob probes (water, punch, roam, push) all spawn bodies that freeze at
## their reset cell and take suffocation damage, which is either a body inside terrain or a body
## in a chunk that has not been installed yet. This prints the two views of the same cells -- the
## chunk manager's block read and the body's own resolver -- before and after the world settles,
## then puts one goblin on the ground and shoves it to see whether it moves at all.
##
##   probes/probe_world_ready.gd  (Godot --headless --path <project> --script res://probes/probe_world_ready.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
const W := preload("res://probes/probe_world.gd")

var ok := true


func _initialize() -> void:
	_run()


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
	var at: Vector3 = player.global_position
	print("PROBE player on the floor after %d frames at %s; settled=%s"
		% [waited, str(at), str(cm.call("is_fill_settled"))])
	print("PROBE stream: %s" % str(cm.call("get_stream_snapshot")))

	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	body.add_to_group("goblin")
	main.add_child(body)
	var sim: Object = body.get("_sim")
	if sim == null:
		print("PROBE FAIL: no body sim")
		quit(1)
		return

	# The two views of the same column, before the world has finished installing.
	_report(cm, sim, at, 0, "before settle")
	var frames := 0
	while frames < 6000 and not bool(cm.call("is_fill_settled")):
		await process_frame
		frames += 1
	print("PROBE settled=%s after %d more frames; stream: %s"
		% [str(cm.call("is_fill_settled")), frames, str(cm.call("get_stream_snapshot"))])
	_report(cm, sim, at, 0, "after settle")
	print("PROBE settled through the helper: %s"
		% str(await W.wait_settled(self, cm)))

	# One on the ground, then shoved: a body that is not inside terrain has to move.
	# From well above the area, and through the body's own resolver: the downward-from-the-feet
	# scan this probe used to do returns a CEILING under any roof, and a body put on a ceiling is
	# inside the hill.
	var ground := W.stand_in_column(sim, floori(at.x), floori(at.z), floori(at.y) + 40)
	if ground < 0:
		print("PROBE FAIL: no solid ground under the player through the body's own resolver")
		quit(1)
		return
	sim.call("reset", Vector3(float(floori(at.x)) + 0.5, float(ground), float(floori(at.z)) + 0.5))
	for i in 20:
		await process_frame
	print("PROBE spawned: at=%s ground=%s wet=%s health=%s"
		% [str(sim.call("get_tick_position")), str(sim.call("is_on_ground")),
			str(sim.call("is_in_water")), str(body.call("get_health"))])
	sim.call("shove", sim.call("get_tick_position") + Vector3(1.0, 0.0, 0.0), Vector3.ZERO)
	var before: Vector3 = sim.call("get_tick_position")
	for i in 30:
		await process_frame
	var after: Vector3 = sim.call("get_tick_position")
	print("PROBE shoved: %s -> %s (moved %.3f), health %s"
		% [str(before), str(after), before.distance_to(after), str(body.call("get_health"))])
	# The trace's own sample line for this body, formatted the way a session's file gets it: a `%`
	# count that does not match its tuple throws only when a real session's recorder asks, which is
	# the wrong place to find out.
	var state := str(body.call("trace_state"))
	print("PROBE trace_state: %s" % state)
	if not state.contains("wish=") or not state.contains("strokes="):
		print("PROBE FAIL: the trace sample is missing the walk's own numbers")
		ok = false
	if before.distance_to(after) < 0.05:
		print("PROBE FAIL: a shoved body on the ground did not move")
		ok = false
	body.queue_free()
	for i in 3:
		await process_frame
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)


## The top solid cell of a column, scanned with the body's own resolver (the view that decides
## whether the body stands or suffocates).
func _top_through_body(sim: Object, x: int, z: int, hint: int) -> int:
	for y in range(hint + 8, hint - 24, -1):
		if bool(sim.call("is_solid_at", x, y, z)):
			return y
	return -1


func _report(cm: Node, sim: Object, at: Vector3, dx: int, tag: String) -> void:
	var x := floori(at.x) + dx
	var z := floori(at.z)
	var body_top := _top_through_body(sim, x, z, floori(at.y))
	var cm_id := int(cm.call("get_block", x, body_top, z)) if body_top >= 0 else -1
	print("PROBE %s column (%d,%d): body-view top=%d (block %d), manager read of that cell=%d"
		% [tag, x, z, body_top, cm_id, cm_id])
