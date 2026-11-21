extends SceneTree
## Checks that Main.tscn's third-person body is present and comes up: the
## PlayerModel node, the player.glb instance behind it, player_model.gd, its six
## parts with meshes and materials at body size, the skin, and the scene's own
## rule that third person reveals it.
##
## It exists because the controller only WARNS when the node is missing, and the
## instance vanished silently once: PlayerModel is stored as a uid reference, so
## while .godot/uid_cache.bin still mapped that uid to the pre-Assets/ location
## the editor dropped the instance on the next save, and nothing failed -- the
## game simply had no body in third person. The controller's reparent is the
## proof it resolved: it moves the node under a ModelPivot it creates itself, so a
## node the controller never found stays where the scene put it.
##
## Run: Godot --headless --path <project> --script res://probes/probe_player_model.gd
## Exits non-zero when any check fails, so it is safe to put in a script or a gate.

var failures := 0

func _initialize() -> void:
	_run()

func _check(ok: bool, what: String) -> void:
	if ok:
		print("PROBE player_model ok: " + what)
		return
	failures += 1
	print("PROBE player_model FAIL: " + what)
	push_error("probe_player_model: " + what)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_check(false, "Main.tscn loads")
		_summary()
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame

	var player := main.get_node_or_null("Player")
	_check(player != null, "Player exists")
	var model: Node = player.find_child("PlayerModel", true, false) if player != null else null
	if model == null:
		_check(false, "Player has a PlayerModel node (Main.tscn: add Assets/models/player.glb "
			+ "as Player/PlayerModel with player_model.gd attached)")
		_summary()
		return
	_check(true, "Player has a PlayerModel node")

	_check(model.scene_file_path == "res://Assets/models/player.glb",
		"it is the player.glb instance ('%s')" % model.scene_file_path)
	var script_obj: Variant = model.get_script()
	_check(script_obj != null and str(script_obj.resource_path).ends_with("scripts/player_model.gd"),
		"player_model.gd is attached ('%s')" % str(script_obj.resource_path if script_obj != null else "-"))
	# The controller reparents what it found; anything else means it did not find
	# this node and warned instead.
	var parent := model.get_parent()
	_check(parent != null and parent.name == "ModelPivot",
		"the controller resolved it and reparented it under ModelPivot ('%s')"
			% (parent.name if parent != null else "no parent"))

	var parts := model.find_children("*", "MeshInstance3D", true, false)
	var measured := 0
	var bare := 0
	var untextured := 0
	var aabb := AABB()
	for node in parts:
		var part := node as MeshInstance3D
		if part.mesh == null:
			bare += 1
			continue
		var textured := part.get_surface_override_material(0) != null \
			or part.material_override != null \
			or (part.mesh.get_surface_count() > 0 and part.mesh.surface_get_material(0) != null)
		if not textured:
			untextured += 1
		var box: AABB = part.global_transform * part.get_aabb()
		aabb = box if measured == 0 else aabb.merge(box)
		measured += 1
	_check(measured > 0, "it has mesh parts with meshes (%d of %d)" % [measured, parts.size()])
	_check(bare == 0, "every part carries its mesh (%d without)" % bare)
	_check(untextured == 0, "every part has a material (%d without)" % untextured)
	_check(measured > 0 and aabb.size.length() > 0.5,
		"the body is at the player and body-sized (%.2f x %.2f x %.2f blocks)"
			% [aabb.size.x, aabb.size.y, aabb.size.z])
	_check(aabb.size.y > 1.0, "it is body-height (%.2f blocks)" % aabb.size.y)

	var skin: Variant = model.get("skin_texture")
	_check(skin is Texture2D, "the skin texture loads")
	if skin is Texture2D:
		var size: Vector2 = (skin as Texture2D).get_size()
		_check(size.x > 0.0 and size.y > 0.0, "the skin has pixels (%dx%d)" % [int(size.x), int(size.y)])

	_check(not model.visible, "it starts hidden, as the scene sets it for first person")
	if player != null and player.has_method("set_third_person"):
		player.call("set_third_person", true)
		await process_frame
		await process_frame
		_check(model.visible, "third person reveals it")
		player.call("set_third_person", false)
		await process_frame
		_check(not model.visible, "back to first person hides it again")
	else:
		_check(false, "Player binds set_third_person")
	_summary()

func _summary() -> void:
	if failures == 0:
		print("PROBE player_model summary: the third-person body is in the scene and comes up")
		quit(0)
		return
	print("PROBE player_model summary: %d check(s) failed -- the third-person body is missing or broken" % failures)
	push_error("probe_player_model: %d check(s) failed" % failures)
	quit(1)
