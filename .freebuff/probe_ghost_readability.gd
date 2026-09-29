extends SceneTree
## How STRONGLY each ghost treatment shows up, not just how much area it covers.
##
## The coverage test could not tell a bold overlay from one you have to hunt for.
## This measures the size of the change: how many pixels differ from the same
## frame with the ghost hidden, the mean and the peak difference per channel sum.
## A treatment that covers thousands of pixels with a peak of 0.05 is invisible in
## practice, whatever its area says.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_ghost_readability.gd

var _baseline: Image

func _initialize() -> void:
	_run()

func _shot() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

func _measure(image: Image) -> String:
	var changed := 0
	var total := 0.0
	var peak := 0.0
	for y in range(0, image.get_height(), 2):
		for x in range(0, image.get_width(), 2):
			var a := _baseline.get_pixel(x, y)
			var b := image.get_pixel(x, y)
			var d := absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
			if d > 0.03:
				changed += 1
				total += d
			peak = maxf(peak, d)
	var mean := total / float(maxi(changed, 1))
	return "changed=%-7d mean_delta=%.3f peak_delta=%.3f" % [changed, mean, peak]

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 30:
		await process_frame

	var renderer := root.get_node_or_null("BlockIconRenderer")
	if renderer == null:
		print("PROBE FAIL: no BlockIconRenderer")
		quit(1)
		return
	# Deliberately over plain sky rather than terrain: a dark backdrop is where a
	# faint overlay is easiest to see, and where terrain cannot mask the result.
	var mesh: ArrayMesh = renderer.build_ghost_mesh(1, 0.42)
	if mesh == null:
		print("PROBE FAIL: no ghost mesh")
		quit(1)
		return

	var cam := Camera3D.new()
	cam.position = Vector3(0, 400, 0)
	main.add_child(cam)
	cam.make_current()

	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = mesh
	multi.instance_count = 1
	multi.set_instance_transform(0, Transform3D(Basis().scaled(Vector3(6, 6, 6)), Vector3(0, 400, -8)))
	var node := MultiMeshInstance3D.new()
	node.multimesh = multi
	node.visible = false
	main.add_child(node)

	for i in 5:
		await process_frame
	_baseline = await _shot()
	print("PROBE baseline (sky) taken")

	var variants := [
		{"name": "as shipped   mix 0.42, depth off", "blend": BaseMaterial3D.BLEND_MODE_MIX, "alpha": 0.42, "tint": 1.0, "depth": true},
		{"name": "mix 0.60, depth off", "blend": BaseMaterial3D.BLEND_MODE_MIX, "alpha": 0.60, "tint": 1.0, "depth": true},
		{"name": "mix 0.55, albedo 1.8x", "blend": BaseMaterial3D.BLEND_MODE_MIX, "alpha": 0.55, "tint": 1.8, "depth": true},
		{"name": "additive 0.42, depth off", "blend": BaseMaterial3D.BLEND_MODE_ADD, "alpha": 0.42, "tint": 1.0, "depth": true},
		{"name": "additive 0.85, depth off", "blend": BaseMaterial3D.BLEND_MODE_ADD, "alpha": 0.85, "tint": 1.0, "depth": true},
		{"name": "additive 0.60, albedo 1.6x", "blend": BaseMaterial3D.BLEND_MODE_ADD, "alpha": 0.60, "tint": 1.6, "depth": true},
	]
	for variant in variants:
		for surface in range(mesh.get_surface_count()):
			var source: StandardMaterial3D = mesh.surface_get_material(surface)
			var material: StandardMaterial3D = source.duplicate()
			material.blend_mode = variant["blend"]
			material.albedo_color = Color(variant["tint"], variant["tint"], variant["tint"], variant["alpha"])
			material.no_depth_test = variant["depth"]
			mesh.surface_set_material(surface, material)
		node.visible = true
		for i in 4:
			await process_frame
		var image: Image = await _shot()
		print("PROBE %-34s %s" % [variant["name"], _measure(image)])
		node.visible = false
		for i in 3:
			await process_frame

	print("PROBE done")
	quit(0)
