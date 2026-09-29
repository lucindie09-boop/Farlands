extends SceneTree
## Is rebuilding the texture array (compression on -> off -> on) safe while
## chunks are streaming? That is what the Liquid Texture Lab's Live toggle does,
## so if it stalls here it would stall in the game.

func _initialize() -> void:
	_run()

func _run() -> void:
	var main: Node3D = load("res://main.tscn").instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("PROBE no chunk manager")
		quit(1)
		return
	for i in 20:
		await process_frame
	print("PROBE stats before: " + str(cm.call("get_fluid_stats")).substr(0, 0) + "compression=" + str(cm.call("get_compression_enabled")))
	var was: bool = cm.call("get_compression_enabled")

	var start := Time.get_ticks_msec()
	cm.call("set_compression_enabled", not was)
	print("PROBE flip 1 took %d ms" % (Time.get_ticks_msec() - start))
	for i in 5:
		await process_frame

	start = Time.get_ticks_msec()
	cm.call("set_compression_enabled", was)
	print("PROBE flip 2 took %d ms" % (Time.get_ticks_msec() - start))
	for i in 5:
		await process_frame

	# And with frames being pushed between the flips, like Live mode does.
	var frame := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	frame.fill(Color(1, 0, 0, 1))
	start = Time.get_ticks_msec()
	for i in 40:
		cm.call("push_texture_frame", "water", frame)
		await process_frame
	print("PROBE 40 pushes took %d ms" % (Time.get_ticks_msec() - start))
	cm.call("restore_texture_layer", "water")
	print("PROBE compression now=" + str(cm.call("get_compression_enabled")) + " (was " + str(was) + ")")
	print("PROBE PASS")
	quit(0)
