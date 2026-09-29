extends SceneTree
## Does continuously animating the liquid layers destabilise the world?
##
## probe_lava_acid.gd pushed frames into three texture-array layers every few
## ticks for the length of a run, and two of four runs died with a bare segfault
## right after startup — a rate the untouched probes show too, but "probably the
## usual headless flakiness" is not a diagnosis. This is the experiment: run the
## animator flat out for a fixed number of frames, then run the same number with
## it off, and say where it died.
##
##   Godot --headless --path <project> --script res://.freebuff/probe_anim_stress.gd
##   ... -- on     animate for the whole run (default)
##   ... -- off    never prepare the animator at all (the control)
##   ... -- push   push the same frames by hand, no animator (the tighter control)

const FRAMES := 2400   # ~20 s of wall clock at the frame rates headless reaches
const REPORT_EVERY := 200

var main: Node3D
var animator: Node
var cm: Node
var mode := "on"
var frames := 0
var pushes := 0
var frames_a: Array[Image] = []

func _initialize() -> void:
	var args: Array = OS.get_cmdline_user_args()
	if args.size() > 0:
		mode = String(args[0])
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing"); quit(1); return
	main = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	animator = root.get_node_or_null("LiquidAnimator")
	if cm == null or animator == null:
		print("PROBE FAIL: ChunkManager or LiquidAnimator missing"); quit(1); return

	if mode == "off":
		animator.call("set_enabled", false)
	elif mode == "push":
		animator.call("set_enabled", false)
		var settings: Dictionary = LiquidTextureGen.default_settings("water", 16)
		var strip: Image = LiquidTextureGen.generate_strip(settings)
		var size := strip.get_width()
		for index in int(strip.get_height() / size):
			frames_a.append(strip.get_region(Rect2i(0, index * size, size, size)))

	print("probe: mode %s, %d frames, animator enabled=%s" % [mode, FRAMES, animator.call("is_enabled")])
	var start := Time.get_ticks_msec()
	var previous := 0
	for index in range(FRAMES):
		await process_frame
		frames += 1
		if mode == "push":
			cm.push_texture_frame("water", frames_a[frames % frames_a.size()])
			pushes += 1
		elif mode == "off":
			pass
		else:
			var status: Dictionary = animator.call("get_status")
			if int(status.get("water", {}).get("pushed", -1)) != previous:
				previous = int(status.get("water", {}).get("pushed", -1))
				pushes += 1
		if frames % REPORT_EVERY == 0:
			print("probe: %s frame %d (%d ms, %d pushes)" % [mode, frames, Time.get_ticks_msec() - start, pushes])
	print("probe: %s finished %d frames (%d ms, %d pushes)" % [mode, frames, Time.get_ticks_msec() - start, pushes])
	print("PROBE PASS (%s)" % mode)
	quit(0)
