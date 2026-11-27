extends SceneTree
## Headless check that `data/mobs.json` reaches a real body.
##
## The parse is pinned by `tests/test_mob_attributes.cpp`, which cannot show the half that
## matters to a player: that a walker spawned in the world runs with the numbers the file states
## instead of the constants it was written against. So this reads the registry, spawns a goblin,
## and holds the body to the file -- then writes a different mob to `user://`, loads THAT, and
## checks a second body comes up with the new numbers. A body reading its own constants would
## pass the first half and fail the second.
##
##   probes/probe_mob_attributes.gd  (Godot --headless --path <project> --script res://probes/probe_mob_attributes.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
const SILENCED_IDLE := 1000000
## A goblin that is deliberately nothing like the shipped one, so "the body read the file" and
## "the body kept its constants" cannot both be true.
const CHANGED := {
	"health": 30.0,
	"damage": 5.0,
	"speed": 0.5,
	"follow_range": 12.0,
	"alert_box": 4.0,
	"anger_window": [10, 20],
	"step_height": 1.05,
	"safe_fall_distance": 6.0,
	"fall_damage_multiplier": 0.5,
	"armor": 8.0,
	"knockback_resistance": 0.5,
}

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _spawn(main: Node) -> Node3D:
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	body.add_to_group("goblin")
	main.add_child(body)
	body.set("_idle_ticks", SILENCED_IDLE)
	return body


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	if player == null:
		print("PROBE FAIL: the world has no Player")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break

	# --- The shipped file ------------------------------------------------------
	# `Mobs.load` runs with the world's configs, so an empty answer here is that load never
	# having happened rather than a probe mistake.
	var shipped: Dictionary = Mobs.attributes("goblin")
	print("PROBE shipped goblin: %s" % str(shipped))
	if shipped.is_empty():
		print("PROBE FAIL: no goblin in the registry (mobs.json never loaded)")
		quit(1)
		return
	for field in ["health", "damage", "speed", "follow_range", "alert_box", "anger_window",
			"step_height", "safe_fall_distance", "fall_damage_multiplier", "armor",
			"knockback_resistance"]:
		if not shipped.has(field):
			_fail("the goblin entry is missing \"%s\"" % field)
	var window: Array = shipped.get("anger_window", [])
	if window.size() != 2 or int(window[0]) > int(window[1]):
		_fail("anger_window is not a [min, max] pair: %s" % str(window))

	var body := _spawn(main)
	for i in 4:
		await process_frame
	var health := float(body.call("get_max_health"))
	print("PROBE body spawned with %s of %s max health" % [str(health), str(shipped["health"])])
	if absf(health - float(shipped["health"])) > 0.001:
		_fail("the body's max health is %.2f, the file says %.2f" % [health, float(shipped["health"])])
	var follow := float(body.get("_follow_range"))
	if absf(follow - float(shipped["follow_range"])) > 0.001:
		_fail("the body follows to %.2f blocks, the file says %.2f" % [follow, float(shipped["follow_range"])])
	var alert := float(body.get("_alert_box"))
	if absf(alert - float(shipped["alert_box"])) > 0.001:
		_fail("the body's alert box is %.2f blocks, the file says %.2f" % [alert, float(shipped["alert_box"])])
	var damage := float(body.get("_damage"))
	if absf(damage - float(shipped["damage"])) > 0.001:
		_fail("the body's blow costs %.2f, the file says %.2f" % [damage, float(shipped["damage"])])
	if int(body.get("_anger_min_ticks")) != int(window[0]) \
			or int(body.get("_anger_max_ticks")) != int(window[1]):
		_fail("the body's anger window is %s, the file says %s"
			% [str([body.get("_anger_min_ticks"), body.get("_anger_max_ticks")]), str(window)])

	# The five numbers the SIM holds: the file's value reaching the script's own field and the
	# value the body actually moves and hurts with are two different claims, and the first can be
	# true while the second is not. So this reads them off the body's sim.
	var shipped_sim: RefCounted = body.get("_sim")
	if shipped_sim == null or not shipped_sim.has_method("get_step_height"):
		_fail("the shipped body's sim does not take a mob's own numbers")
	else:
		for pair in [["get_step_height", "step_height"],
				["get_safe_fall_distance", "safe_fall_distance"],
				["get_fall_damage_multiplier", "fall_damage_multiplier"],
				["get_knockback_resistance", "knockback_resistance"]]:
			var got := float(shipped_sim.call(pair[0]))
			if absf(got - float(shipped[pair[1]])) > 0.001:
				_fail("the shipped body's %s is %.3f, the file says %.3f"
					% [pair[1], got, float(shipped[pair[1]])])
		# A bare goblin's armor costs nothing: two half-hearts arrive as two.
		var bare := float(shipped_sim.mitigate(2.0))
		print("PROBE shipped body: 2 half-hearts costs %.3f with no armor" % bare)
		if absf(bare - 2.0) > 0.0001:
			_fail("a goblin with no armor softened a two-half-heart hit to %.3f" % bare)

	# --- A different file, and a second body -----------------------------------
	# The whole point of the file: a number changed in the data is a number the game runs
	# with. Written to user:// rather than over the shipped file, which a probe has no
	# business editing.
	var text := "{\"mobs\": {\"goblin\": %s}}" % JSON.stringify(CHANGED)
	var file := FileAccess.open("user://probe_mobs.json", FileAccess.WRITE)
	if file == null:
		print("PROBE FAIL: cannot write user://probe_mobs.json")
		quit(1)
		return
	file.store_string(text)
	file.close()
	if not Mobs.load("user://probe_mobs.json"):
		print("PROBE FAIL: the probe's own file did not load")
		quit(1)
		return
	var changed: Dictionary = Mobs.attributes("goblin")
	print("PROBE reloaded goblin: %s" % str(changed))
	var second := _spawn(main)
	for i in 4:
		await process_frame
	var second_health := float(second.call("get_max_health"))
	print("PROBE second body max health: %s (file says %s)"
		% [str(second_health), str(changed.get("health", 0))])
	if absf(second_health - float(CHANGED["health"])) > 0.001:
		_fail("a body spawned after a reloaded file still has %.2f max health" % second_health)
	if absf(float(second.get("_follow_range")) - float(CHANGED["follow_range"])) > 0.001:
		_fail("a body spawned after a reloaded file still follows to %s"
			% str(second.get("_follow_range")))
	if absf(float(second.get("_alert_box")) - float(CHANGED["alert_box"])) > 0.001:
		_fail("a body spawned after a reloaded file still shouts to %s"
			% str(second.get("_alert_box")))

	# ...and the sim's own half of the reloaded file: a different step height, fall numbers and
	# knockback resistance, and armor that actually softens what a hit costs.
	var moved: RefCounted = second.get("_sim")
	if moved == null or not moved.has_method("get_step_height"):
		_fail("the second body's sim does not take a mob's own numbers")
	else:
		for pair in [["get_step_height", "step_height"],
				["get_safe_fall_distance", "safe_fall_distance"],
				["get_fall_damage_multiplier", "fall_damage_multiplier"],
				["get_knockback_resistance", "knockback_resistance"]]:
			var got := float(moved.call(pair[0]))
			if absf(got - float(CHANGED[pair[1]])) > 0.001:
				_fail("a body spawned after a reloaded file has %s %.3f, the file says %.3f"
					% [pair[1], got, float(CHANGED[pair[1]])])
		var softened := float(moved.mitigate(4.0))
		print("PROBE second body: 4 half-hearts costs %.3f after 8 armor points" % softened)
		if softened >= 3.0:
			_fail("eight armor points did not soften a four-half-heart hit (%.3f)" % softened)
		if absf(float(moved.mitigate(0.0))) > 0.0001:
			_fail("a zero hit costs %.4f after armor" % float(moved.mitigate(0.0)))

	# The shipped file goes back in force, so nothing after this probe runs on the probe's
	# goblin.
	if not Mobs.load("res://data/mobs.json"):
		_fail("the shipped mobs.json did not load back")
	DirAccess.remove_absolute("user://probe_mobs.json")

	second.queue_free()
	body.queue_free()
	for i in 3:
		await process_frame
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
