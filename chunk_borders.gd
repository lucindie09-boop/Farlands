extends Node
## Debug helper: press the bound action (toggle_chunk_borders, default B) to
## show the chunk grid around the player as world-space lines — the 32-block
## column boundaries as verticals, plus the floor and ceiling of the chunk slice
## the player is standing in as a grid. Press again to hide.
##
## Two decisions worth knowing before changing it:
##
##  * The lines are drawn with depth testing OFF. The question this overlay
##    answers is "where is the boundary", and a border buried in rock, under a
##    liquid, or inside a hillside answers nothing — the seams that go wrong are
##    exactly the ones you cannot see the boundary of. So it reads through
##    terrain on purpose.
##  * Chunk geometry is rebuilt only when the player's CHUNK changes, not every
##    frame. The grid is world-anchored, so walking within one chunk cannot move
##    a line, and the vertical span is measured from the chunk slice rather than
##    from the player's feet for the same reason.
##
## The player's own chunk is drawn brighter than the rest, so "which chunk is
## this seam the edge of" is never a guess.

const TOGGLE_ACTION := "toggle_chunk_borders"

# Chunk dimensions — src/core/chunk_coords.hpp (CHUNK_WIDTH / CHUNK_HEIGHT /
# CHUNK_DEPTH, all 32). The engine owns this constant; this is the GDScript side
# of it, and the only place to change if the engine's does.
const CHUNK_SIZE := 32
# Chunks drawn each way from the player's own: 2 gives a 5x5 chunk grid.
const RADIUS_CHUNKS := 2
# How far above and below the player's chunk slice the verticals run.
const VERTICAL_SPAN := 64.0

const COLOR_GRID := Color(0.35, 0.85, 1.0, 0.42)
const COLOR_PLAYER_CHUNK := Color(1.0, 0.8, 0.15, 0.95)

# An impossible chunk, so the first build always happens.
const NO_CHUNK := Vector3i(0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF)

var _enabled := false
var _mesh_instance: MeshInstance3D = null
var _mesh := ImmediateMesh.new()
var _material: StandardMaterial3D = null
var _built_chunk := NO_CHUNK

func _ready() -> void:
	_material = StandardMaterial3D.new()
	_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_material.vertex_color_use_as_albedo = true
	_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	# See the header: an overlay that hides inside terrain cannot show a seam.
	_material.no_depth_test = true
	_material.disable_receive_shadows = true

func _unhandled_input(event: InputEvent) -> void:
	if not event.is_action_pressed(TOGGLE_ACTION) or event.is_echo():
		return
	# Gameplay only, like the other debug keys: with the mouse released (chat,
	# inventory, a menu) the key belongs to whatever is open.
	if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	set_enabled(not _enabled)

func _process(_delta: float) -> void:
	if not _enabled:
		return
	var player := _player()
	if player == null:
		return
	var chunk := _chunk_of(player.global_position)
	if chunk == _built_chunk:
		return
	_build(chunk)

## Toggle from code as well as from the key (the probe drives this).
func set_enabled(on: bool) -> void:
	_enabled = on
	if not on:
		if _mesh_instance != null and is_instance_valid(_mesh_instance):
			_mesh_instance.visible = false
		# Re-enabling redraws even if the player has not moved: a scene reload
		# and a chunk-size change both land here.
		_built_chunk = NO_CHUNK
		return
	_ensure_instance()
	if _mesh_instance != null:
		_apply_material()
		_mesh_instance.visible = true
		_built_chunk = NO_CHUNK  # force a rebuild on the next frame

func is_enabled() -> bool:
	return _enabled

## The chunk the overlay is (or would be) centred on, in chunk coordinates.
func get_built_chunk() -> Vector3i:
	return _built_chunk

func get_mesh_instance() -> MeshInstance3D:
	return _mesh_instance

# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

