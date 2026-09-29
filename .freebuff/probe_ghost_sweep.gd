extends SceneTree
## Is the wand's ghost actually DRAWN from every angle and distance, or does the
## renderer drop it?
##
## The report is that the preview disappears at some camera angles or distances and
## "culls when it shouldn't". Measuring the shipped material cannot answer that,
## because a 20%-opacity blue cell over blue sky changes the pixel by almost
## nothing — "faint" and "absent" come out as the same number. So this test changes
## only PRESENCE, not looks: the cells are forced to opaque magenta with the depth
## test off (so terrain cannot legitimately hide them either) and counted against a
## reference box of the same size wearing the same material. Whatever the angle, the
## cells sit inside that box, so the ratio between them should stay in a band; a
## cliff in it means instances really went undrawn.
##
## It also verifies every instance lies inside `custom_aabb`, the box the frustum
## test uses — if that box is too small, culling would depend on where the camera is,
## which is exactly the complaint.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_ghost_sweep.gd
##
## Needs a real renderer: a headless run draws nothing and returns zeros.

const DISTANCES := [8.0, 24.0, 64.0, 192.0]
const YAWS := [0.0, 90.0, 180.0, 270.0]
const STEP := 6  # sample every Nth pixel of each axis: the ghost covers thousands

var _ghost: MultiMeshInstance3D
var _outline: MeshInstance3D
var _reference: MeshInstance3D
var _camera: Camera3D

func _initialize() -> void:
	_run()

func _shot() -> Image:
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

## Pixels this frame differs from the baseline in, sampled on a grid. `get_pixel`,
## not `get_data`: the viewport's image format is not RGBA8 (assuming it was cost a
## whole sweep of silent zeros) and a byte-wise read of an unknown format is
## meaningless anyway.
func _diff(baseline: Image, image: Image) -> int:
	var width := mini(baseline.get_width(), image.get_width())
	var height := mini(baseline.get_height(), image.get_height())
	var count := 0
	var y := 0
	while y < height:
		var x := 0
		while x < width:
			var a := baseline.get_pixel(x, y)
			var b := image.get_pixel(x, y)
			if absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b) > 0.03:
				count += 1
			x += STEP
		y += STEP
	return count

func _set_visibility(cells: bool, reference: bool) -> void:
	_ghost.visible = cells
	_reference.visible = reference

