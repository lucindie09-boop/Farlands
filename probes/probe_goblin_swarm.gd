extends SceneTree
## What a CROWD of goblins costs the CPU: pathfinding and collision at N = 0, 25, 50, 100, 200.
##
## This is not a draw-call probe. `probes/probe_goblin_drawcalls.gd` pins the crowd at six draws
## (one MultiMesh per rig part), and pathfinding and collision submit no geometry at all — a
## planner run is worker time and a collision query is main-thread math. So the number this probe
## is after is FRAME TIME: the goblin share of `TIME_PHYSICS_PROCESS` and `TIME_PROCESS`, and
## whether the worst frame of a twenty-second window stays under the 33 ms a 30 Hz target needs.
##
## Four scenarios per N, each after a two-second settle, each measured over a twenty-second
## window. They isolate the layers from the bottom up:
##
##   render  every goblin's `_physics_process` is OFF, so what is left is the model, the batch and
##           the torch lights — the floor a crowd costs before any behaviour runs.
##   sim     physics is back on, the wander is silenced (`_idle_ticks` pinned) and nobody is angry:
##           the per-tick decision layer, the collision probes and the crowd's own push, with no
##           planner work at all.
##   chase   every goblin is angry at the player (`take_anger`), who stands still: the planner and
##           the crowd field under the real thing, a hundred of them re-planning on their own
##           4-to-11-tick clocks.
##   moving  the same chase, with the player moved four blocks every two seconds round a small
##           square: every answer is invalidated before it is walked, so chases re-plan.
##
## Per scenario it records the mean process/physics time, the FPS, the worst single frame (the
## largest process+physics of the window, which is the main thread's own cost), the draw calls
## (flat, or something is submitting per body), the peak in-flight path jobs, the peak pool queue
## and the planner's own worker cost — `path_ms`/`field_ms` divided by the window's wall seconds is
## "path cores".
##
## The session's nav trace is claimed and muted: a hundred bodies would otherwise write a trace
## line per second each, and the recorder would be the thing being measured.
##
## WINDOWED (the counters come from a real renderer), and slow on purpose: five counts x four
## scenarios x twenty-two seconds is about eight minutes, plus the world settle.
##
##   probes/run_probe_shot.sh probes/probe_goblin_swarm.gd 1200
##
## or, without touching the saved world (this probe writes none):
##
##   "$GODOT" --path . --script res://probes/probe_goblin_swarm.gd

const GOBLIN_SCRIPT := "res://scripts/goblin.gd"
const MODEL_SCRIPT := "res://scripts/goblin_model.gd"
const MODEL_PATH := "res://assets/models/Goblin.obj"
const NAV_TRACE := "res://scripts/nav_trace.gd"
const COUNTS := [0, 25, 50, 100, 200]
const WINDOW_SECONDS := 20.0
const SETTLE_SECONDS := 2.0
## Close enough that the bodies are always leaning on each other: the crowd's push, and the
## collision resolver's own multi-box queries, are half of what this measures.
const SPACING := 1.2
const COLUMNS := 14
## How far from the player the crowd is put. It has to be inside the goblin's FOLLOW RANGE — a
## target further off than that is forgotten on the first tick and the scenario measures nothing —
## and well outside the 1.45-block attack reach, or the crowd arrives before the settle is over
## and the scenario measures melee. `data/mobs.json`'s follow range is the number that decides it.
const CROWD_DISTANCE := 12.0
## The scenarios, in order: `render` is the crowd's draw cost with the sim off, `sim` the body's
## own tick cost, `paths` the planner's with a hundred bodies asking at once, and the two chases
## the decision layer on top of that.
const SCENARIOS := ["render", "sim", "paths", "chase", "moving chase"]
## The largest height difference from the crowd's own centre a stand may have: a body on the far
## side of a slope is a different experiment.
const STAND_SLACK := 2
## Pinned into `_idle_ticks`: a body that never rolls a stroll.
const SILENCED_IDLE := 1000000
## How often, and how far, the player steps in the moving scenario. A four-block hop every two
## seconds round a square keeps them inside the chase leash (35 blocks) while invalidating every
## plan that was made for the last spot.
const MOVE_EVERY := 2.0
const MOVE_STEP := 4.0
const TARGET_FRAME_MS := 33.0

