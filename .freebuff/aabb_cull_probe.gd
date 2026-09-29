extends SceneTree

# Does the real renderer cull a MultiMeshInstance3D whose instances are on screen
# but whose node sits at the world origin? Camera looks down -Z from (0,70,0), the
# instances sit right in front of it, and the origin itself is behind the camera:
# if the overlay is culled by a bounds box that only covers the node, the frame
# comes back empty.
func _init() -> void:
	var cam := Camera3D.new()
	cam.position = Vector3(0, 70, 0)
	get_root().add_child(cam)
	cam.current = true

	var cube := BoxMesh.new()
	cube.size = Vector3.ONE
	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = cube
	var inst := MultiMeshInstance3D.new()
	inst.multimesh = multi
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.albedo_color = Color(1, 0, 0, 1)
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	inst.material_override = mat
	get_root().add_child(inst)

	var pts: Array[Vector3] = []
	for x in range(-4, 5):
		for y in range(-4, 5):
			pts.append(Vector3(float(x) + 0.5, 70.5, -12.5 + float(y) * 0.0))
	multi.instance_count = pts.size()
	for i in pts.size():
		multi.set_instance_transform(i, Transform3D(Basis(), pts[i]))
	print("PROBE instances=%d node_at=%s" % [pts.size(), inst.global_position])

	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	var img: Image = get_root().get_texture().get_image()
	var red := 0
	for y in img.get_height():
		for x in img.get_width():
			var c := img.get_pixel(x, y)
			if c.r > 0.4 and c.g < 0.25 and c.b < 0.25:
				red += 1
	print("PROBE viewport=%dx%d red_pixels=%d" % [img.get_width(), img.get_height(), red])
	print("PROBE culling=%s" % ("BROKEN — on-screen instances were culled" if red == 0 else "fine"))
	quit()
