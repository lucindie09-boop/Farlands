extends Node3D
## The chips a broken block throws.
##
## The break itself is engine-side (godot_bindings/player_controller_interact.cpp)
## and used to have no visible aftermath at all: the crack overlay vanished and
## the block was simply gone. This is what puts something in the air at that
## instant. It listens for the player's `block_broken` signal and spawns one short
## burst of tiny cubes at the cell, wearing the block's OWN side texture on every
## face -- a broken stair sheds stair, not generic dust -- which is the whole
## reason the signal carries the block id rather than just the position.
##
## The chips are lit by the cell they came out of, through the same light model as
## every other object in the world -- they draw with the ITEM shader
## (shaders/item_shader.gdshader), which is that model's one home, rather than a
## second copy of it. Debris that glows in a coal-dark cave is worse than no
## debris at all, and the project has exactly one answer to "what light is this
## texel in". That light is an INSTANCE uniform, set once per burst, because a
## burst is one cell: every chip in it shares the block's light.
##
## Nothing is pooled: a burst is a one-shot CPUParticles3D that frees itself on its
## own `finished`, and MAX_BURSTS is what keeps a chain of breaks from stacking
## emitters without bound.

## How many chips one block sheds. Enough to read as a block coming apart, few
## enough that a wall coming down is still background motion.
const CHIP_COUNT := 16
## Seconds a chip lives: long enough to see where it went, short enough to be gone
## before the player looks back at the hole.
const CHIP_LIFETIME := 0.5
## The cube a chip is drawn as, in blocks -- a splinter, not a mini-block.
const CHIP_SIZE := 0.08
## Emitters allowed alive at once. Past this the OLDEST is dropped: the break that
## matters is the one that just happened.
const MAX_BURSTS := 16
## One shader for every chip, the item shader the rest of the world's objects
## use; each block's material below is only its texture.
const CHIP_SHADER: Shader = preload("res://shaders/item_shader.gdshader")

var _player: Node = null
var _chunk_manager: Node = null
var _chip_mesh: BoxMesh
## block id -> its ShaderMaterial, or null where the block has no texture to wear.
var _materials := {}
## Live emitters, oldest first, so the cap knows what to drop.
var _bursts: Array[CPUParticles3D] = []


func _ready() -> void:
	_player = get_node_or_null("/root/Main/Player")
	_chunk_manager = get_node_or_null("/root/Main/ChunkManager")

	_chip_mesh = BoxMesh.new()
	_chip_mesh.size = Vector3.ONE * CHIP_SIZE

	# The player may not be up yet on a scene reload; the signal is the whole
	# reason this node exists, so a missing one is reported rather than swallowed.
	if _player != null and _player.has_signal("block_broken"):
		_player.block_broken.connect(_on_block_broken)
	else:
		push_warning("BlockBreakParticles: no Player.block_broken signal to listen to")


## One block's worth of debris, at `block` (its integer cell) in `block_id`'s
## texture. Public so a probe can ask for a burst without mining anything.
func burst(block_id: int, block: Vector3i) -> void:
	var material := _material_for(block_id)
	if material == null:
		return  # no texture to wear: air, a hidden block, or a pack that lost the tile

	# The world's light changes with the hour and the player's own glow moves with
	# them, so the material is refreshed per burst instead of once at load.
	if _chunk_manager != null:
		_chunk_manager.apply_item_lighting(material)

	var chips := CPUParticles3D.new()
	chips.mesh = _chip_mesh
	chips.material_override = material
	chips.amount = CHIP_COUNT
	chips.lifetime = CHIP_LIFETIME
	chips.one_shot = true
	chips.explosiveness = 1.0  # every chip leaves on the frame of the break
	chips.local_coords = false  # chips stay where they land, even as the emitter ages
	chips.emitting = false

	# Out of the block's own volume, upward and sideways: a pile that sat still and
	# then fell would read as crumbling, this reads as breaking.
	chips.emission_shape = CPUParticles3D.EMISSION_SHAPE_BOX
	chips.emission_box_extents = Vector3(0.45, 0.45, 0.45)
	chips.direction = Vector3.UP
	chips.spread = 85.0
	chips.initial_velocity_min = 1.4
	chips.initial_velocity_max = 2.6
	chips.gravity = Vector3(0.0, -18.0, 0.0)
	chips.damping_min = 0.6
	chips.damping_max = 1.6
	chips.particle_flag_rotate_y = true  # no two chips land on the same face

	# Shrink out of sight rather than blinking: at this size a hard pop is the one
	# thing the eye catches, and an opaque chip needs no alpha blend to fade.
	var shrink := Curve.new()
	shrink.add_point(Vector2(0.0, 1.0))
	shrink.add_point(Vector2(0.7, 1.0))
	shrink.add_point(Vector2(1.0, 0.05))
	chips.scale_amount_curve = shrink
	chips.scale_amount_min = 0.6
	chips.scale_amount_max = 1.1

	chips.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	chips.position = Vector3(block) + Vector3(0.5, 0.5, 0.5)
	if _chunk_manager != null:
		# One cell, one light: every chip in the burst carries the light of the
		# block that broke (see the header).
		chips.set_instance_shader_parameter(
			"item_light", _chunk_manager.get_light_at(block.x, block.y, block.z))

	add_child(chips)
	chips.finished.connect(_on_burst_finished.bind(chips))
	_bursts.append(chips)
	if _bursts.size() > MAX_BURSTS:
		var oldest: CPUParticles3D = _bursts.pop_front()
		if is_instance_valid(oldest):
			oldest.queue_free()
	chips.emitting = true


func _on_block_broken(block_id: int, block: Vector3i) -> void:
	burst(block_id, block)


func _on_burst_finished(chips: CPUParticles3D) -> void:
	_bursts.erase(chips)
	if is_instance_valid(chips):
		chips.queue_free()


## `block_id`'s material, built on first use and cached: a burst wears the block's
## side texture, the face a placed block shows the most of. Null where the block
## has none, so nothing is drawn with an untextured chip.
func _material_for(block_id: int) -> ShaderMaterial:
	if _materials.has(block_id):
		return _materials[block_id]

	var texture: Texture2D = BlockTextures.get_texture(block_id)
	var material: ShaderMaterial = null
	if texture != null:
		material = ShaderMaterial.new()
		material.shader = CHIP_SHADER
		material.set_shader_parameter("albedo_texture", texture)
	_materials[block_id] = material
	return material
