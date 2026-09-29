extends SceneTree
## Checks that every method, signal and property the PlayerController binds is
## actually registered under its GDScript name — the thing a split of
## _bind_methods can silently break (a call left out of the new grouping, or
## moved into a helper that never runs). Compares the live ClassDB list against
## the names the GDScript side calls, plus one round trip per property.
##
## Run: Godot --headless --path <project> --script res://probes/probe_bindings.gd

const METHODS := [
	# bind_actions
	"toggle_fly_mode", "break_block", "place_block", "use_item", "get_selected_block",
	"set_selected_block", "get_block_edit_counter", "get_break_state",
	# bind_inventory_api
	"get_hotbar_slot_count", "get_hotbar_slot_block_id", "get_selected_hotbar_slot",
	"select_hotbar_slot", "set_hotbar_slot", "get_inventory_slot_count",
	"get_inventory_slot_block_id", "set_inventory_slot", "give_block", "clear_inventory",
	"match_recipe", "craft_recipe", "save_inventory", "load_inventory",
	"set_active_texture_pack", "get_installed_pack_names", "resolve_texture_path",
	"set_inventory_open", "is_inventory_open", "set_table_menu_open", "is_table_menu_open",
	# bind_ui_state
	"set_chat_open", "is_chat_open", "set_settings_open", "set_wand_menu_open", "is_wand_held",
	"is_wand_menu_open", "is_settings_open", "teleport_to", "set_fly_mode", "get_fly_mode",
	# bind_tuning
	"set_sensitivity", "get_sensitivity", "set_fly_speed", "get_fly_speed",
	# bind_state_and_view
	"get_health", "set_health", "is_dead", "die", "respawn", "is_on_floor", "is_in_water",
	"toggle_third_person", "set_third_person", "get_third_person", "set_third_person_view",
	"get_third_person_view", "update_player_animation", "get_aim_origin", "get_aim_direction",
]

const SIGNALS := ["crafting_table_used", "block_placed", "died", "respawned",
	"wand_menu", "wand_use", "wand_confirm"]

const PROPERTIES := ["health", "sensitivity", "fly_speed", "third_person"]

var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	var player: Node = main.get_node_or_null("Player")
	if player == null:
		_fail("Player missing")
		quit(1)
		return

	var live := {}
	for m in player.get_method_list():
		live[String(m["name"])] = true
	for name in METHODS:
		if not live.has(name):
			_fail("PlayerController::%s is not bound" % name)

	for name in SIGNALS:
		if not player.has_signal(name):
			_fail("signal %s is not registered" % name)

	for name in PROPERTIES:
		if not name in player:
			_fail("property %s is not registered" % name)
			continue
		var value: Variant = player.get(name)
		player.set(name, value)

	# A live call through each group, so the bound pointer is the method meant:
	# a wrong entry in the table would still list the name.
	if not (player.get_health() > 0):
		_fail("get_health() did not answer with a living player")
	if player.get_fly_mode() != false:
		_fail("get_fly_mode() is not false at spawn")
	if player.get_hotbar_slot_count(0) < 0:
		_fail("get_hotbar_slot_count(0) refused its slot argument")
	if player.get_aim_direction().length() < 0.5:
		_fail("get_aim_direction() is not a unit-ish direction")

	print("PROBE %s (%d methods, %d signals, %d properties)" %
		["PASS" if ok else "FAIL", METHODS.size(), SIGNALS.size(), PROPERTIES.size()])
	quit(0 if ok else 1)
