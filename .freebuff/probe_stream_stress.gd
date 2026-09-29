extends SceneTree
## The player's own regression test, made repeatable: fly along a line at
## increasing speed and let the chunk/LOD streaming work for its living.
##
## The player's manual sweep (/fly 250 -> 100000) is what used to bring the
## crash out, so this drives the same thing headlessly: fly mode on, the speed
## the run is at, and a teleport along +X every frame so the streamer has to
## load and unload around a moving point constantly. It reads the world and
## restores the position before quitting, and a crash lands in FARLANDS_CRASH_DIR
## like any other.
##
##   FARLANDS_CRASH_DIR=<dir> .freebuff/run_probe.sh .freebuff/probe_stream_stress.gd

const SPEEDS := [250.0, 1000.0, 10000.0, 100000.0]
const FRAMES_PER_SPEED := 90

func _initialize() -> void:
	_run()

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 30:
		await process_frame

	var player: Node3D = main.get_node_or_null("Player")
	if player == null:
		print("PROBE FAIL: no Player node")
		quit(1)
		return
	var supported: Array = []
	for method in ["set_fly_mode", "set_fly_speed"]:
		if player.has_method(method):
			supported.append(method)
	print("PROBE fly bindings: %s" % [supported])
	if player.has_method("set_fly_mode"):
		player.set_fly_mode(true)

	var home: Vector3 = player.global_position
	var travelled := 0.0
	for speed in SPEEDS:
		if player.has_method("set_fly_speed"):
			player.set_fly_speed(speed)
		var step: float = max(1.0, speed * 0.1)
		for i in FRAMES_PER_SPEED:
			travelled += step
			player.global_position = home + Vector3(travelled, 120.0, 0.0)
			await process_frame
		print("PROBE speed %d ok (%.0f blocks out)" % [int(speed), travelled])

	player.global_position = home
	await process_frame
	print("PROBE stream stress survived %d speeds, %.0f blocks" % [SPEEDS.size(), travelled])
	quit(0)
