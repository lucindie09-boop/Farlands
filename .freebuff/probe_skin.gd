extends SceneTree

func _initialize() -> void:
	var mgr := root.get_node_or_null("SkinManager")
	print("PROBE SkinManager autoload found: ", mgr != null)
	if mgr != null:
		var tex: ImageTexture = mgr.get_texture()
		var img: Image = tex.get_image()
		var transparent := 0
		for y in range(img.get_height()):
			for x in range(img.get_width()):
				if img.get_pixel(x, y).a < 0.05:
					transparent += 1
		print("PROBE SkinManager texture ", img.get_width(), "x", img.get_height(),
			" transparent texels (a<0.05): ", transparent)
	var model: Node3D = load("res://player.glb").instantiate()
	model.set_script(load("res://scripts/player_model.gd"))
	root.add_child(model)
	await process_frame
	for mi in model.find_children("", "MeshInstance3D", true, false):
		for s in range(mi.mesh.get_surface_count()):
			var m: Material = mi.get_surface_override_material(s)
			if m == null:
				m = mi.mesh.surface_get_material(s)
			if m is StandardMaterial3D:
				var sm := m as StandardMaterial3D
				print("PROBE ", mi.name, " surf ", s,
					" transparency=", sm.transparency,
					" cull=", sm.cull_mode,
					" filter=", sm.texture_filter,
					" albedo=", sm.albedo_texture)
			else:
				print("PROBE ", mi.name, " surf ", s, " NOT StandardMaterial3D: ", m)
	quit()