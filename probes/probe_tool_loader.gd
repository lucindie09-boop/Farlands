extends SceneTree
## The debug tools are no longer autoloads: `ToolLoader` creates each on the first
## press of its own key. This presses all three the way the engine would and
## checks the node exists, is the script it should be, and that the same press
## reached it (chunk borders toggles itself on when the mouse is captured).
##
##   probes/probe_tool_loader.gd

const TOOLS := {
	"toggle_chunk_borders": "res://scripts/chunk_borders.gd",
	"liquid_lab_toggle": "res://scripts/liquid_texture_lab.gd",
	"pose_clone_toggle": "res://scripts/pose_clone_debug.gd",
}

var ok := true

func _initialize() -> void:
	_run()

func _check(condition: bool, message: String) -> void:
	if not condition:
		ok = false
	print("PROBE %s: %s" % ["ok" if condition else "FAIL", message])

func _run() -> void:
	var loader := root.get_node_or_null("ToolLoader")
	_check(loader != null, "ToolLoader autoload is present")
	if loader == null:
		quit(1)
		return
	# The tools are gameplay keys: they ignore a press while the mouse is free.
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	for action in TOOLS:
		var before: Dictionary = loader.get("_created")
		_check(not before.has(action), "%s is not created before its key" % action)
		var event := InputEventAction.new()
		event.action = action
		event.pressed = true
		Input.parse_input_event(event)
		await process_frame
		await process_frame
		var created: Dictionary = loader.get("_created")
		_check(created.has(action), "%s spawned a node on its key" % action)
		if created.has(action):
			var tool: Node = created[action]
			_check(is_instance_valid(tool) and tool.is_inside_tree(),
				"%s's node is in the tree" % action)
			_check(tool.get_script() != null and tool.get_script().resource_path == TOOLS[action],
				"%s's node carries the right script" % action)
		# A second press must not spawn a second node.
		Input.parse_input_event(event)
		await process_frame
		var after: Dictionary = loader.get("_created")
		_check(after.size() == created.size(), "%s does not spawn twice" % action)
	_check(loader.get_child_count() == TOOLS.size(),
		"one node per tool under ToolLoader (%d)" % loader.get_child_count())
	quit(0 if ok else 1)
