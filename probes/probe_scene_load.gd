extends SceneTree
## What the ~0.6 s `Main.tscn` load is made of: every `ext_resource` in the
## scene, loaded and timed on its own, biggest first. Scripts already loaded by
## an autoload read as ~0 here; a cold GUI boot loads them all.
##
##   probes/probe_scene_load.gd

func _initialize() -> void:
	_run()

func _run() -> void:
	var text := FileAccess.get_file_as_string("res://Main.tscn")
	if text.is_empty():
		print("PROBE scene FAIL: Main.tscn missing")
		quit(1)
		return
	var paths: Array[String] = []
	for line in text.split("\n"):
		if not line.begins_with("[ext_resource"):
			continue
		var at := line.find("path=\"")
		if at < 0:
			continue
		var rest := line.substr(at + 6)
		var path := rest.substr(0, rest.find("\""))
		if not paths.has(path):
			paths.append(path)
	var start := Time.get_ticks_usec()
	var rows: Array = []
	for path in paths:
		var t := Time.get_ticks_usec()
		var resource: Resource = load(path)
		rows.append([Time.get_ticks_usec() - t, path, resource != null])
	var total := Time.get_ticks_usec() - start
	rows.sort_custom(func(a: Array, b: Array) -> bool: return a[0] > b[0])
	for row in rows:
		if row[0] >= 500:
			print("PROBE scene part ms=%.1f path=%s ok=%s" % [
				float(row[0]) / 1000.0, row[1], row[2]])
	var cold := 0
	for row in rows:
		if row[0] >= 500:
			cold += 1
	print("PROBE scene summary total_ms=%.1f parts=%d over_0.5ms=%d" % [
		float(total) / 1000.0, paths.size(), cold])
	# The scene itself, now that its parts are in the cache.
	var t_scene := Time.get_ticks_usec()
	var packed := load("res://Main.tscn") as PackedScene
	print("PROBE scene cached_load ms=%.1f ok=%s" % [
		float(Time.get_ticks_usec() - t_scene) / 1000.0, packed != null])
	if packed != null:
		var t_inst := Time.get_ticks_usec()
		var node := packed.instantiate()
		print("PROBE scene instantiate ms=%.1f" % [float(Time.get_ticks_usec() - t_inst) / 1000.0])
		node.free()
	quit(0)
