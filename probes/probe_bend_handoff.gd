extends SceneTree
## Does the overlay find the engine, and does it hand the bend over?
##
## probe_bend_cull.gd answers this end to end (the frame draws more geometry once
## the engine is told), but that costs a whole world to stream first. This is the
## same question asked cheaply: no world, no settling, just the wiring. It exists
## because the hand-off was an invisible failure - the overlay asked for a node at
## a fixed path, the node was not there, and the only symptom was that the culling
## never got better.
##
##   Godot --path . --script res://probes/probe_bend_handoff.gd
##
## The engine is found by asking rather than by path, so what this checks is that
## the search works from wherever the overlay happens to sit in the scene.

const OVERLAY_PATH := "HUD/ShaderOverlay"
const BEND_ID := "bend"

var failures := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		_fail("Main.tscn missing")
		_finish()
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	# The overlay's push is deferred, and `_ready` of its siblings may still be
	# running on the first frame.
	for i in range(4):
		await process_frame

	var overlay: Node = main.get_node_or_null(OVERLAY_PATH)
	var cm: Node = main.get_node_or_null("ChunkManager")
	if overlay == null or cm == null:
		_fail("%s / ChunkManager missing" % OVERLAY_PATH)
		_finish()
		return

	# The overlay's own view of it, read straight out of the script. This is the
	# number that was null in the bug: null meant every push returned early.
	var world: Node = overlay.get("_world")
	print("probe: the overlay resolved the engine as %s" % world)
	if world == null:
		_fail("the overlay's hand-off found no engine: the bend would reach the material and not the culling")
	elif world != cm:
		_fail("the overlay's hand-off found %s, which is not the scene's ChunkManager" % world)

	# And it has to be the one the bend is pushed onto, with the bend's own keys.
	if not cm.has_method("set_world_bend"):
		_fail("ChunkManager has no set_world_bend: the engine build is stale")

	# A world definition is the only thing this pushes for, so it has to be the
	# registry's, and its params have to be the uniform names the engine reads.
	var world_ids: Array = []
	for definition in overlay.call("get_definitions"):
		if String(definition.get("kind", "screen")) == "world":
			world_ids.append(String(definition.get("id", "")))
	print("probe: the registry's world effects are %s" % [world_ids])
	if not world_ids.has(BEND_ID):
		_fail("the registry has no world effect called %s" % BEND_ID)
	for key in ["world_bend", "world_bend_radius", "world_bend_rise"]:
		if overlay.call("get_value", BEND_ID, key) == null:
			_fail("the %s definition has no %s" % [BEND_ID, key])

	# A switch has to change the overlay's answer, which is what the engine is
	# told: this is the number the menu's own rows end up pushing.
	for row in [[true, 1.0], [false, 0.0]]:
		overlay.call("set_enabled", BEND_ID, bool(row[0]))
		for i in range(3):
			await process_frame
		var seen: bool = bool(overlay.call("is_enabled", BEND_ID))
		print("probe: switch pushed %s, the overlay says %s, the engine is at %s"
			% [row[0], seen, world])
		if seen != bool(row[0]):
			_fail("the switch pushed %s and the overlay says %s" % [row[0], seen])

	_finish()


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _finish() -> void:
	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)