## Anchors `file` above the player and orbits the camera around its volume box.
func _sweep(wand: Node, player: Node, file: String) -> void:
	print("PROBE ==== %s ====" % file)
	wand.set_function("paste")
	if not wand.choose_file(file):
		print("PROBE FAIL: choose_file(%s) refused" % file)
		return
	var frames := 0
	while frames < 4000 and not wand._build_ready():
		await process_frame
		frames += 1
	if not wand._build_ready():
		print("PROBE FAIL: %s never became ready" % file)
		return
	var origin := Vector3i(
		int(player.global_position.x), int(player.global_position.y) + 60, int(player.global_position.z))
	var info: Dictionary = wand.anchor_preview_at(origin)
	if not bool(info.get("ok", false)):
		print("PROBE FAIL: anchor refused for %s" % file)
		return
	for i in 4:
		await process_frame

	var box: AABB = _ghost.custom_aabb
	var multi: MultiMesh = _ghost.multimesh
	var center := box.get_center()
	var instance_lo := Vector3(INF, INF, INF)
	var instance_hi := Vector3(-INF, -INF, -INF)
	for i in multi.instance_count:
		var at: Vector3 = multi.get_instance_transform(i).origin
		instance_lo = instance_lo.min(at)
		instance_hi = instance_hi.max(at)
	# A cube of size 1 per instance, so the geometry reaches half a cell past the
	# instance origin.
	var geometry_lo := instance_lo - Vector3.ONE * 0.5
	var geometry_hi := instance_hi + Vector3.ONE * 0.5
	var encloses := box.encloses(AABB(geometry_lo, geometry_hi - geometry_lo))
	print("PROBE instances=%d cells=%d box=%s center=%s" % [
		multi.instance_count, int(floor(info.get("cells", PackedByteArray()).size() / 16.0)),
		str(box), str(center)])
	print("PROBE instance geometry %s..%s  custom_aabb encloses it: %s" % [
		str(geometry_lo), str(geometry_hi), str(encloses)])
	if not encloses:
		print("PROBE FAIL: custom_aabb does not enclose the instances — frustum culling can drop them")

	# The reference box: same volume, same material, drawn instead of the cells.
	var reference_mesh := BoxMesh.new()
	reference_mesh.size = box.size
	_reference.mesh = reference_mesh
	_reference.global_position = center

	var material := _ghost.material_override
	print("PROBE presence material: blend=%d (MIX=%d) alpha=%.2f no_depth=%s — magenta, so any cell drawn shows" % [
		material.blend_mode, BaseMaterial3D.BLEND_MODE_MIX, material.albedo_color.a, str(material.no_depth_test)])
	print("PROBE %-8s %-6s %8s %8s %7s" % ["dist", "yaw", "cells_px", "box_px", "ratio"])
	var lowest := INF
	var lowest_row := ""
	for distance in DISTANCES:
		for yaw in YAWS:
			var radians := deg_to_rad(yaw)
			_camera.global_position = center + Vector3(sin(radians), 0.0, cos(radians)) * distance
			_camera.look_at(center)
			_set_visibility(false, false)
			for i in 3:
				await process_frame
			var baseline: Image = await _shot()
			_set_visibility(true, false)
			for i in 3:
				await process_frame
			var cells := _diff(baseline, await _shot())
			_set_visibility(false, true)
			for i in 3:
				await process_frame
			var reference := _diff(baseline, await _shot())
			var ratio := float(cells) / float(maxi(reference, 1))
			print("PROBE %-8.0f %-6.0f %8d %8d %7.2f" % [distance, yaw, cells, reference, ratio])
			if ratio < lowest:
				lowest = ratio
				lowest_row = "dist %.0f yaw %.0f: cells %d vs box %d" % [distance, yaw, cells, reference]
	_set_visibility(false, false)
	print("PROBE lowest cells/box %.2f (%s)" % [lowest, lowest_row])

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 30:
		await process_frame

	var player: Node = main.get_node_or_null("Player")
	var wand: Node = main.get_node_or_null("HUD/Wand")
	if player == null or wand == null:
		print("PROBE FAIL: player=%s wand=%s" % [str(player), str(wand)])
		quit(1)
		return
	# The wand cancels its own preview when it is not the held item.
	player.set_hotbar_slot(0, BlockTextures.get_block_id_by_name("wand"), 1)
	for i in 5:
		await process_frame
	_ghost = wand._ghost
	_outline = wand._outline

	# Presence, not looks: opaque magenta, both faces, depth test off so terrain
	# cannot hide the ghost either — anything missing is the renderer dropping it.
	var presence := StandardMaterial3D.new()
	presence.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	presence.transparency = BaseMaterial3D.TRANSPARENCY_DISABLED
	presence.albedo_color = Color(1.0, 0.0, 1.0)
	presence.cull_mode = BaseMaterial3D.CULL_DISABLED
	presence.no_depth_test = true
	presence.disable_receive_shadows = true
	_ghost.material_override = presence
	_outline.visible = false

	_reference = MeshInstance3D.new()
	_reference.material_override = presence
	_reference.visible = false
	main.add_child(_reference)

	_camera = Camera3D.new()
	_camera.fov = 70.0
	_camera.far = 4000.0
	main.add_child(_camera)
	_camera.make_current()
	print("PROBE camera=%s ghost=%s" % [str(_camera), str(_ghost)])

	for file in ["church.schematic", "10179.schematic"]:
		await _sweep(wand, player, file)

	print("PROBE done")
	quit(0)
