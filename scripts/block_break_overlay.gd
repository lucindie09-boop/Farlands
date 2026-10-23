extends Node3D
## The ten-stage crack overlay on the block being mined.
##
## It used to be ONE cube, scaled a hair over the cell, which is right only for a
## full block: on a slab, stair, wall or pole the cracks hung in the air where the
## block is not, and were missing from the parts that stick out. It now draws one
## cube per SELECTION BOX of the block's resolved shape -- the same boxes the
## outline and the collision use (`ChunkManager.get_selection_boxes_at`) -- so
## what cracks is exactly what the player sees and can hit.
##
## The boxes are resolved against the world rather than read from the block alone,
## because a fence post on its own is one box and inside a run it has arms: the set
## can change without the block id changing. They are compared each frame and the
## parts rebuilt only when the set really differs.

@onready var player_controller = get_node("/root/Main/Player")
@onready var chunk_manager = get_node("/root/Main/ChunkManager")

const CRACK_DIR := "res://textures/animated/"
const CRACK_COUNT := 10
## A hair over the box on every axis, so a crack face sits just in front of the
## block's own face instead of z-fighting with it.
const FACE_LIFT := 1.002

var _material: StandardMaterial3D
var _cube: ArrayMesh
## One MeshInstance3D per selection box of the block being mined.
var _parts: Array[MeshInstance3D] = []
var _crack_textures: Array = []
## What the parts were built for, so an unchanged target rebuilds nothing.
var _built := false
var _cell := Vector3i.ZERO
var _block_id := -1
var _boxes: Array = []


func _ready() -> void:
	_material = StandardMaterial3D.new()
	_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_material.cull_mode = BaseMaterial3D.CULL_BACK
	_material.render_priority = 12
	# 16x16 crack textures stay crisp when scaled up over a block face.
	_material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_cube = _build_cube_mesh()
	for i in CRACK_COUNT:
		_crack_textures.append(load(CRACK_DIR + "l0_sprite_%02d.png" % (i + 1)))


func _process(_delta) -> void:
	if not player_controller or not chunk_manager:
		_set_visible(false)
		return
	var state: Dictionary = player_controller.get_break_state()
	if not state.get("active", false):
		_set_visible(false)
		return

	var cell := Vector3i(int(state.get("x", 0)), int(state.get("y", 0)), int(state.get("z", 0)))
	var block_id: int = chunk_manager.get_block(cell.x, cell.y, cell.z)
	var boxes: Array = chunk_manager.get_selection_boxes_at(block_id, cell.x, cell.y, cell.z)
	if not _built or cell != _cell or block_id != _block_id or not _same_boxes(boxes):
		_rebuild(cell, block_id, boxes)

	var stage: int = int(state.get("stage", 0))
	if stage >= 0 and stage < _crack_textures.size():
		_material.albedo_texture = _crack_textures[stage]
	_set_visible(true)


## Rebuilds the parts for `cell`'s resolved `boxes`: one unit cube per box, scaled
## to the box and centred on it. Boxes are local to the cell (0..1), the form every
## shape registry path produces.
func _rebuild(cell: Vector3i, block_id: int, boxes: Array) -> void:
	_cell = cell
	_block_id = block_id
	_boxes = boxes.duplicate()
	_built = true
	for part in _parts:
		part.queue_free()
	_parts.clear()

	for box in boxes:
		var b: PackedFloat32Array = box
		if b.size() < 6:
			continue
		var lo := Vector3(b[0], b[1], b[2])
		var hi := Vector3(b[3], b[4], b[5])
		var size := hi - lo
		if size.x <= 0.0 or size.y <= 0.0 or size.z <= 0.0:
			continue
		var part := MeshInstance3D.new()
		part.mesh = _cube
		part.material_override = _material
		part.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		# The shared cube is a unit block centred on its origin, so a box is a
		# scale and the box's own centre -- no per-shape geometry to build.
		part.scale = size * FACE_LIFT
		part.position = Vector3(cell) + (lo + hi) * 0.5
		add_child(part)
		_parts.append(part)


## Whether `boxes` is the set the parts were built for. Packed arrays compare by
## value, so this is a size walk rather than an identity test.
func _same_boxes(boxes: Array) -> bool:
	if boxes.size() != _boxes.size():
		return false
	for i in boxes.size():
		var a: PackedFloat32Array = boxes[i]
		var b: PackedFloat32Array = _boxes[i]
		if a.size() != b.size():
			return false
		for j in a.size():
			if not is_equal_approx(a[j], b[j]):
				return false
	return true


func _set_visible(on: bool) -> void:
	for part in _parts:
		part.visible = on


func _build_cube_mesh() -> ArrayMesh:
	# Shared cube geometry (C++ ViewmodelMeshes binding): texture-top = world-top
	# on every face, so the crack pattern is centered and oriented identically
	# across all six faces (a BoxMesh rotates the top/bottom UVs).
	var data := ViewmodelMeshes.build_cube_mesh()
	if data.is_empty():
		return ArrayMesh.new()
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)

	arrays[Mesh.ARRAY_VERTEX] = data["verts"]
	arrays[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arrays[Mesh.ARRAY_NORMAL] = data["normals"]
	arrays[Mesh.ARRAY_INDEX] = data["indices"]

	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh
