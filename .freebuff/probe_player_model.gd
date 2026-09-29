extends SceneTree
## Headless check that the third-person body survives a Main.tscn re-save.
##
## The body is a player.glb instance named PlayerModel under Player, carrying
## player_model.gd (which needs a direct AnimationPlayer child for Idle.anim).
## PlayerController::_ready finds that node by name anywhere in the tree, moves
## it under a ModelPivot wrapper, and drives its visibility from the camera mode.
##
## The regression this catches: an editor re-save of Main.tscn dropping the node
## (and its ext_resources). The C++ then finds nothing, silently leaves model_
## null, and F5 shows an empty world with no error at all — so it is pinned here
## as an explicit contract instead of being trusted to a silent find_child.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_player_model.gd

var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _ok(msg: String) -> void:
	print("PROBE OK: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame

	var player := main.get_node_or_null("Player")
	if player == null:
		_fail("Player node missing")
		quit(1)
		return

	# 1. The node must exist SOMEWHERE in the tree — that is all the C++ needs,
	#    since it searches recursively and then reparents.
	var model: Node3D = main.find_child("PlayerModel", true, false) as Node3D
	if model == null:
		_fail("no PlayerModel node anywhere under Main — F5 will show no body")
		quit(1)
		return
	_ok("PlayerModel found in the scene")

	# 2. It must be a real glb instance with geometry, not an empty placeholder.
	var meshes := model.find_children("", "MeshInstance3D", true, false)
	if meshes.is_empty():
		_fail("PlayerModel has no MeshInstance3D children (empty glb instance)")
	else:
		_ok("PlayerModel carries %d mesh instances" % meshes.size())

	# 3. player_model.gd must be attached, or the skin and head-look break.
	if model.get_script() == null:
		_fail("PlayerModel has no script (skin/head-look would silently break)")
	else:
		_ok("PlayerModel runs player_model.gd")

	# 4. The Idle rig: player_model.gd looks up a DIRECT AnimationPlayer child and
	#    plays default/Idle. Missing it means a static body with no error.
	var anim := model.get_node_or_null("AnimationPlayer") as AnimationPlayer
	if anim == null:
		_fail("PlayerModel has no direct AnimationPlayer child — idle animation lost")
	elif not anim.has_animation("default/Idle"):
		_fail("AnimationPlayer has no default/Idle library (Idle.anim failed to load)")
	else:
		_ok("AnimationPlayer playing default/Idle")

	# 5. The controller must have taken ownership: reparented under ModelPivot and
	#    hidden while the camera is first person.
	var parent := model.get_parent()
	if parent == null or parent.name != "ModelPivot":
		_fail("PlayerModel is not under ModelPivot (parent=%s) — body-yaw lag lost"
			% ("null" if parent == null else parent.name))
	else:
		_ok("PlayerModel reparented under ModelPivot")

	if player.get_third_person_view() != 0:
		_fail("expected to start in first person, got view %d" % player.get_third_person_view())
	elif model.visible:
		_fail("body visible in first person — it would fill the screen")
	else:
		_ok("body hidden in first person")

	# 6. F5 (the cycle) must actually reveal it. Walk every mode: only view 0 hides.
	var shown := {}
	for view in range(3):
		player.set_third_person_view(view)
		await process_frame
		shown[view] = model.visible
	if shown[0] != false:
		_fail("first-person view shows the body")
	if not (shown[1] and shown[2]):
		_fail("third-person views did not show the body: %s" % [shown])
	else:
		_ok("third person shows the body (views 1 and 2)")

	# 7. The body must sit on the player's feet and be scaled like the glb rig,
	#    not left at the glb's raw import scale.
	var basis: Basis = model.global_transform.basis
	var sx: float = basis.get_scale().x
	if absf(sx - 0.05625) > 0.0001:
		_fail("model scale is %.5f, expected 0.05625" % sx)
	else:
		_ok("model keeps its 0.05625 glb scale")

	# 8. Restoring it must not have resurrected the old aim bug: the body sits at
	#    the player's horizontal position (the +0.0844 z offset is sub-block).
	var dr: Vector3 = model.global_position - (player as Node3D).global_position
	if absf(dr.x) > 0.2 or absf(dr.z) > 0.2:
		_fail("body offset from player by (%.2f, %.2f) — aim/eye line would drift"
			% [dr.x, dr.z])
	else:
		_ok("body centred on the player")

	player.set_third_person_view(0)
	await process_frame

	print("PROBE %s" % ("PASS" if ok else "FAIL"))
	quit(0 if ok else 1)
