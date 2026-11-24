extends Node
## The creator/debug tools, created on first use rather than at boot.
##
## They used to be autoloads so their keys worked from anywhere, but compiling
## their scripts at boot put ~40 ms in front of the title screen for three tools
## (`liquid_texture_lab.gd` alone is 29 ms of the launch). Each is instantiated
## here on first use: the one with a key is handed the same press, so the press
## that creates it also opens it, and the lab and the dummy are asked for by name
## from the Tools page and the dummy's spawn egg.
##
## Nothing outside this file names these three as autoloads: a probe that needs
## one instantiates its script directly (`probes/probe_lava_acid.gd` does that for
## the lab), which is why removing them from `project.godot` needs no call sites
## changed. `open_tool()` is the same creation without a key press behind it, for
## callers that have none: the Tools page's buttons and the dummy's spawn egg.
##
## `SkinManager` and `BlockManager` are deferred by time instead of by input. They
## own the player skin and the block-maker texture, and the title reads neither,
## so `BootScene` asks for them two frames after the title is on screen (and once
## more before it instantiates the world, in case the load beat the warm-up).
## Both are only ever read by path (`/root/SkinManager`, `/root/BlockManager`),
## so a node created later is found by every existing call site.

## Tool name -> script. Three are asked for by name with no key of their own (the
## Tools page's buttons, and the dummy egg for the pose clone); the last is reached
## by a key.
const TOOLS := {
	"liquid_lab": "res://scripts/liquid_texture_lab.gd",
	"pose_clone": "res://scripts/pose_clone_debug.gd",
	"goblin": "res://scripts/goblin_spawn.gd",
	"chunk_borders": "res://scripts/chunk_borders.gd",
}

## The one tool that only has a key (F3+B).
const KEY_TOOL := "chunk_borders"
const KEY_ACTION := "toggle_chunk_borders"

## G puts a goblin on the aimed block: the same thing the Tools page's button does,
## without the menu in the way. A key rather than the tool's own, because the tool
## is asked for by name from two places already and nothing else about it is an
## input handler.
const GOBLIN_TOOL := "goblin"
const GOBLIN_ACTION := "goblin_spawn"

## Autoloads the title does not need, created by `ensure_autoload()` before the
## world that reads them exists.
const LAZY_AUTOLOADS := {
	"SkinManager": "res://scripts/skin_manager.gd",
	"BlockManager": "res://scripts/block_manager.gd",
}

var _created: Dictionary = {}
## The player, hooked by `_hook_player`: a press in the world has to be told apart
## from a letter typed into the chat or a click on a menu.
var _player: Node = null


func _ready() -> void:
	# A held dummy egg right-clicks through PlayerController's `dummy_spawn`, and
	# the player arrives with the world: after this in the game, already there in a
	# probe, so both the tree and what is added to it are hooked.
	get_tree().node_added.connect(_hook_player)
	_hook_player(get_tree().root.find_child("Player", true, false))

func _hook_player(node: Node) -> void:
	if node == null or node.name != "Player" or not node.has_signal("dummy_spawn"):
		return
	_player = node
	if not node.is_connected("dummy_spawn", _spawn_dummy):
		node.connect("dummy_spawn", _spawn_dummy)

func _spawn_dummy() -> void:
	var tool := open_tool("pose_clone")
	if tool != null:
		tool.call("spawn")


func _unhandled_input(event: InputEvent) -> void:
	if event.is_echo():
		return
	if event.is_action_pressed(GOBLIN_ACTION):
		if _world_is_driven():
			var goblin := open_tool(GOBLIN_TOOL)
			if goblin != null:
				goblin.call("spawn")
				get_viewport().set_input_as_handled()
		return
	if not _created.has(KEY_TOOL) and event.is_action_pressed(KEY_ACTION):
		open_tool(KEY_TOOL, event)


## Whether a press belongs to the game rather than to a screen over it: the chat
## takes letters ("g" is a word) and a menu owns the mouse, so neither may drop a
## goblin into the world behind it. Anything else — a cursor the player has not
## captured yet included — is the world.
func _world_is_driven() -> bool:
	if _player == null or not is_instance_valid(_player):
		return true
	for method in ["is_chat_open", "is_settings_open", "is_inventory_open",
			"is_table_menu_open"]:
		if _player.has_method(method) and bool(_player.call(method)):
			return false
	return true


## Makes `tool_name` if it is not there yet and answers it; null if that name is not
## a tool. A keyed tool's press is handed over by hand, with the tool's input off
## until then, so one event cannot reach it twice and toggle it back shut; a tool
## with no key is asked for its `open()` (or nothing, if it has none). Both read
## their own input from the frame after this returns.
func open_tool(tool_name: String, event: InputEvent = null) -> Node:
	var tool: Node = _created.get(tool_name, null)
	var made := tool == null
	if made:
		if not TOOLS.has(tool_name):
			push_error("ToolLoader: %s is not a tool" % tool_name)
			return null
		var script := load(TOOLS[tool_name]) as Script
		if script == null:
			push_error("ToolLoader: %s did not load" % TOOLS[tool_name])
			return null
		tool = script.new() as Node
		if tool == null:
			push_error("ToolLoader: %s is not a Node" % TOOLS[tool_name])
			return null
		tool.set_process_unhandled_input(false)
		add_child(tool)
		_created[tool_name] = tool
		tool.set_process_unhandled_input.call_deferred(true)
	if event == null:
		if tool.has_method("open"):
			tool.call("open")
	elif made and tool.has_method("_unhandled_input"):
		tool.call("_unhandled_input", event)
	return tool


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
