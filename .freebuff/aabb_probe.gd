extends SceneTree

func _init() -> void:
	var root := Node3D.new()
	get_root().add_child(root)
	var cube := BoxMesh.new()
	cube.size = Vector3.ONE
	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = cube
	var inst := MultiMeshInstance3D.new()
	inst.multimesh = multi
	root.add_child(inst)
	# Instances like the overlays: centred on far-away block cells.
	var pts := [Vector3(1200.5, 70.5, 900.5), Vector3(1210.5, 72.5, 910.5)]
	multi.instance_count = pts.size()
	for i in pts.size():
		multi.set_instance_transform(i, Transform3D(Basis(), pts[i]))
	print("multimesh aabb      : ", multi.get_aabb())
	print("instance aabb       : ", inst.get_aabb())
	print("custom_aabb (unset) : ", inst.custom_aabb)
	quit()
