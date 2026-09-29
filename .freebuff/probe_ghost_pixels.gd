extends SceneTree
## What the wand's right-click ghost actually puts on screen.
##
## Renders a frame without the preview, anchors it in front of the camera, renders
## again, and counts the pixels that changed - plus the renderer's own draw-call
## and primitive counters either side. That separates the two possibilities the
## headless probe cannot: the ghost is not submitted to the renderer at all, or it
## is submitted and simply too faint to see.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_ghost_pixels.gd

var _image_a: Image
var _image_b: Image

func _initialize() -> void:
	_run()

func _diff(a: Image, b: Image) -> int:
	var changed := 0
	for y in range(0, a.get_height(), 2):
		for x in range(0, a.get_width(), 2):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			if absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b) > 0.03:
				changed += 1
	return changed

func _monitors() -> String:
	return "primitives=%d draw_calls=%d objects=%d" % [
		int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME))]

func _shot() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 30:
		await process_frame

	var player: Node = main.get_node_or_null("Player")
	var cam: Camera3D = main.get_node_or_null("Player/Camera3D")
	var wand: Node = main.get_node_or_null("HUD/Wand")
	if player == null or cam == null or wand == null:
		print("PROBE FAIL: player=%s camera=%s wand=%s" % [str(player), str(cam), str(wand)])
		quit(1)
		return

	var wand_id: int = BlockTextures.get_block_id_by_name("wand")
	# The saved inventory lands a frame or two after the scene builds, and filling a
	# slot the player is not looking at leaves the hand empty: set it AND select it
	# until it sticks, or the wand cancels its own preview and this measures nothing.
	for _attempt in 60:
		player.set_hotbar_slot(0, wand_id, 1)
		player.select_hotbar_slot(0)
		await process_frame
		if int(player.get_selected_block()) == wand_id:
			break
	print("PROBE player: selected=%d wand_id=%d held=%s" % [
		int(player.get_selected_block()), wand_id,
		str(player.is_wand_held()) if player.has_method("is_wand_held") else "?"])
	if int(player.get_selected_block()) != wand_id:
		print("PROBE FAIL: the wand is not held, the preview will cancel itself")
		quit(1)
		return
	wand.set_function("paste")
	wand.choose_file("church.schematic")
	var waits := 0
	while waits < 2000 and not wand._build_ready():
		await process_frame
		waits += 1
	for i in 5:
		await process_frame

	# Somewhere the camera is definitely looking at: straight down its own axis.
	var forward: Vector3 = -cam.global_transform.basis.z
	var aim: Vector3 = cam.global_position + forward * 14.0
	var origin := Vector3i(int(aim.x), int(aim.y), int(aim.z))
	print("PROBE camera at %s looking at %s" % [str(cam.global_position), str(origin)])

	_image_a = await _shot()
	print("PROBE without preview: %s" % _monitors())

	var info: Dictionary = wand.anchor_preview_at(origin)
	print("PROBE anchor ok=%s cells=%d" % [
		str(info.get("ok", false)), (info.get("cells", PackedByteArray()) as PackedByteArray).size()])
	for i in 5:
		await process_frame
	_image_b = await _shot()
	print("PROBE with preview:    %s" % _monitors())

	var changed := _diff(_image_a, _image_b)
	print("PROBE pixels changed by the preview: %d of %d sampled" % [
		changed, (_image_a.get_width() / 2) * (_image_a.get_height() / 2)])

	# The in-game state of the nodes themselves, which headless cannot tell us.
	var nodes: Dictionary = wand._ghost_nodes
	var inside := 0
	var listed := 0
	for block_id in nodes.keys():
		var node = nodes[block_id]
		if node.is_inside_tree() and node.is_visible_in_tree():
			inside += 1
		if listed < 3:
			listed += 1
			print("PROBE   node[%d] visible=%s in_tree=%s global=%s aabb=%s" % [
				int(block_id), str(node.visible), str(node.is_inside_tree()),
				str(node.global_transform.origin), str(node.custom_aabb)])
	print("PROBE ghost nodes visible and in tree: %d of %d" % [inside, nodes.size()])
	if wand._outline != null:
		print("PROBE outline visible=%s in_tree=%s global=%s" % [
			str(wand._outline.visible), str(wand._outline.is_inside_tree()),
			str(wand._outline.global_transform.origin)])

	# And with the outline alone, so the two can be told apart.
	wand._outline.visible = false
	for i in 3:
		await process_frame
	var without_outline: Image = await _shot()
	print("PROBE pixels changed by the block cells alone: %d" % _diff(_image_a, without_outline))
	print("PROBE done")
	quit(0)
