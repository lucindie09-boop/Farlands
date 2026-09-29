extends SceneTree

func _init() -> void:
	var a: Animation = load("res://Animations/Idle.anim")
	print("length=", a.length, " tracks=", a.get_track_count())
	for t in range(a.get_track_count()):
		var path: NodePath = a.track_get_path(t)
		var kc: int = a.track_get_key_count(t)
		print("track ", t, " path=", path, " keys=", kc)
		for k in range(kc):
			var t0: float = a.track_get_key_time(t, k)
			var v: Variant = a.track_get_key_value(t, k)
			print("  key ", k, " t=", "%.3f" % t0, " val=", v)
	quit()