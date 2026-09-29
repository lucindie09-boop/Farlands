extends SceneTree
## Smoke test for the /genstats command: that the handler runs at all (it reaches
## the engine through a local node lookup, so a missing node would be a runtime
## error rather than a parse error), that its arithmetic survives a session with
## no checks yet, and that the `reset` form zeroes the counters while keeping the
## candidate-list size.
##
##   .freebuff/run_probe.sh .freebuff/probe_genstats_cmd.gd 200

var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	var main: Node = scene.instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	var chat: Node = main.get_node_or_null("UI/Chat")
	if cm == null:
		_fail("ChunkManager missing")
		quit(1)
		return
	if chat == null:
		# The HUD node may be elsewhere; find it by script instead of guessing.
		for child in main.find_children("*", "CanvasLayer", true, false):
			if child.has_method("_run_command"):
				chat = child
				break
	if chat == null:
		for child in main.find_children("*", "", true, false):
			if child.has_method("_run_command"):
				chat = child
				break
	if chat == null:
		_fail("no node with _run_command found")
		quit(1)
		return
	print("PROBE found chat node: %s" % chat.name)

	# Small render distance so the probe itself stays quick; the command's output
	# does not depend on it.
	cm.set_render_distance(4)
	var frames := 0
	while frames < 150:
		await process_frame
		frames += 1

	var before: Dictionary = cm.get_generation_stats()
	print("PROBE before: frames=%d checks=%d gens=%d list=%d" % [
		int(before.get("frames", 0)), int(before.get("checks", 0)),
		int(before.get("generations", 0)), int(before.get("candidate_offsets", 0))])

	# The command itself, both forms. Any error inside (a bad Dictionary cast, a
	# missing key, a format mismatch) surfaces as a script error in the run log.
	chat.call("_run_command", "/genstats")
	chat.call("_run_command", "/genstats reset")

	var after: Dictionary = cm.get_generation_stats()
	print("PROBE after reset: frames=%d checks=%d gens=%d list=%d" % [
		int(after.get("frames", 0)), int(after.get("checks", 0)),
		int(after.get("generations", 0)), int(after.get("candidate_offsets", 0))])

	if int(after.get("checks", -1)) != 0:
		_fail("checks did not reset to 0")
	if int(after.get("frames", -1)) != 0:
		_fail("frames did not reset to 0")
	if int(before.get("candidate_offsets", 0)) > 0 and int(after.get("candidate_offsets", 0)) == 0:
		_fail("candidate_offsets was lost by the reset")

	# And again straight after a reset, with a near-empty window: the percentage
	# helpers must not divide by zero.
	chat.call("_run_command", "/genstats")
	for i in range(5):
		await process_frame
	chat.call("_run_command", "/genstats")

	print("PROBE %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)