func _build(chunk: Vector3i) -> void:
	_built_chunk = chunk
	_ensure_instance()
	if _mesh_instance == null:
		return

	var x_lo := chunk.x - RADIUS_CHUNKS
	var x_hi := chunk.x + RADIUS_CHUNKS
	var z_lo := chunk.z - RADIUS_CHUNKS
	var z_hi := chunk.z + RADIUS_CHUNKS
	var y_lo := float(chunk.y * CHUNK_SIZE) - VERTICAL_SPAN
	var y_hi := float((chunk.y + 1) * CHUNK_SIZE) + VERTICAL_SPAN

	_mesh.clear_surfaces()
	_mesh.surface_begin(Mesh.PRIMITIVE_LINES, _material)

	# Verticals at every grid intersection, i.e. at every chunk corner. The four
	# around the player's own chunk are the bright colour: they are the two
	# boundaries (x and z) a seam in front of the player actually lies on.
	for gx in range(x_lo, x_hi + 2):
		for gz in range(z_lo, z_hi + 2):
			var own := gx >= chunk.x and gx <= chunk.x + 1 and gz >= chunk.z and gz <= chunk.z + 1
			var color := COLOR_PLAYER_CHUNK if own else COLOR_GRID
			var x := float(gx * CHUNK_SIZE)
			var z := float(gz * CHUNK_SIZE)
			_line(Vector3(x, y_lo, z), Vector3(x, y_hi, z), color)

	# The floor and ceiling of the player's chunk slice, one square per chunk.
	# Both planes are what makes the 32-block height of a chunk readable; only
	# their corners meet the verticals.
	var floor_y := chunk.y * CHUNK_SIZE
	var ceil_y := (chunk.y + 1) * CHUNK_SIZE
	for cx in range(x_lo, x_hi + 1):
		for cz in range(z_lo, z_hi + 1):
			var color := COLOR_PLAYER_CHUNK if (cx == chunk.x and cz == chunk.z) else COLOR_GRID
			_square(cx, floor_y, cz, color)
			_square(cx, ceil_y, cz, color)

	_mesh.surface_end()
	_mesh_instance.visible = true

func _square(cx: int, plane_y: int, cz: int, color: Color) -> void:
	var x := float(cx * CHUNK_SIZE)
	var z := float(cz * CHUNK_SIZE)
	var y := float(plane_y)
	var p00 := Vector3(x, y, z)
	var p10 := Vector3(x + CHUNK_SIZE, y, z)
	var p11 := Vector3(x + CHUNK_SIZE, y, z + CHUNK_SIZE)
	var p01 := Vector3(x, y, z + CHUNK_SIZE)
	_line(p00, p10, color)
	_line(p10, p11, color)
	_line(p11, p01, color)
	_line(p01, p00, color)

func _line(a: Vector3, b: Vector3, color: Color) -> void:
	# ImmediateMesh takes a colour per vertex; both ends carry the same one.
	_mesh.surface_set_color(color)
	_mesh.surface_add_vertex(a)
	_mesh.surface_set_color(color)
	_mesh.surface_add_vertex(b)

# ---------------------------------------------------------------------------
# Scene plumbing
# ---------------------------------------------------------------------------

func _ensure_instance() -> void:
	var scene_root := get_tree().current_scene if is_inside_tree() else null
	if scene_root == null:
		return
	if _mesh_instance != null and is_instance_valid(_mesh_instance) and \
			_mesh_instance.get_parent() == scene_root:
		return
	if _mesh_instance != null and is_instance_valid(_mesh_instance):
		_mesh_instance.queue_free()
	_mesh_instance = MeshInstance3D.new()
	_mesh_instance.name = "ChunkBorders"
	_mesh_instance.mesh = _mesh
	_mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_mesh_instance.visible = false
	_apply_material()
	scene_root.add_child(_mesh_instance)

func _apply_material() -> void:
	if _mesh_instance != null and is_instance_valid(_mesh_instance):
		_mesh_instance.material_override = _material

func _player() -> Node3D:
	var scene_root := get_tree().current_scene if is_inside_tree() else null
	if scene_root == null:
		return null
	return scene_root.get_node_or_null("Player") as Node3D

func _chunk_of(world_position: Vector3) -> Vector3i:
	return Vector3i(
		floori(world_position.x / CHUNK_SIZE),
		floori(world_position.y / CHUNK_SIZE),
		floori(world_position.z / CHUNK_SIZE))
