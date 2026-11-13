extends Node
## The creator/debug tools, created on the first press of their own key.
##
## They used to be autoloads so their keys worked from anywhere, but compiling
## their scripts at boot put ~40 ms in front of the title screen for three tools
## that cannot be reached before a key is pressed (`liquid_texture_lab.gd` alone
## is 29 ms of the launch). Each is instantiated here on its first press and
## handed the same event, so the press that creates one also opens it; from then
## on the node is in the tree and reads its own input like any other autoload.
##
## Nothing outside this file names these three as autoloads: a probe that needs
## one instantiates its script directly (`probes/probe_lava_acid.gd` does that for
## the lab), which is why removing them from `project.godot` needs no call sites
## changed.
##
## `SkinManager` and `BlockManager` are deferred by time instead of by input. They
## own the player skin and the block-maker texture, and the title reads neither,
## so `BootScene` asks for them two frames after the title is on screen (and once
## more before it instantiates the world, in case the load beat the warm-up).
## Both are only ever read by path (`/root/SkinManager`, `/root/BlockManager`),
## so a node created later is found by every existing call site.

const TOOLS := {
	"liquid_lab_toggle": "res://scripts/liquid_texture_lab.gd",
	"toggle_chunk_borders": "res://scripts/chunk_borders.gd",
	"pose_clone_toggle": "res://scripts/pose_clone_debug.gd",
}

## Autoloads the title does not need, created by `ensure_autoload()` before the
## world that reads them exists.
const LAZY_AUTOLOADS := {
	"SkinManager": "res://scripts/skin_manager.gd",
	"BlockManager": "res://scripts/block_manager.gd",
}

var _created: Dictionary = {}


func _unhandled_input(event: InputEvent) -> void:
	if event.is_echo():
		return
	for action in TOOLS:
		if _created.has(action) or not event.is_action_pressed(action):
			continue
		_spawn(action, event)
		return


func _spawn(action: String, event: InputEvent) -> void:
	var script := load(TOOLS[action]) as Script
	if script == null:
		push_error("ToolLoader: %s did not load" % TOOLS[action])
		return
	var tool := script.new() as Node
	if tool == null:
		push_error("ToolLoader: %s is not a Node" % TOOLS[action])
		return
	# Its input is off until this event has been handed over by hand, so the
	# same press cannot be delivered to it twice and toggle it back shut.
	tool.set_process_unhandled_input(false)
	add_child(tool)
	_created[action] = tool
	if tool.has_method("_unhandled_input"):
		tool.call("_unhandled_input", event)
	tool.set_process_unhandled_input.call_deferred(true)


## Creates a deferred autoload under `/root` if it is not there yet, and answers
## the node either way. Called from `BootScene`; a second call is a lookup.
func ensure_autoload(autoload_name: String) -> Node:
	var root := get_tree().root
	var existing := root.get_node_or_null(NodePath(autoload_name))
	if existing != null:
		return existing
	if not LAZY_AUTOLOADS.has(autoload_name):
		push_error("ToolLoader: %s is not a deferred autoload" % autoload_name)
		return null
	var script := load(LAZY_AUTOLOADS[autoload_name]) as Script
	if script == null:
		push_error("ToolLoader: %s did not load" % LAZY_AUTOLOADS[autoload_name])
		return null
	var node := script.new() as Node
	if node == null:
		push_error("ToolLoader: %s is not a Node" % LAZY_AUTOLOADS[autoload_name])
		return null
	node.name = autoload_name
	root.add_child(node)
	return node


func ensure_lazy_autoloads() -> void:
	for autoload_name in LAZY_AUTOLOADS:
		ensure_autoload(autoload_name)