var ok := true
## The sweep as run. `-- --quick` (after the script path) runs two counts over a five-second
## window, which proves the probe and its four scenarios before paying for the whole table.
var counts: Array = COUNTS
var window := WINDOW_SECONDS


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.has("--quick"):
		counts = [25]
		window = 3.0
	# `-- --n=0 --n=100 --window=20` sweeps just those counts; a lone `--n=100` is the crowd.
	var seen_n := false
	for arg in args:
		if arg.begins_with("--n="):
			if not seen_n:
				counts = []
				seen_n = true
			counts.append(int(arg.trim_prefix("--n=")))
		elif arg.begins_with("--window="):
			window = float(arg.trim_prefix("--window="))
	_run()


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_usec()
	while float(Time.get_ticks_usec() - t0) / 1_000_000.0 < seconds:
		await process_frame


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


## One goblin, built the way `probes/probe_goblin_drawcalls.gd` builds one (which is the way
## `scripts/goblin_spawn.gd` does): the entity body carrying goblin.gd in the two groups, and the
## Model child the model script draws with.
func _spawn_goblin(parent: Node, at: Vector3, mesh: Mesh, body_script: Script,
		model_script: Script) -> Node3D:
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(body_script)
	body.add_to_group("pose_clone")
	body.add_to_group("goblin")
	body.position = at
	parent.add_child(body)
	var model := Node3D.new()
	model.name = "Model"
	model.set_script(model_script)
	model.set("source_mesh", mesh)
	body.add_child(model)
	# IMMORTAL, and only because this is a benchmark: a body that falls four blocks takes a
	# half-heart, and one that dies is freed and stops being part of the crowd — which turns a
	# measurement of a hundred goblins into a measurement of ninety. `_take_hurt` still runs, so
	# the receiving chain is still in the frame; it just cannot empty the bar.
	body.set("_max_health", 1000000.0)
	body.set("_health", 1000000.0)
	return body


## `count` goblins in a grid centred on `origin`, packed at SPACING so they are always touching.
func _spawn_grid(cm: Node, parent: Node, origin: Vector3, count: int, mesh: Mesh,
		body_script: Script, model_script: Script) -> Array[Node3D]:
	var bodies: Array[Node3D] = []
	if count <= 0:
		return bodies
	var columns := mini(count, COLUMNS)
	var rows := ceili(float(count) / float(COLUMNS))
	for i in count:
		var col := i % COLUMNS
		var row := floori(float(i) / float(COLUMNS))
		var at := origin + Vector3((float(col) - float(columns - 1) * 0.5) * SPACING, 0.0,
			(float(row) - float(rows - 1) * 0.5) * SPACING)
		# Each body on ITS OWN column's ground, and only where that ground is at the crowd's own
		# level: one height for the whole grid puts the far side of it over a slope or a drop, and
		# a body on a hillside is not one of a crowd standing in one area.
		var ground := _surface(cm, Vector3(at.x, origin.y, at.z))
		if ground == Vector3.INF:
			continue
		if absf(ground.y - origin.y) > float(STAND_SLACK):
			continue
		bodies.append(_spawn_goblin(parent, ground + Vector3(0.0, 0.5, 0.0), mesh, body_script,
			model_script))
	return bodies


## Waits for the world to stop streaming, so a generation install is not measured as a goblin.
func _settle_world(cm: Node, limit_seconds: float) -> void:
	var t0 := Time.get_ticks_usec()
	while cm.has_method("has_pending_mesh_work") and bool(cm.call("has_pending_mesh_work")):
		await process_frame
		if float(Time.get_ticks_usec() - t0) / 1_000_000.0 > limit_seconds:
			return


func _goblins(scene: Node) -> Array[Node]:
	var found: Array[Node] = []
	for node in scene.get_tree().get_nodes_in_group("goblin"):
		if scene.is_ancestor_of(node):
			found.append(node)
	return found


## The pool's counters, as the scenario delta and the peak are taken from.
func _pool(cm: Node) -> Dictionary:
	if not cm.has_method("get_pool_stats"):
		return {}
	return cm.get_pool_stats()


