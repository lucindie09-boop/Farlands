extends MeshInstance3D

## A world-space cloud volume. The cloud mask is generated here as a tileable
## 256x256 alpha texture -- no premade cloud image is used. Each texel covers 12
## blocks, and the mask is extruded four blocks tall. The mask and the extruded
## walls share local cloud space; the node snaps to the 3,072-block tile grid
## and carries the wind offset, so the whole volume drifts together instead of
## the walls sliding with the player.

const CLOUD_SHADER: Shader = preload("res://shaders/clouds.gdshader")
const CLOUD_HEIGHT := 408.0
const CLOUD_SIZE := 6144.0
const CLOUD_SPAN := 3072.0  # one mask tile: 256 texels x 12 blocks
const CLOUD_TILE := 3072.0
const CLOUD_THICKNESS := 4.0
const WIND_SPEED := 1.2
const CLOUD_OPACITY := 0.82

const TEXTURE_SIZE := 256
const MASK_TILES := 2
const MASK_SIZE := TEXTURE_SIZE * MASK_TILES
const CELL_SIZE := 12.0
const BASE_LATTICE := 16
const OCTAVES := 3
const NOISE_SEED := 20261103
const CLOUD_THRESHOLD := 0.54

var _player: Node3D
var _wind_offset := 0.0


func _ready() -> void:
	_player = get_node_or_null("../Player") as Node3D

	var cloud_mask := _generate_cloud_mask()
	mesh = _build_cloud_mesh(cloud_mask)

	var cloud_material := ShaderMaterial.new()
	cloud_material.shader = CLOUD_SHADER
	cloud_material.set_shader_parameter("cloud_texture", _mask_to_texture(cloud_mask))
	cloud_material.set_shader_parameter("cloud_span", CLOUD_SPAN)
	cloud_material.set_shader_parameter("opacity", CLOUD_OPACITY)
	# The water material is priority 1 and writes depth. Drawing the cloud after it
	# lets liquid in front hide the cloud, while liquid behind shows through it.
	cloud_material.render_priority = 2
	material_override = cloud_material

	cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	global_position = Vector3(0.0, CLOUD_HEIGHT, 0.0)
	_place_cloud_tile()


func _process(delta: float) -> void:
	_wind_offset = fmod(_wind_offset + WIND_SPEED * delta, CLOUD_TILE)
	_place_cloud_tile()


func _place_cloud_tile() -> void:
	if not is_instance_valid(_player):
		return
	var player_x := _player.global_position.x
	var player_z := _player.global_position.z
	# Snap to the mask's tile grid, then carry the wind: the player stays inside
	# the volume, and both the mask and the extruded walls move by the same amount.
	var tile_x: float = round((player_x - _wind_offset) / CLOUD_TILE) * CLOUD_TILE
	var tile_z: float = round(player_z / CLOUD_TILE) * CLOUD_TILE
	global_position = Vector3(tile_x + _wind_offset, CLOUD_HEIGHT, tile_z)


func _generate_cloud_mask() -> PackedByteArray:
	var mask := PackedByteArray()
	mask.resize(MASK_SIZE * MASK_SIZE)
	for y in MASK_SIZE:
		for x in MASK_SIZE:
			if _cloud_value(x % TEXTURE_SIZE, y % TEXTURE_SIZE) > CLOUD_THRESHOLD:
				mask[y * MASK_SIZE + x] = 1
	return mask


func _mask_to_texture(mask: PackedByteArray) -> ImageTexture:
	var image := Image.create(TEXTURE_SIZE, TEXTURE_SIZE, false, Image.FORMAT_RGBA8)
	for y in TEXTURE_SIZE:
		for x in TEXTURE_SIZE:
			var cloud := mask[y * MASK_SIZE + x] == 1
			image.set_pixel(x, y, Color(1.0, 1.0, 1.0, 1.0) if cloud else Color(1.0, 1.0, 1.0, 0.0))
	return ImageTexture.create_from_image(image)


