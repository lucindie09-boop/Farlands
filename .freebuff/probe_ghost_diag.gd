extends SceneTree
## Why the right-click preview shows the volume outline but no blocks.
##
## Reports each link in the chain: the icon renderer autoload, a ghost mesh built
## straight from it, whether the preview actually hands the wand cell data, and
## then the state of every per-block MultiMeshInstance3D the ghost keeps.
##
##   Godot --headless --path <project> --script res://.freebuff/probe_ghost_diag.gd

var _fail_count := 0

func _initialize() -> void:
	_run()

func _read_i32(bytes: PackedByteArray, at: int) -> int:
	return bytes.decode_s32(at)

func _mesh_text(m) -> String:
	if m == null:
		return "NULL"
	var surfaces: int = m.get_surface_count()
	var aabb: AABB = m.get_aabb()
	var text := "surfaces=%d aabb(size=%s)" % [surfaces, str(aabb.size)]
	if surfaces > 0:
		var mat = m.surface_get_material(0)
		if mat == null:
			text += " surface0_material=NULL"
		else:
			text += " surface0_material alpha=%.2f transparency=%d blend=%d no_depth=%s" % [
				mat.albedo_color.a, mat.transparency, mat.blend_mode, str(mat.no_depth_test)]
	return text

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 30:
		await process_frame

	# 1. the autoload the ghost asks for its meshes
	var renderer := root.get_node_or_null("BlockIconRenderer")
	print("PROBE /root/BlockIconRenderer: %s" % ["found" if renderer != null else "MISSING"])
	if renderer == null:
		quit(1)
		return
	print("PROBE renderer has build_ghost_mesh=%s" % str(renderer.has_method("build_ghost_mesh")))
	var direct = renderer.build_ghost_mesh(1, 0.35)
	print("PROBE build_ghost_mesh(stone=1) -> %s" % _mesh_text(direct))
	print("PROBE build_ghost_mesh(stone=1) again -> %s" % _mesh_text(renderer.build_ghost_mesh(1, 0.35)))

	# 2. give the wand a build and anchor it, like a right click does
	var player: Node = main.get_node_or_null("Player")
	var wand: Node = main.get_node_or_null("HUD/Wand")
	if player == null or wand == null:
		print("PROBE FAIL: player=%s wand=%s" % [str(player), str(wand)])
		quit(1)
		return
	var wand_id: int = BlockTextures.get_block_id_by_name("wand")
	player.set_hotbar_slot(0, wand_id, 1)
	for i in 5:
		await process_frame
	print("PROBE wand held=%s selected=%d wand_id=%d" % [
		str(player.is_wand_held()) if player.has_method("is_wand_held") else "?",
		int(player.get_selected_block()), wand_id])

	wand.set_function("paste")
	print("PROBE choose_file(church.schematic)=%s" % str(wand.choose_file("church.schematic")))
	var frames := 0
	while frames < 2000 and not wand._build_ready():
		await process_frame
		frames += 1
	print("PROBE build ready=%s after %d frames" % [str(wand._build_ready()), frames])

	var origin := Vector3i(int(player.global_position.x), int(player.global_position.y) + 6, int(player.global_position.z) - 12)
	var info: Dictionary = wand.anchor_preview_at(origin)
	print("PROBE anchor ok=%s keys=%s" % [str(info.get("ok", false)), str(info.keys())])
	var cells: PackedByteArray = info.get("cells", PackedByteArray())
	print("PROBE info cells bytes=%d (type %s)" % [cells.size(), typeof(cells)])

	# 3. the ghost itself: ONE MultiMesh of cubes, every instance placed through the
	# per-instance setter. Do not go back to assigning `buffer` blind: its layout is
	# a 3x4 row-major matrix, not origin-first, and getting it wrong is silent (see
	# AGENTS.md and .freebuff/probe_multimesh_buffer.gd).
	var ghost: MultiMeshInstance3D = wand._ghost
	var multi: MultiMesh = ghost.multimesh
	print("PROBE ghost node=%s visible=%s instances=%d aabb=%s" % [
		str(ghost), str(ghost.visible), multi.instance_count, str(ghost.custom_aabb)])
	var expected := int(floor(cells.size() / 16.0))
	if multi.instance_count > 0 and cells.size() >= 16:
		var first: Transform3D = multi.get_instance_transform(0)
		var last: Transform3D = multi.get_instance_transform(multi.instance_count - 1)
		print("PROBE first cell (%d,%d,%d) -> instance 0 at %s" % [
			_read_i32(cells, 0), _read_i32(cells, 4), _read_i32(cells, 8), str(first.origin)])
		print("PROBE last cell (%d,%d,%d) -> instance %d at %s" % [
			_read_i32(cells, cells.size() - 16), _read_i32(cells, cells.size() - 12),
			_read_i32(cells, cells.size() - 8), multi.instance_count - 1, str(last.origin)])
	print("PROBE cells=%d instances=%d" % [expected, multi.instance_count])

	# 4. does the outline (the part that DOES draw) differ in any way?
	print("PROBE outline visible=%s material=%s" % [
		str(wand._outline.visible) if wand._outline != null else "null",
		"blend=%d no_depth=%s alpha=%.2f priority=%d" % [
			wand._outline.material_override.blend_mode,
			str(wand._outline.material_override.no_depth_test),
			wand._outline.material_override.albedo_color.a,
			wand._outline.material_override.render_priority] if wand._outline != null else "-"])
	# 5. the cells' own material. Two properties are load-bearing and both were got
	# wrong once: MIX rather than ADD (adding has no ceiling, so a deep volume sums
	# to white and the brightness moves with the camera), and the depth test ON (an
	# X-ray ghost draws through terrain AND through the player's own arm and item).
	var cells_mat = ghost.material_override
	print("PROBE cells material: transparency=%d blend=%d (MIX=%d) cull=%d (DISABLED=%d) no_depth=%s depth_draw=%d priority=%d alpha=%.2f tint=%s" % [
		cells_mat.transparency, cells_mat.blend_mode, BaseMaterial3D.BLEND_MODE_MIX,
		cells_mat.cull_mode, BaseMaterial3D.CULL_DISABLED, str(cells_mat.no_depth_test),
		cells_mat.depth_draw_mode, cells_mat.render_priority, cells_mat.albedo_color.a,
		str(cells_mat.albedo_color)])
	if cells_mat.blend_mode != BaseMaterial3D.BLEND_MODE_MIX:
		print("PROBE FAIL: cells blend mode %d is not MIX — additive volume brightens with depth" % cells_mat.blend_mode)
		_fail_count += 1
	if cells_mat.no_depth_test:
		print("PROBE FAIL: cells are an X-ray — they will draw through terrain, the player model and the viewmodel")
		_fail_count += 1
	if cells_mat.cull_mode != BaseMaterial3D.CULL_DISABLED:
		print("PROBE FAIL: cells cull a face; standing inside the ghost will look hollow")
		_fail_count += 1
	# Depth decides who is in front, and the overlay needs both halves of it: it has
	# to WRITE depth (the liquid shader does, so an overlay that does not gets depth-
	# rejected behind any liquid and simply never draws there), and it has to draw
	# BEFORE the liquids so their test is taken against the overlay and not the other
	# way round. Ordering alone cannot do it: it only decides who blends over whom.
	if cells_mat.depth_draw_mode != BaseMaterial3D.DEPTH_DRAW_ALWAYS:
		print("PROBE FAIL: cells do not write depth (%d), so a liquid behind them culls them away" % cells_mat.depth_draw_mode)
		_fail_count += 1
	if cells_mat.render_priority >= 0:
		print("PROBE FAIL: cells have priority %d; they must draw before the liquids (priority < 0) for their depth to be laid down first" % cells_mat.render_priority)
		_fail_count += 1
	if wand._outline != null and wand._outline.material_override.render_priority <= cells_mat.render_priority:
		print("PROBE FAIL: the outline is not above the cells; its own ghost can cover the box")
		_fail_count += 1
	print("PROBE failures=%d" % _fail_count)
	print("PROBE done")
	quit(1 if _fail_count > 0 else 0)