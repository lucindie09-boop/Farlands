extends SceneTree

# The real overlay shape: the NODE sits at the world origin while its instances
# are far away, where the player is. If the instance bounds come from the node,
# the whole overlay is culled the moment the origin leaves the frustum — which is
# "it vanishes at random angles" while standing 1500 blocks away.
func _init() -> void:
	var cam := Camera3D.new()
	cam.position = Vector3(1500, 70, 1500)
	cam.rotation_degrees = Vector3(0, 0, 0)  # looking -Z
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
	mat.cull_mode = BaseMaterial3D.CULL_BACK
	mat.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	inst.material_override = mat
	get_root().add_child(inst)   # node at (0,0,0): 2100 blocks behind the camera

	var pts: Array[Vector3] = []
	for x in range(-4, 5):
		pts.append(Vector3(1500.5 + float(x), 70.5, 1487.5))
	multi.instance_count = pts.size()
	for i in pts.size():
		multi.set_instance_transform(i, Transform3D(Basis(), pts[i]))

	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	var red := _count_red(get_root().get_texture().get_image())
	print("PROBE no custom_aabb: red_pixels=%d (node at %s, instances at 1500,70,1487)"
		% [red, inst.global_position])

	# The fix under test: bounds that cover the instances.
	inst.custom_aabb = AABB(Vector3(1494, 64, 1482), Vector3(13, 13, 12))
	await process_frame
	await RenderingServer.frame_post_draw
	var red2 := _count_red(get_root().get_texture().get_image())
	print("PROBE with custom_aabb: red_pixels=%d" % red2)
	quit()

func _count_red(img: Image) -> int:
	var n := 0
	for y in img.get_height():
		for x in img.get_width():
			var c := img.get_pixel(x, y)
			if c.r > 0.4 and c.g < 0.25 and c.b < 0.25:
				n += 1
	return n
