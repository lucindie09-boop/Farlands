extends SceneTree
## Headless check of the nav trace's PLAYER track: every step the player takes is written out with
## the 3x3 of column heights around his feet.
##
## The line exists because the goblin's traces could not settle the corner: the planner's answers and
## the walk's drops say what the mob tried, and nothing said what the PLAYER did across the same
## blocks. His feet, his step and the wall/hole/step beside him, at that scale, are the answer. So
## what is under test is the map's own arithmetic — which cell is `0` (the floor under the feet),
## which is a step `1`, a wall `2`, a hole `-1`, nothing `X` — and that a line is written when he
## moves and not when he stands still.
##
##   probes/probe_trace_player.gd  (Godot --headless --path <project> --script res://probes/probe_trace_player.gd)

const TRACE_SCRIPT := "res://scripts/nav_trace.gd"
## A world with no chunks in it: the cells a probe places, and air everywhere else. `get_block` is
## the one call the map makes.
const WORLD_SCRIPT := """extends Node
var cells := {}
func get_block(x: int, y: int, z: int) -> int:
	return 1 if cells.has(Vector3i(x, y, z)) else 0
func get_texture_layer_info(_block) -> Dictionary:
	return {}
"""

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _run() -> void:
	# The trace looks for the world at `/root/Main/ChunkManager` and for the player at
	# `current_scene/Player`, which is what it finds in the game.
	var main := Node.new()
	main.name = "Main"
	root.add_child(main)
	var world := Node.new()
	world.name = "ChunkManager"
	world.set_script(_script(WORLD_SCRIPT))
	main.add_child(world)
	var stage := Node3D.new()
	stage.name = "Scene"
	root.add_child(stage)
	var player := Node3D.new()
	player.name = "Player"
	stage.add_child(player)
	current_scene = stage
	# The map, in cells: the floor under the feet (0), a step one up beside it (1), a two-block wall
	# on the other side (2), and nothing at all behind him (X).
	world.set("cells", {
		Vector3i(0, 63, 0): true, Vector3i(0, 63, 1): true,
		Vector3i(-1, 63, 0): true, Vector3i(-1, 64, 0): true, Vector3i(-1, 63, 1): true,
		Vector3i(1, 63, 0): true, Vector3i(1, 64, 0): true, Vector3i(1, 65, 0): true,
		Vector3i(1, 63, 1): true,
	})
	player.position = Vector3(0.5, 64.0, 0.5)
	await process_frame

	var trace: Node = load(TRACE_SCRIPT).new()
	trace.name = "NavTrace"
	root.add_child(trace)
	# Its own 1 Hz sampling is not under test, and a node that samples would write on top of this.
	trace.set_process(false)
	# `user://nav_trace.txt` is the live session's file; a check of the lines must not write over it.
	trace.get_script().set("muted", true)
	trace.call("_track_player")
	var lines: Array = trace.get("_player_lines")
	print("PROBE first: %s" % (str(lines[lines.size() - 1]) if not lines.is_empty() else "nothing"))
	if lines.is_empty():
		_fail("standing still on the first frame wrote no line at all")
	else:
		var first: String = str(lines[0])
		if not first.contains("step=first"):
			_fail("the first line is not the first step (%s)" % first)
		if not first.contains("3x3=[X,X,X / 1,0,2 / 0,0,0]"):
			_fail("the map is not the fixture [X,X,X / 1,0,2 / 0,0,0] (%s)" % first)

	# Standing still writes nothing more: the reader wants steps, not a heartbeat.
	var before := lines.size()
	trace.call("_track_player")
	print("PROBE still: %d line(s) before, %d after standing still" % [before, (trace.get("_player_lines") as Array).size()])
	if (trace.get("_player_lines") as Array).size() != before:
		_fail("a player standing still wrote another line")

	# A fifth of a block is a step, and the step since the last line is in it.
	player.position = Vector3(0.75, 64.0, 0.5)
	trace.call("_track_player")
	lines = trace.get("_player_lines")
	var last: String = str(lines[lines.size() - 1])
	print("PROBE step: %s" % last)
	if lines.size() != before + 1:
		_fail("a quarter of a block wrote %d lines" % (lines.size() - before))
	elif not last.contains("step=(0.25, 0.00, 0.00)"):
		_fail("the step since the last line is not in it (%s)" % last)

	trace.queue_free()
	stage.queue_free()
	main.queue_free()
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)


func _script(text: String) -> GDScript:
	var script := GDScript.new()
	script.source_code = text
	script.reload()
	return script
