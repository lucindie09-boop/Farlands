extends SceneTree
## Headless check for the size a dropped item is drawn and solved as.
##
## An item that has left the player's hand is not the size of the block it came
## from. Every dropped item is BASE_SCALE (dropped_items.gd) of the shape it was
## authored as, and this probe throws one of each kind and holds the two halves of
## that contract to the same number:
##
##   * the BODY's boxes: `size` on the item, the shape the native solve is handed;
##   * the MESH as DRAWN: its own bounds times the scale on its node.
##
## Both must be BASE_SCALE of the raw shape, and the mesh's drawn centre must sit
## on the body's origin -- so the thing the player sees and the thing the world
## collides with are one box in one place. A body left at full size fails against
## the mesh; a mesh scaled about the wrong point (a slab's centre is not the
## cell's) shows up in the offset; a sprite still at cell size shows up on the cap.
##
## Run: Godot --headless --path <project> --script res://probes/probe_item_size.gd

const BASE_SCALE := 0.5
const EPS := 0.01

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _same(label: String, got: Vector3, want: Vector3) -> void:
	if absf(got.x - want.x) > EPS or absf(got.y - want.y) > EPS or absf(got.z - want.z) > EPS:
		_fail("%s is %s, expected %s" % [label, got, want])


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	for i in range(5):
		await process_frame
	var dropped: Node = main.get_node_or_null("DroppedItems")
	var cm: Node = main.get_node_or_null("ChunkManager")
	if dropped == null or cm == null:
		_fail("main scene is missing DroppedItems or ChunkManager")
		quit(1)
		return

	# A full cube: the headline number -- a dropped block is half a block.
	_check(dropped, cm, "stone", "block")
	# Shaped blocks: the scale is of the SHAPE, so a slab drops as a half-size slab
	# (1 x 0.5 x 1 -> 0.5 x 0.25 x 0.5) and a stair as its own two boxes at that
	# scale, not as a shrunk cube.
	_check(dropped, cm, "oak_slab", "block")
	_check(dropped, cm, "oak_stairs", "block")
	# Sprites: the body is the silhouette the mesh draws, at the same scale.
	_check(dropped, cm, "stick", "item")
	_check(dropped, cm, "torch", "item")

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)


func _check(dropped: Node, cm: Node, name: String, kind: String) -> void:
	var id := BlockTextures.get_block_id_by_name(name)
	if id <= 0:
		_fail("%s did not resolve by name" % name)
		return
	dropped.spawn(id, 1, Vector3(0.0, 60.0, 0.0), Vector3(0.0, 0.0, 1.0))
	var item: Dictionary = dropped._items[dropped._items.size() - 1]
	var body: Vector3 = item["size"]
	var node: Node3D = item["node"]
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance == null or mesh_instance.mesh == null:
		_fail("%s: the drop has no mesh to measure" % name)
		return
	var aabb: AABB = mesh_instance.mesh.get_aabb()
	var drawn: Vector3 = aabb.size * mesh_instance.scale
	var offset: Vector3 = mesh_instance.position + aabb.get_center() * mesh_instance.scale
	print("probe: %-12s body %s drawn %s scale %s" % [name, body, drawn, mesh_instance.scale])

	# One scale on the mesh, every axis.
	_same("%s: mesh scale" % name, mesh_instance.scale, Vector3.ONE * BASE_SCALE)
	# The mesh and the body are the same size: what is seen is what is solved.
	_same("%s: body against the drawn mesh" % name, body, drawn)
	# ...and the same box in the same place: the mesh is drawn about the body's own
	# origin, not beside it.
	if offset.length() > EPS:
		_fail("%s: drawn mesh centre is %.4f off the body origin (%s)" % [name, offset.length(), offset])
	# Nothing is cell-sized any more. A sprite's own bounds are its silhouette, but
	# nothing may reach the full cell it was authored in.
	var body_big := maxf(body.x, maxf(body.y, body.z))
	var drawn_big := maxf(drawn.x, maxf(drawn.y, drawn.z))
	if body_big > BASE_SCALE + EPS or drawn_big > BASE_SCALE + EPS:
		_fail("%s: still too big (body %s drawn %s, cap %f)" % [name, body, drawn, BASE_SCALE])
	# A block's body must be BASE_SCALE of the shape the registry describes.
	if kind == "block":
		var lo := Vector3(1.0e9, 1.0e9, 1.0e9)
		var hi := Vector3(-1.0e9, -1.0e9, -1.0e9)
		for b in cm.get_selection_boxes(id):
			lo = lo.min(Vector3(b[0], b[1], b[2]))
			hi = hi.max(Vector3(b[3], b[4], b[5]))
		_same("%s: body against the shape's own boxes" % name, body, (hi - lo) * BASE_SCALE)
