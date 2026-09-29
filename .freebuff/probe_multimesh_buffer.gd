extends SceneTree
## MultiMesh.buffer versus set_instance_transform, rendered.
##
## The wand's ghost fills a buffer of 12 floats per instance and relies on it;
## reading those instances back in a windowed run gives (1,1,1) rather than the
## cells they were built from, which would stack every block of the preview at one
## point - an outline around nothing, exactly what the player reports. This puts
## two MultiMeshes with the same five positions in front of a camera, one built
## each way, and reports what each stored and how much of the screen it covers.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_multimesh_buffer.gd

func _initialize() -> void:
	_run()

func _material(colour: Color) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = colour
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.no_depth_test = true
	return mat

func _positions() -> Array:
	var list: Array = []
	for i in 5:
		list.append(Vector3(float(i) * 3.0, 0.0, 0.0))
	return list

func _shot() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

func _count(image: Image, want: Color) -> int:
	var found := 0
	for y in range(0, image.get_height(), 2):
		for x in range(0, image.get_width(), 2):
			var c := image.get_pixel(x, y)
			if ((want.r > 0.5 and c.r > 0.35 and c.g < 0.3)
					or (want.g > 0.5 and c.g > 0.35 and c.r < 0.3)):
				found += 1
	return found

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 20:
		await process_frame

	var cam := Camera3D.new()
	cam.position = Vector3(6, 40, 0)
	main.add_child(cam)
	cam.make_current()

	var mesh := BoxMesh.new()
	mesh.size = Vector3(1, 1, 1)

	# A: through the buffer, the way the wand does it.
	var via_buffer := MultiMesh.new()
	via_buffer.transform_format = MultiMesh.TRANSFORM_3D
	via_buffer.mesh = mesh
	var positions := _positions()
	via_buffer.instance_count = positions.size()
	var floats := PackedFloat32Array()
	floats.resize(positions.size() * 12)
	for i in positions.size():
		var base := i * 12
		var at: Vector3 = positions[i]
		floats[base + 0] = at.x
		floats[base + 1] = at.y
		floats[base + 2] = at.z
		floats[base + 3] = 1.0
		floats[base + 7] = 1.0
		floats[base + 11] = 1.0
	via_buffer.buffer = floats
	mesh.surface_set_material(0, _material(Color(1, 0, 0)))
	var node_a := MultiMeshInstance3D.new()
	node_a.multimesh = via_buffer
	node_a.position = Vector3(-10, 0, -20)
	main.add_child(node_a)

	# B: through the per-instance setter, same positions.
	var mesh_b := BoxMesh.new()
	mesh_b.size = Vector3(1, 1, 1)
	mesh_b.surface_set_material(0, _material(Color(0, 1, 0)))
	var via_setter := MultiMesh.new()
	via_setter.transform_format = MultiMesh.TRANSFORM_3D
	via_setter.mesh = mesh_b
	via_setter.instance_count = positions.size()
	for i in positions.size():
		via_setter.set_instance_transform(i, Transform3D(Basis(), positions[i]))
	var node_b := MultiMeshInstance3D.new()
	node_b.multimesh = via_setter
	node_b.position = Vector3(-10, 0, -40)
	main.add_child(node_b)

	for i in 5:
		await process_frame
	for i in positions.size():
		print("PROBE buffer path instance %d reads back %s (wanted %s)" % [
			i, str(via_buffer.get_instance_transform(i).origin), str(positions[i])])
	for i in positions.size():
		print("PROBE setter path instance %d reads back %s (wanted %s)" % [
			i, str(via_setter.get_instance_transform(i).origin), str(positions[i])])

	var image: Image = await _shot()
	print("PROBE rendered: buffer path red pixels=%d   setter path green pixels=%d" % [
		_count(image, Color(1, 0, 0)), _count(image, Color(0, 1, 0))])
	print("PROBE done")
	quit(0)
