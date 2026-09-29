extends SceneTree
## Does the ghost draw over the player's own body, arm and held item?
##
## Reported symptom of the X-ray version: a preview you can see THROUGH your own
## hand. The fix was to give the cells the depth test back, so this checks it with
## the preview anchored around the player — camera inside the volume, looking through
## the model and the viewmodel at the cells. Any change in the regions the body and
## the item occupy means the ghost is drawn over them, i.e. still an X-ray.
##
## The outline is measured the same way and is EXPECTED to bleed through: it is the
## deliberate X-ray half of the pair.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_ghost_arm.gd
##
## Needs a real renderer (the real camera and its viewmodel).

const STEP := 4

var _baseline: Image

func _initialize() -> void:
	_run()

func _shot() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

## Changed pixels inside `rect`, sampled on a grid.
func _count(image: Image, rect: Rect2i) -> int:
	var count := 0
	var y := rect.position.y
	while y < rect.end.y:
		var x := rect.position.x
		while x < rect.end.x:
			var a := _baseline.get_pixel(x, y)
			var b := image.get_pixel(x, y)
			if absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b) > 0.03:
				count += 1
			x += STEP			y += STEP
	return count


func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 40:
		await process_frame

	var player: Node = main.get_node_or_null("Player")
	var wand: Node = main.get_node_or_null("HUD/Wand")
	if player == null or wand == null:
		print("PROBE FAIL: player=%s wand=%s" % [str(player), str(wand)])
		quit(1)
		return
	player.set_hotbar_slot(0, BlockTextures.get_block_id_by_name("wand"), 1)
	for i in 5:
		await process_frame

	wand.set_function("paste")
	if not wand.choose_file("church.schematic"):
		print("PROBE FAIL: choose_file refused")
		quit(1)
		return
	var frames := 0
	while frames < 4000 and not wand._build_ready():
		await process_frame
		frames += 1
	if not wand._build_ready():
		print("PROBE FAIL: build never became ready")
		quit(1)
		return

	# Anchor the build so the player stands at its middle: every direction is
	# through the volume, and the model and viewmodel are between eye and cells.
	var ghost: MultiMeshInstance3D = wand._ghost
	var outline: MeshInstance3D = wand._outline
	var at := Vector3i(
		int(player.global_position.x) - 13, int(player.global_position.y) - 13,
		int(player.global_position.z) - 15)
	var info: Dictionary = wand.anchor_preview_at(at)
	if not bool(info.get("ok", false)):
		print("PROBE FAIL: anchor refused")
		quit(1)
		return
	for i in 6:
		await process_frame
	print("PROBE anchored at %s instances=%d box=%s player=%s" % [
		str(at), ghost.multimesh.instance_count, str(ghost.custom_aabb), str(player.global_position)])

	var camera: Camera3D = player.get_node_or_null("Camera3D")
	if camera == null:
		var cameras := player.find_children("*", "Camera3D", true, false)
		if cameras.is_empty():
			print("PROBE FAIL: no player camera")
			quit(1)
			return
		camera = cameras[0]
	print("PROBE camera=%s current=%s" % [str(camera), str(camera.is_current())])

	# Regions where the body and the item live: the bottom strip of the frame, and
	# the bottom corners the arm and viewmodel occupy.
	var size := Vector2i(main.get_viewport().get_visible_rect().size)
	var regions := {
		"whole frame": Rect2i(0, 0, size.x, size.y),
		"bottom strip": Rect2i(0, int(size.y * 0.62), size.x, size.y),
		"bottom-right": Rect2i(int(size.x * 0.55), int(size.y * 0.55), size.x, size.y),
		"bottom-left": Rect2i(0, int(size.y * 0.55), int(size.x * 0.45), size.y),
	}

	ghost.visible = false
	outline.visible = false
	for i in 4:
		await process_frame
	_baseline = await _shot()

	ghost.visible = true
	for i in 4:
		await process_frame
	var cells: Image = await _shot()
	ghost.visible = false

	outline.visible = true
	for i in 4:
		await process_frame
	var out: Image = await _shot()
	outline.visible = false

	for name in regions:
		var rect: Rect2i = regions[name]
		print("PROBE %-13s cells_changed=%-7d outline_changed=%-7d" % [
			name, _count(cells, rect), _count(out, rect)])
	print("PROBE done")
	quit(0)
