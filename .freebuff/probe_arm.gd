extends Node

const SHOT_DIR := "user://probe_arm"
const MODEL_SCALE := 0.05625
const MC_ARM_BASIS := Basis(
	Vector3(-0.3679, -0.7333, -0.5717),
	Vector3(0.3336, 0.4698, -0.8173),
	Vector3(0.8679, -0.4915, 0.0717)
)

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1.0, 0.0, 1.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1)
	env.ambient_light_energy = 1.0
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var cam := Camera3D.new()
	cam.fov = 70.0
	add_child(cam)
	cam.make_current()
	# --- replicate viewmodel.gd arm assembly (under the camera) ---
	var arm_root := Node3D.new()
	cam.add_child(arm_root)
	arm_root.position = Vector3(1.41, -0.1, -1.59)
	var mdl: Node3D = load("res://player.glb").instantiate()
	var arm_node := mdl.get_child(3) as Node3D
	mdl.remove_child(arm_node)
	mdl.free()
	var arm_pivot := Node3D.new()
	arm_root.add_child(arm_pivot)
	arm_pivot.add_child(arm_node)
	arm_node.position = Vector3(0, -12.0, 0) * MODEL_SCALE
	arm_node.scale = Vector3.ONE * MODEL_SCALE * 1.17
	arm_pivot.basis = MC_ARM_BASIS
	_flip_arm_mesh_uvs(arm_node)
	_apply_skin(arm_node)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 35, 0)
	light.light_energy = 1.6
	add_child(light)
	var i := 0
	for roll: float in [0.0, 30.0, 60.0, 90.0, 120.0, 150.0, 180.0, 210.0, 240.0, 270.0, 300.0, 330.0]:
		arm_root.rotation = Vector3(0, 0, deg_to_rad(roll))
		await get_tree().process_frame
		await get_tree().process_frame
		var img: Image = get_viewport().get_texture().get_image()
		img.save_png(SHOT_DIR + "/a_%d.png" % i)
		print("PROBE saved a_", i)
		i += 1
	get_tree().quit()

func _flip_arm_mesh_uvs(arm: Node3D) -> void:
	var mi := arm as MeshInstance3D
	if mi == null or mi.mesh == null:
		return
	var old_mesh := mi.mesh
	var new_mesh := ArrayMesh.new()
	for s in range(old_mesh.get_surface_count()):
		var arrays := old_mesh.surface_get_arrays(s)
		var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV].duplicate()
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var top_indices: Array[int] = []
		var bot_indices: Array[int] = []
		for vi in range(uvs.size()):
			var norm := normals[vi]
			if norm.y > 0.5:
				top_indices.append(vi)
			elif norm.y < -0.5:
				bot_indices.append(vi)
			else:
				uvs[vi].y = (52.0 / 64.0) - uvs[vi].y
		for ti in top_indices:
			var tp := positions[ti]
			for bi in bot_indices:
				var bp := positions[bi]
				if is_equal_approx(tp.x, bp.x) and is_equal_approx(tp.z, bp.z):
					var temp := uvs[ti]
					uvs[ti] = uvs[bi]
					uvs[bi] = temp
					break
		arrays[Mesh.ARRAY_TEX_UV] = uvs
		new_mesh.add_surface_from_arrays(old_mesh.surface_get_primitive_type(s), arrays)
	mi.mesh = new_mesh

func _apply_skin(arm: Node3D) -> void:
	var mgr := get_node_or_null("/root/SkinManager")
	var tex: Texture2D = mgr.get_texture() if mgr != null else null
	var mi := arm as MeshInstance3D
	if mi == null or mi.mesh == null:
		return
	var std_mat := StandardMaterial3D.new()
	std_mat.shading_mode = BaseMaterial3D.SHADING_MODE_PER_PIXEL
	std_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	std_mat.no_depth_test = true
	std_mat.render_priority = 5
	std_mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
	std_mat.roughness = 1.0
	std_mat.metallic = 0.0
	if tex != null:
		std_mat.albedo_texture = tex
	mi.material_override = std_mat