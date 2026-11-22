extends SceneTree
## The debug tools are no longer autoloads: `ToolLoader` makes each on first use.
## The chunk-border overlay is the one with a key left (`toggle_chunk_borders`,
## pressed here the way the engine would); the lab and the pose clone are asked for
## by name, which is what the Tools page's button and the dummy's spawn egg do.
## Each must make exactly one node, in the tree, carrying its own script, and a
## second ask must answer that node rather than make another.
##
##   probes/probe_tool_loader.gd

const BY_NAME := {
	"liquid_lab": "res://scripts/liquid_texture_lab.gd",
	"pose_clone": "res://scripts/pose_clone_debug.gd",
}
const KEYED := "chunk_borders"
const KEYED_SCRIPT := "res://scripts/chunk_borders.gd"
const KEY_ACTION := "toggle_chunk_borders"

var ok := true

func _initialize() -> void:
	_run()

func _check(condition: bool, message: String) -> void:
	if not condition:
		ok = false
	print("PROBE %s: %s" % ["ok" if condition else "FAIL", message])

func _right(tool: Node, path: String, label: String) -> void:
	_check(is_instance_valid(tool) and tool.is_inside_tree(), "%s's node is in the tree" % label)
	_check(tool.get_script() != null and tool.get_script().resource_path == path,
		"%s's node carries the right script" % label)

func _run() -> void:
	var loader := root.get_node_or_null("ToolLoader")
	_check(loader != null, "ToolLoader autoload is present")
	if loader == null:
		quit(1)
		return

	for tool_name in BY_NAME:
		_check(not (loader.get("_created") as Dictionary).has(tool_name),
			"%s is not created before it is asked for" % tool_name)
		loader.call("open_tool", tool_name)
		await process_frame
		await process_frame
		var created: Dictionary = loader.get("_created")
		_check(created.has(tool_name), "%s was made by open_tool" % tool_name)
		if created.has(tool_name):
			_right(created[tool_name], BY_NAME[tool_name], tool_name)
		_check(loader.call("open_tool", tool_name) == created.get(tool_name),
			"%s answers the same node twice, and makes no second one" % tool_name)

	# The keyed tool: the engine's own press, which the loader hands over.
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	_check(not (loader.get("_created") as Dictionary).has(KEYED),
		"%s is not created before its key" % KEYED)
	var event := InputEventAction.new()
	event.action = KEY_ACTION
	event.pressed = true
	Input.parse_input_event(event)
	await process_frame
	await process_frame
	var after_key: Dictionary = loader.get("_created")
	_check(after_key.has(KEYED), "%s spawned a node on its key" % KEYED)
	if after_key.has(KEYED):
		_right(after_key[KEYED], KEYED_SCRIPT, KEYED)
	Input.parse_input_event(event)
	await process_frame
	_check((loader.get("_created") as Dictionary).size() == after_key.size(),
		"%s does not spawn twice" % KEYED)

	_check(loader.get_child_count() == BY_NAME.size() + 1,
		"one node per tool under ToolLoader (%d)" % loader.get_child_count())
	quit(0 if ok else 1)