func _num(stats: Dictionary, key: String) -> float:
	return float(stats.get(key, 0.0))


## One scenario's twenty-second window.
func _measure(cm: Node, bodies: Array[Node3D], player: Node3D, scenario: String,
		moving: bool) -> void:
	await _seconds(SETTLE_SECONDS)
	var before := _pool(cm)
	var pending_max := 0
	var queue_max := 0
	var worst_ms := 0.0
	var process_sum := 0.0
	var physics_sum := 0.0
	var draws := 0
	var frames := 0
	var window_start := Time.get_ticks_usec()
	var next_move := MOVE_EVERY
	var square := [Vector3(MOVE_STEP, 0.0, 0.0), Vector3(0.0, 0.0, MOVE_STEP),
		Vector3(-MOVE_STEP, 0.0, 0.0), Vector3(0.0, 0.0, -MOVE_STEP)]
	var corner := 0
	while float(Time.get_ticks_usec() - window_start) / 1_000_000.0 < window:
		await process_frame
		frames += 1
		var process_ms := float(Performance.get_monitor(Performance.TIME_PROCESS)) * 1000.0
		var physics_ms := float(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)) * 1000.0
		process_sum += process_ms
		physics_sum += physics_ms
		worst_ms = maxf(worst_ms, process_ms + physics_ms)
		draws = int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
		if cm.has_method("get_pending_paths"):
			pending_max = maxi(pending_max, int(cm.call("get_pending_paths")))
		var pool := _pool(cm)
		queue_max = maxi(queue_max, int(_num(pool, "queue_size")))
		if scenario == "paths":
			# The planner's own load, with the chase's decisions out of the way: every body that has
			# neither a route nor a job in flight asks for one toward where the player is now, so the
			# window is a hundred concurrent asks against the admission cap, not a hundred wanderers.
			var target := player.global_position
			for body in bodies:
				if not is_instance_valid(body) or int(body.get("_job")) != 0:
					continue
				if not (body.get("_route") as Array).is_empty():
					continue
				body.call("walk_to", target)
		if moving and float(Time.get_ticks_usec() - window_start) / 1_000_000.0 >= next_move:
			next_move += MOVE_EVERY
			var step: Vector3 = square[corner % square.size()]
			corner += 1
			if player.has_method("teleport_to"):
				player.call("teleport_to", player.global_position + step)
	var after := _pool(cm)
	var wall := float(Time.get_ticks_usec() - window_start) / 1_000_000.0
	var path_ms := _num(after, "path_ms") - _num(before, "path_ms")
	var field_ms := _num(after, "field_ms") - _num(before, "field_ms")
	var path_tasks := int(_num(after, "path_tasks") - _num(before, "path_tasks"))
	var field_builds := 0
	if cm.has_method("get_flow_stats"):
		field_builds = int(float(cm.call("get_flow_stats").get("builds", 0.0)))
	var fps := Engine.get_frames_per_second()
	print("PROBE swarm n=%-3d %-12s process_ms=%.2f physics_ms=%.2f fps=%d worst_ms=%.2f "
		% [bodies.size(), scenario, process_sum / maxf(float(frames), 1.0),
			physics_sum / maxf(float(frames), 1.0), fps, worst_ms]
		+ "draws=%d pending_max=%d queue_max=%d path_ms=%.1f field_ms=%.1f path_tasks=%d "
			% [draws, pending_max, queue_max, path_ms, field_ms, path_tasks]
		+ "field_builds=%d cores=%.2f" % [field_builds, (path_ms + field_ms) / (wall * 1000.0)])
	if scenario == "moving chase" and worst_ms > TARGET_FRAME_MS:
		# Not a hard failure — the point of the probe is to report the number — but it is the bar,
		# so it is said out loud.
		print("PROBE NOTE: a path-related frame over %.0f ms in the moving-chase window (%.2f ms)"
			% [TARGET_FRAME_MS, worst_ms])
	# One body's own state, so a scenario that quietly did nothing is visible in the table rather
	# than inferred from a suspiciously cheap column: a chase with `enemy=null` measures the
	# wander, and `sim=null` measures an empty scene.
	if not bodies.is_empty() and is_instance_valid(bodies[0]):
		var sample := bodies[0]
		var angry := 0
		for body in bodies:
			if is_instance_valid(body) and body.get("_enemy") != null:
				angry += 1
		var paths: Dictionary = {}
		if cm.has_method("get_path_stats"):
			paths = cm.call("get_path_stats")
		print("PROBE state n=%-3d %-12s sim=%s enemy=%s angry=%d job=%d route=%d wait=%d "
			% [bodies.size(), scenario, str(sample.get("_sim") != null),
				str(sample.get("_enemy") != null), angry, int(sample.get("_job")),
				sample.get("_route").size(), int(sample.get("_chase_wait_ticks"))]
			+ "ticks=%d paths=%s" % [int(sample.get("_last_ticks")), str(paths)])


