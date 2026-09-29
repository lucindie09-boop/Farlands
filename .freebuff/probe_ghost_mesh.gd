extends SceneTree
## The preview draws the BLOCKS now, so this checks the plumbing a rendered
## screenshot cannot: that group-by-block-type produces meshes, that each one
## carries its own translucent per-face materials, that the instance buffers are
## the size MultiMesh wants (12 floats each), and that a re-anchor to a different
## spot reuses the same nodes instead of leaking a set per click.
##
##   .freebuff/run_probe.sh .freebuff/probe_ghost_mesh.gd

const FILE := "church.schematic"

var main: Node
var wand: Node

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	print("PROBE FAIL: " + msg)
	quit(1)

func _run() -> void:
	main = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 40:
		await process_frame
	wand = main.get_node_or_null("HUD/Wand")
	if wand == null:
		_fail("no wand")
		return

	if not wand.choose_file(FILE):
		_fail("could not choose " + FILE)
		return
	var t := Time.get_ticks_usec()
	while not wand._build_ready() and Time.get_ticks_usec() - t < 30_000_000:
		await process_frame
	if not wand._build_ready():
		_fail(FILE + " never decoded")
		return

	var player: Node3D = main.get_node_or_null("Player")
	var at := player.global_position
	var anchor := Vector3i(int(at.x) - 24, int(at.y) + 4, int(at.z) - 24)
	var info: Dictionary = wand.anchor_preview_at(anchor)
	if not info.get("ok", false):
		_fail("anchor_preview_at -> %s" % str(info))
		return

	var nodes: Dictionary = wand._ghost_nodes
	if nodes.is_empty():
		_fail("no per-block ghost nodes were created")
		return
	var types := 0
	var total := 0
	var surfaces := 0
	var translucent := 0
	for block_id in nodes.keys():
		var node: MultiMeshInstance3D = nodes[block_id]
		if not node.visible:
			continue
		types += 1
		var multi: MultiMesh = node.multimesh
		total += multi.instance_count
		if multi.instance_count != multi.visible_instance_count and multi.visible_instance_count != -1:
			_fail("visible instance count %d != %d for block %d"
				% [multi.visible_instance_count, multi.instance_count, block_id])
			return
		var mesh: ArrayMesh = multi.mesh
		if mesh == null:
			_fail("block %d has no mesh" % block_id)
			return
		surfaces += mesh.get_surface_count()
		var mats := 0
		for s in range(mesh.get_surface_count()):
			var mat := mesh.surface_get_material(s)
			if mat == null:
				continue
			mats += 1
			if (mat as StandardMaterial3D).transparency == BaseMaterial3D.TRANSPARENCY_ALPHA:
				translucent += 1
		if mats != mesh.get_surface_count():
			_fail("block %d: %d of %d surfaces have a material"
				% [block_id, mats, mesh.get_surface_count()])
			return
		var aabb: AABB = node.custom_aabb
		if aabb.size.length() <= 0.0:
			_fail("block %d claims an empty custom_aabb" % block_id)
			return
	print("probe: %d block types, %d instances, %d surfaces, %d translucent materials"
		% [types, total, surfaces, translucent])
	var cells_returned := int(info.get("cells_returned", 0))
	if total != cells_returned:
		_fail("%d instances for %d cells (every cell should draw exactly one)"
			% [total, cells_returned])
		return

	# A re-anchor must reuse the same nodes: the ghost is clicked dozens of times.
	var before := nodes.size()
	var moved: Dictionary = wand.anchor_preview_at(anchor + Vector3i(6, 0, 0))
	if not moved.get("ok", false):
		_fail("re-anchor -> %s" % str(moved))
		return
	if wand._ghost_nodes.size() < before:
		_fail("re-anchor dropped nodes (%d -> %d)" % [before, wand._ghost_nodes.size()])
		return
	var re_used := 0
	for block_id in wand._ghost_nodes.keys():
		if wand._ghost_nodes[block_id].visible:
			re_used += 1
	if re_used == 0:
		_fail("re-anchor left nothing visible")
		return

	# And putting the tool away hides every node.
	wand.cancel_preview()
	for block_id in wand._ghost_nodes.keys():
		if wand._ghost_nodes[block_id].visible:
			_fail("cancel_preview left block %d visible" % block_id)
			return
	print("PROBE PASS: preview draws real blocks, re-anchors on the same nodes, hides on cancel")
	quit(0)
