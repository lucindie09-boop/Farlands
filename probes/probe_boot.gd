extends SceneTree
## Boots the real scene with the real renderer and quits: used to hunt the
## intermittent startup crash, which happens within a second of launch.
func _initialize() -> void:
	_run()

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 240:
		await process_frame
	print("PROBE boot survived 240 frames")
	quit(0)
