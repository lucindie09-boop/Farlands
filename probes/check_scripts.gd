extends SceneTree
## Compiles every .gd in the project and prints what the engine says about it.
##
## The GDScript warnings the CI log carries (shadowed properties, ternaries whose
## arms disagree) are emitted when a script is *compiled*, which is when it is
## loaded - so loading each one is the whole check. Nothing is instantiated: no
## scene, no window, no user:// writes, so this is safe to run beside the game,
## unlike the probes.
##
##   "$GODOT" --headless --path . --script res://probes/check_scripts.gd
##
## Warnings go to the engine's own log, so run it through grep for WARNING.

func _initialize() -> void:
	var files: Array = []
	_walk("res://", files)
	files.sort()
	var failed := 0
	for path in files:
		var script: Resource = load(path)
		if script == null:
			print("check: FAILED to load %s" % path)
			failed += 1
	print("check: %d script(s), %d failed to load" % [files.size(), failed])
	quit(1 if failed > 0 else 0)


func _walk(dir_path: String, out: Array) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		if entry.begins_with("."):
			entry = dir.get_next()
			continue
		var path := dir_path.path_join(entry)
		if dir.current_is_dir():
			_walk(path + "/", out)
		elif entry.ends_with(".gd"):
			out.append(path)
		entry = dir.get_next()
	dir.list_dir_end()
