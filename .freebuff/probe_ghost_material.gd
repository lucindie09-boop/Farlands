extends SceneTree
## Which material treatment makes the wand's ghost blocks readable?
##
## The ghost's materials are the real per-face textures at alpha 0.42 with the
## depth test off, and the player reports the cells as invisible while the volume
## outline (a plain additive line material) reads fine. So this puts the ghost's
## own mesh in front of a camera and measures, for each treatment, how many pixels
## it changes against the same frame with the ghost hidden. A number near zero
## means that treatment does not show up at all.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_ghost_material.gd

var _baseline: Image

func _initialize() -> void:
	_run()

func _shot() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

func _changed(image: Image) -> int:
	var count := 0
	for y in range(0, image.get_height(), 2):
		for x in range(0, image.get_width(), 2):
			var a := _baseline.get_pixel(x, y)
			var b := image.get_pixel(x, y)
			if absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b) > 0.03:
				count += 1
	return count

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
	var mesh: ArrayMesh = renderer.build_ghost_mesh(1, 0.42)
	if mesh == null:
		print("PROBE FAIL: no ghost mesh")
		quit(1)
		return
	var base_material: StandardMaterial3D = mesh.surface_get_material(0)
	print("PROBE ghost material: alpha=%.2f transparency=%d blend=%d no_depth=%s cull=%d textured=%s" % [
		base_material.albedo_color.a, base_material.transparency, base_material.blend_mode,
		str(base_material.no_depth_test), base_material.cull_mode,
		str(base_material.albedo_texture != null)])

	# Our own camera, looking straight at where the test node goes.
	var cam := Camera3D.new()
	cam.position = Vector3(0, 200, 0)
	main.add_child(cam)
	cam.make_current()

	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = mesh
	multi.instance_count = 1
	multi.set_instance_transform(0, Transform3D(Basis().scaled(Vector3(6, 6, 6)), Vector3(0, 200, -8)))
	var node := MultiMeshInstance3D.new()
	node.multimesh = multi
	node.visible = false
	main.add_child(node)

	for i in 5:
		await process_frame
	_baseline = await _shot()
	print("PROBE baseline taken")

	var variants := [
		{"name": "as shipped (mix, depth off, both faces)", "apply": func(m): pass},
		{"name": "mix, DEPTH TEST ON, both faces", "apply": func(m): m.no_depth_test = false},
		{"name": "additive, depth off, both faces", "apply": func(m): m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD},
		{"name": "mix, depth on, alpha 0.65", "apply": func(m):
			m.no_depth_test = false
			m.albedo_color = Color(1, 1, 1, 0.65)},
	]
	for variant in variants:
		for surface in range(mesh.get_surface_count()):
			var material: StandardMaterial3D = base_material.duplicate()
			# Keep each face's own texture, which is the point of the treatment.
			material.albedo_texture = mesh.surface_get_material(surface).albedo_texture
			(variant["apply"] as Callable).call(material)
			mesh.surface_set_material(surface, material)
		node.visible = true
		for i in 4:
			await process_frame
		var image: Image = await _shot()
		print("PROBE %-46s pixels changed=%d" % [variant["name"], _changed(image)])
		node.visible = false
		for i in 3:
			await process_frame

	print("PROBE done")
	quit(0)
