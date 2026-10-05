extends SceneTree
## Headless check for the size a dropped item is drawn and solved as.
##
## An item that has left the player's hand is not the size of the block it came
## from, and a pile of more than one is not the size of a single item. Every drop is
## BASE_SCALE (dropped_items.gd) of the shape it was authored as, and a stack of
## more than one is that times a capped, sublinear factor -- 1 + 0.25*log2(count) up
## to twice the size. The factor goes on the BODY as well as on the mesh, so what a
## pile collides as is exactly what it is drawn as. This probe throws one of each
## kind and then one of each count, and holds the contract to the same numbers:
##
##   * the BODY's boxes: `size` on the item, the shape the native solve is handed.
##     It must be BASE_SCALE times the count's own factor of the shape's raw boxes;
##   * the MESH as DRAWN: its own bounds times the scale on its node, which must be
##     the same size as the body;
##   * agreement between the two: the mesh's centre sits on the body's origin, so
##     the drawn thing and the solved thing are one box in one place -- a stacked
##     pile does not hover over what it should touch or sink into the floor it
##     stands on.
##
## A body left at the single item's size fails against the mesh; a mesh scaled
## about the wrong point (a slab's centre is not the cell's) shows up in the offset;
## a sprite still at cell size, or a pile that ignores its count, shows up on the
## cap and the table.
##
## Run: Godot --headless --path <project> --script res://probes/probe_item_size.gd

const BASE_SCALE := 0.5
const EPS := 0.01

## The curve's cap, and count -> the size a pile is drawn AND solved at, as a
## multiple of the single item's own body. The numbers the curve promises,
## hardcoded: 1 + 0.25*log2(count), capped at twice the size from sixteen items up.
const MERGE_SCALE_MAX := 2.0
const MERGE_TABLE := [[1, 1.0], [2, 1.25], [4, 1.5], [8, 1.75], [16, 2.0], [64, 2.0]]

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

	# --- a pile is drawn and solved bigger by its count ------------------------
	for case in MERGE_TABLE:
		_check_merge(dropped, case[0], case[1])

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


## One stack count: the solved body and the drawn mesh are one box at the count's
## own factor of a single item's size -- a pile that reads bigger to the eye is
## bigger to the world, and only by the curve's capped factor.
func _check_merge(dropped: Node, count: int, want: float) -> void:
	var id := BlockTextures.get_block_id_by_name("stone")
	dropped.spawn(id, count, Vector3(0.0, 60.0, 0.0), Vector3(0.0, 0.0, 1.0))
	var item: Dictionary = dropped._items[dropped._items.size() - 1]
	var body: Vector3 = item["size"]
	var node: Node3D = item["node"]
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance == null or mesh_instance.mesh == null:
		_fail("stone x%d: the drop has no mesh to measure" % count)
		return
	var aabb: AABB = mesh_instance.mesh.get_aabb()
	var drawn: Vector3 = aabb.size * mesh_instance.scale
	var offset: Vector3 = mesh_instance.position + aabb.get_center() * mesh_instance.scale
	print("probe: stone x%-3d body %s drawn %s scale %s (x%.2f)"
		% [count, body, drawn, mesh_instance.scale, want])

	# The BODY has the pile's size: sixty-four items are solved as twice a single
	# item, so a big pile rests on and against the world at the size it is drawn.
	_same("stone x%d: body size" % count, body, Vector3.ONE * (BASE_SCALE * want))
	# The mesh and the body are one box: what is seen is what is solved.
	_same("stone x%d: body against the drawn mesh" % count, body, drawn)
	if offset.length() > EPS:
		_fail("stone x%d: drawn mesh centre is %.4f off the body origin (%s)" % [count, offset.length(), offset])
	# ...and never past the curve's cap: a pile is at most MERGE_SCALE_MAX of the
	# single item, and the cap is what keeps it from becoming bigger than the shape
	# it came from.
	var body_big := maxf(body.x, maxf(body.y, body.z))
	if body_big > BASE_SCALE * MERGE_SCALE_MAX + EPS:
		_fail("stone x%d: body is %s, past the %fx cap" % [count, body, MERGE_SCALE_MAX])