func _build_cloud_mesh(mask: PackedByteArray) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var colors := PackedColorArray()
	var normals := PackedVector3Array()
	var indices := PackedInt32Array()
	var half := CLOUD_SIZE * 0.5

	# The top and bottom are the same binary mask, so greedy rectangles cover
	# both. Each covered cell is 12x12 blocks.
	var used := PackedByteArray()
	used.resize(MASK_SIZE * MASK_SIZE)
	for z in MASK_SIZE:
		for x in MASK_SIZE:
			var cell := z * MASK_SIZE + x
			if mask[cell] == 0 or used[cell] == 1:
				continue
			var width := 1
			while x + width < MASK_SIZE:
				var next_cell := cell + width
				if mask[next_cell] == 0 or used[next_cell] == 1:
					break
				width += 1
			var depth := 1
			while z + depth < MASK_SIZE:
				var row := (z + depth) * MASK_SIZE
				var row_free := true
				for xx in range(x, x + width):
					if mask[row + xx] == 0 or used[row + xx] == 1:
						row_free = false
						break
				if not row_free:
					break
				depth += 1
			for zz in range(z, z + depth):
				var row := zz * MASK_SIZE
				for xx in range(x, x + width):
					used[row + xx] = 1

			var x0 := float(x) * CELL_SIZE - half
			var x1 := float(x + width) * CELL_SIZE - half
			var z0 := float(z) * CELL_SIZE - half
			var z1 := float(z + depth) * CELL_SIZE - half
			_add_quad(vertices, colors, normals, indices,
				Vector3(x0, CLOUD_THICKNESS, z0),
				Vector3(x0, CLOUD_THICKNESS, z1),
				Vector3(x1, CLOUD_THICKNESS, z1),
				Vector3(x1, CLOUD_THICKNESS, z0),
				false, 1.0, Vector3.UP)
			_add_quad(vertices, colors, normals, indices,
				Vector3(x0, 0.0, z0),
				Vector3(x0, 0.0, z1),
				Vector3(x1, 0.0, z1),
				Vector3(x1, 0.0, z0),
				false, 0.5, Vector3.DOWN)

	# A side wall where a cloud cell meets empty space. The outer edge of the
	# tile counts as empty: the volume has to close even though the mask tiles.
	for z in MASK_SIZE:
		for x in MASK_SIZE:
			var cell := z * MASK_SIZE + x
			if mask[cell] == 0:
				continue
			var x0 := float(x) * CELL_SIZE - half
			var x1 := float(x + 1) * CELL_SIZE - half
			var z0 := float(z) * CELL_SIZE - half
			var z1 := float(z + 1) * CELL_SIZE - half
			if x == 0 or mask[cell - 1] == 0:
				_add_quad(vertices, colors, normals, indices,
					Vector3(x0, 0.0, z0),
					Vector3(x0, 0.0, z1),
					Vector3(x0, CLOUD_THICKNESS, z1),
					Vector3(x0, CLOUD_THICKNESS, z0),
					true, 0.6, Vector3.LEFT)
			if x == MASK_SIZE - 1 or mask[cell + 1] == 0:
				_add_quad(vertices, colors, normals, indices,
					Vector3(x1, 0.0, z0),
					Vector3(x1, 0.0, z1),
					Vector3(x1, CLOUD_THICKNESS, z1),
					Vector3(x1, CLOUD_THICKNESS, z0),
					true, 0.6, Vector3.RIGHT)
			if z == 0 or mask[cell - MASK_SIZE] == 0:
				_add_quad(vertices, colors, normals, indices,
					Vector3(x0, 0.0, z0),
					Vector3(x1, 0.0, z0),
					Vector3(x1, CLOUD_THICKNESS, z0),
					Vector3(x0, CLOUD_THICKNESS, z0),
					true, 0.8, Vector3.FORWARD)
			if z == MASK_SIZE - 1 or mask[cell + MASK_SIZE] == 0:
				_add_quad(vertices, colors, normals, indices,
					Vector3(x0, 0.0, z1),
					Vector3(x1, 0.0, z1),
					Vector3(x1, CLOUD_THICKNESS, z1),
					Vector3(x0, CLOUD_THICKNESS, z1),
					true, 0.8, Vector3.BACK)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	var cloud_mesh := ArrayMesh.new()
	cloud_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return cloud_mesh


func _add_quad(vertices: PackedVector3Array, colors: PackedColorArray, normals: PackedVector3Array, indices: PackedInt32Array,
		p0: Vector3, p1: Vector3, p2: Vector3, p3: Vector3, side: bool, shade: float, normal: Vector3) -> void:
	var base := vertices.size()
	vertices.push_back(p0)
	vertices.push_back(p1)
	vertices.push_back(p2)
	vertices.push_back(p3)
	var color := Color(1.0 if side else 0.0, shade, 0.0, 1.0)
	for _i in 4:
		colors.push_back(color)
		normals.push_back(normal)
	indices.push_back(base)
	indices.push_back(base + 1)
	indices.push_back(base + 2)
	indices.push_back(base)
	indices.push_back(base + 2)
	indices.push_back(base + 3)


func _cloud_value(px: int, py: int) -> float:
	# Sample a periodic lattice so the texture tiles without a visible seam.
	var u := float(px) / float(TEXTURE_SIZE) * float(BASE_LATTICE)
	var v := float(py) / float(TEXTURE_SIZE) * float(BASE_LATTICE)
	var total := 0.0
	var total_weight := 0.0
	var weight := 0.55
	var period := BASE_LATTICE
	var frequency := 1.0
	for _octave in OCTAVES:
		total += _periodic_noise(u * frequency, v * frequency, period) * weight
		total_weight += weight
		weight *= 0.5
		period *= 2
		frequency *= 2.0
	return total / total_weight


func _periodic_noise(x: float, y: float, period: int) -> float:
	var x0 := floori(x)
	var y0 := floori(y)
	var tx := x - float(x0)
	var ty := y - float(y0)
	tx = tx * tx * (3.0 - 2.0 * tx)
	ty = ty * ty * (3.0 - 2.0 * ty)
	var x1 := x0 + 1
	var y1 := y0 + 1
	var a := _cell(x0, y0, period)
	var b := _cell(x1, y0, period)
	var c := _cell(x0, y1, period)
	var d := _cell(x1, y1, period)
	return lerpf(lerpf(a, b, tx), lerpf(c, d, tx), ty)


func _cell(x: int, y: int, period: int) -> float:
	var h := hash(Vector3i(wrapi(x, 0, period), wrapi(y, 0, period), NOISE_SEED))
	return float(h & 0xffff) / 65535.0
