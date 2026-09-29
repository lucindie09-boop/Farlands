extends SceneTree
## Does a MultiMesh draw a mesh that has several surfaces?
##
## The wand's ghost hands each block's own 6-surface mesh (one surface per face,
## which is what gives every face its own texture) to a MultiMesh, and nothing
## shows up in game while the volume outline draws fine. This puts both shapes in
## front of a camera at once - red = the mesh as built (many surfaces), green =
## the same geometry merged into one surface - and counts the pixels of each, so
## the answer is a number rather than a hunch.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_multimesh_surfaces.gd

func _initialize() -> void:
	_run()

func _flat_material(colour: Color) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = colour
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.no_depth_test = true
	mat.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	return mat

## Every surface of `src` appended into one, so the mesh has a single surface.
func _merge(src: Mesh) -> ArrayMesh:
	var tool := SurfaceTool.new()
	tool.begin(Mesh.PRIMITIVE_TRIANGLES)
	for surface in range(src.get_surface_count()):
		tool.append_from(src, surface, Transform3D.IDENTITY)
	return tool.commit()

func _make_node(mesh: Mesh, at: Vector3, colour: Color) -> MultiMeshInstance3D:
	if mesh is ArrayMesh:
		for surface in range(mesh.get_surface_count()):
			mesh.surface_set_material(surface, _flat_material(colour))
	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = mesh
	multi.instance_count = 1
	multi.set_instance_transform(0, Transform3D(Basis().scaled(Vector3(8, 8, 8)), at))
	var node := MultiMeshInstance3D.new()
	node.multimesh = multi
	return node

func _count(image: Image) -> Dictionary:
	var red := 0
	var green := 0
	for y in range(0, image.get_height(), 2):
		for x in range(0, image.get_width(), 2):
			var c := image.get_pixel(x, y)
			if c.r > 0.4 and c.g < 0.3:
				red += 1
			elif c.g > 0.4 and c.r < 0.3:
				green += 1
	return {"red": red, "green": green}

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
	var source: ArrayMesh = renderer.build_ghost_mesh(1, 1.0)
	if source == null:
		print("PROBE FAIL: build_ghost_mesh gave null")
		quit(1)
		return
	print("PROBE source surfaces=%d" % source.get_surface_count())
	var merged: ArrayMesh = _merge(source)
	print("PROBE merged surfaces=%d" % merged.get_surface_count())

	# A camera of our own, looking at the two, so the game's camera does not matter.
	var cam := Camera3D.new()
	cam.position = Vector3(0, 0, 0)
	cam.rotation = Vector3.ZERO
	main.add_child(cam)
	cam.make_current()

	var left := _make_node(source, Vector3(-1.2, 0, -4.0), Color(1, 0, 0))
	var right := _make_node(merged, Vector3(1.2, 0, -4.0), Color(0, 1, 0))
	main.add_child(left)
	main.add_child(right)

	for i in 5:
		await process_frame
	await RenderingServer.frame_post_draw
	var image := root.get_texture().get_image()
	if image == null:
		print("PROBE FAIL: no viewport image")
		quit(1)
		return
	var counts := _count(image)
	print("PROBE multi-surface mesh (red) pixels=%d   merged single surface (green) pixels=%d" % [
		counts["red"], counts["green"]])
	print("PROBE verdict: %s" % [
		"a MultiMesh only draws ONE surface (merge before use)"
		if counts["green"] > 0 and counts["red"] == 0
		else "multi-surface meshes DO draw (look elsewhere)"
		if counts["green"] > 0 and counts["red"] > 0
		else "neither drew - the MultiMesh setup itself is wrong"])
	quit(0)