func _prepare(bodies: Array[Node3D], player: Node3D, scenario: String) -> void:
	for body in bodies:
		var node := body as Node3D
		if node == null:
			continue
		if scenario == "render":
			node.set_physics_process(false)
			continue
		node.set_physics_process(true)
		# No wander for the three scenarios that measure something other than the wander.
		node.set("_idle_ticks", SILENCED_IDLE)
		if scenario != "chase" and scenario != "moving chase":
			if node.has_method("_give_up"):
				node.call("_give_up")
			continue
		if node.has_method("take_anger"):
			node.call("take_anger", player)


func _run() -> void:
	DisplayServer.window_set_size(Vector2i(1280, 720))
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		print("PROBE FAIL: Main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	var cm := main.get_node_or_null("ChunkManager")
	if player == null or cm == null:
		print("PROBE FAIL: Main.tscn has no Player / ChunkManager")
		quit(1)
		return
	for child in main.get_children():
		if child is CanvasLayer:
			child.visible = false
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	# The baseline is a SETTLED world: a chunk install on the main thread is tens of milliseconds
	# of physics time and has nothing to do with the crowd.
	await _settle_world(cm, 90.0)
	await _seconds(1.0)

	# The session's trace, claimed and muted: a hundred bodies writing a line a second each would
	# be the recorder's cost, not the crowd's.
	var nav: GDScript = load(NAV_TRACE)
	if nav != null:
		nav.set("muted", true)
		nav.call("claim")

	var mesh: Mesh = load(MODEL_PATH)
	var body_script: Script = load(GOBLIN_SCRIPT)
	var model_script: Script = load(MODEL_SCRIPT)
	if mesh == null or body_script == null or model_script == null:
		print("PROBE FAIL: a goblin asset or script did not load")
		quit(1)
		return

	var feet := _surface(cm, player.global_position + Vector3(0.0, 0.0, -CROWD_DISTANCE))
	if feet == Vector3.INF:
		print("PROBE FAIL: no ground for the crowd's area")
		quit(1)
		return
	# The crowd's own LEVEL, which every stand has to be within STAND_SLACK of; the grid is laid
	# out from `origin` and each body then takes its own column's top (see _spawn_grid).
	var origin := feet

	for count in counts:
		for body in _goblins(main):
			if is_instance_valid(body):
				body.queue_free()
		await _seconds(0.5)
		var bodies := _spawn_grid(cm, main, origin, count, mesh, body_script, model_script)
		# Let them land, and let the chunks the grid woke finish installing: a body still falling
		# is the sim's, not the scenario's, and an install is the world's.
		await _seconds(1.5)
		await _settle_world(cm, 30.0)
		for scenario in SCENARIOS:
			_prepare(bodies, player, scenario)
			await _measure(cm, bodies, player, scenario,
				scenario == "moving chase" or scenario == "paths")
		for body in bodies:
			if is_instance_valid(body):
				body.queue_free()
	await _seconds(0.5)

	print("PROBE swarm done: %d count(s) x %d scenario(s); the table is above. A draw call that "
		% [counts.size(), SCENARIOS.size()]
		+ "moves with n is not the crowd's six (probes/probe_goblin_drawcalls.gd pins that flat).")
	quit(0 if ok else 1)
