extends SceneTree
## Boots the real game scene, holds for a few seconds of frames and quits.
##
## No deliberate fault anywhere: the point is to find out whether a plain
## startup dies on its own (the player's report is a crash about a second in,
## after world load starts). A crash here leaves a report in FARLANDS_CRASH_DIR,
## which the runner points somewhere temporary.
##
##   FARLANDS_CRASH_DIR=<dir> .freebuff/run_probe.sh .freebuff/probe_boot_hunt.gd

func _initialize() -> void:
	_run()

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	var frames := 600
	for i in frames:
		await process_frame
	print("PROBE boot survived %d frames" % frames)
	quit(0)
