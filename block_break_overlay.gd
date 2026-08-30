extends Node3D

@onready var player_controller = get_node("/root/Main/Player")
@onready var chunk_manager = get_node("/root/Main/ChunkManager")

const CRACK_DIR := "res://textures/animated/"
const CRACK_COUNT := 10

var _mesh: MeshInstance3D
var _material: StandardMaterial3D
var _crack_textures: Array = []

func _ready():
	_material = StandardMaterial3D.new()
	_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_material.cull_mode = BaseMaterial3D.CULL_BACK
	_material.render_priority = 12
	# 16x16 crack textures stay crisp when scaled up over a block face.
	_material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_mesh = MeshInstance3D.new()
	_mesh.mesh = _build_cube_mesh()
	# Uniform scale keeps the overlay just in front of the block surface so
	# exposed faces draw over it without z-fighting; faces pressed against
	# solid neighbors are depth-occluded.
	_mesh.scale = Vector3(1.002, 1.002, 1.002)
	_mesh.material_override = _material
	_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_mesh.visible = false
	add_child(_mesh)
	for i in CRACK_COUNT:
		_crack_textures.append(load(CRACK_DIR + "l0_sprite_%02d.png" % (i + 1)))

func _process(_delta):
	if not player_controller or not chunk_manager:
		_mesh.visible = false
		return
	var state: Dictionary = player_controller.get_break_state()
	if not state.get("active", false):
		_mesh.visible = false
		return
	_mesh.position = Vector3(state.get("x", 0.0), state.get("y", 0.0), state.get("z", 0.0)) + Vector3(0.5, 0.5, 0.5)
	var stage: int = int(state.get("stage", 0))
	if stage >= 0 and stage < _crack_textures.size():
		_material.albedo_texture = _crack_textures[stage]
	_mesh.visible = true

func _build_cube_mesh() -> ArrayMesh:
	# Same layout as the Block Maker's cube: texture-top = world-top on every
	# face, so the crack pattern is centered and oriented identically across
	# all six faces (a BoxMesh rotates the top/bottom UVs).
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)

	var verts = PackedVector3Array()
	var uvs = PackedVector2Array()
	var normals = PackedVector3Array()
	var indices = PackedInt32Array()

	# +X face (right)
	verts.append(Vector3(0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, -0.5, -0.5))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	var base := 0
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# -X face (left)
	verts.append(Vector3(-0.5, -0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	verts.append(Vector3(-0.5, -0.5, 0.5))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 4
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# +Y face (top)
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 8
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# -Y face (bottom)
	verts.append(Vector3(-0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, -0.5))
	verts.append(Vector3(-0.5, -0.5, -0.5))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 12
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# +Z face (front)
	verts.append(Vector3(-0.5, -0.5, 0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, 0.5))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 16
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# -Z face (back)
	verts.append(Vector3(0.5, -0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, -0.5, -0.5))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 20
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh