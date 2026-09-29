extends SceneTree
## Can a strip saved in an earlier session be loaded in this one?
##
## The lab's Load picker is filled from user://liquids, a directory that outlives
## the process. This writes a settings file there *before* the panel is ever
## opened, then opens it and checks the picker already lists it â€” the case that
## used to come up empty until something was saved or deleted in the session.
##
##   Godot --headless --path <project> --script res://.freebuff/probe_lab_load.gd

const SAVE_DIR := "user://liquids"
const NAME := "probe_lab_load"
const RESULT_FILE := "user://probe_lab_load_result.txt"

var failures: Array[String] = []
var checks := 0
var ok := true

func _check(condition: bool, msg: String) -> void:
	checks += 1
	if not condition:
		ok = false
		failures.append(msg)
		print("PROBE FAIL: " + msg)

func _initialize() -> void:
	DirAccess.make_dir_recursive_absolute(SAVE_DIR)
	# A settings file left by an earlier session, written before the lab exists.
	var settings: Dictionary = LiquidTextureGen.default_settings("lava", 16)
	settings["frames"] = 7
	settings["field_scale"] = 1.37
	var file := FileAccess.open(SAVE_DIR + "/" + NAME + ".json", FileAccess.WRITE)
	file.store_string(JSON.stringify(settings))
	file.close()

	var lab: Node = load("res://scripts/liquid_texture_lab.gd").new()
	root.add_child(lab)
	await process_frame
	_check(lab.get("_load_pick") == null, "the panel is built lazily, so there is no picker yet")

	# First open of the session.
	lab.call("_show_lab")
	await process_frame
	var picker: OptionButton = lab.get("_load_pick")
	_check(picker != null, "the panel built its picker")
	var found := -1
	for index in picker.item_count:
		if picker.get_item_text(index) == NAME:
			found = index
	print("PROBE picker has %d entries, %s %s" % [
		picker.item_count, NAME, "found at %d" % found if found >= 0 else "MISSING"])
	_check(found >= 0, "a file saved in an earlier session is listed on the first open")

	# Loading it restores the values that are in the file, not the defaults.
	if found >= 0:
		picker.selected = found
		lab.call("_load_selected")
		await process_frame
		var loaded: Dictionary = lab.get("settings")
		_check(int(loaded.get("frames", 0)) == 7, "load restored frames (%d)" % int(loaded.get("frames", 0)))
		_check(absf(float(loaded.get("field_scale", 0.0)) - 1.37) < 0.0001, "load restored field_scale")
		_check(String(loaded.get("style", "")) == "lava", "load restored the style")

	# And the name box now points at the file, so a later open selects it rather
	# than whatever happens to be first in the directory.
	lab.call("_refresh_load_list", NAME)
	_check(picker.item_count > 0 and picker.get_item_text(picker.selected) == NAME,
		"the picker lands on the strip the name box shows")

	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + NAME + ".json"))
	var verdict := ("PASS (%d checks)" % checks) if ok else ("FAIL (%d/%d)\n" % [failures.size(), checks] + "\n".join(failures))
	var out := FileAccess.open(RESULT_FILE, FileAccess.WRITE)
	if out != null:
		out.store_string(verdict)
		out.close()
	print("PROBE " + verdict.split("\n")[0])
	quit(0 if ok else 1)
