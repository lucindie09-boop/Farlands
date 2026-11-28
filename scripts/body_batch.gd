extends Node3D
## One MultiMesh per rig part, drawn for every body of that rig at once.
##
## Forward+ DOES instance MeshInstance3D nodes that share a mesh and a material, and that is
## what makes a crowd of goblins cheap at all -- but only up to a batch's own limit. Past it the
## renderer starts another draw, and a hundred goblins measure ~30 draws instead of six, with no
## project setting to raise it (probes/probe_goblin_drawcalls.gd counts it: ten bodies are one
## draw per part, a hundred are five). A MultiMesh has no such ladder: one draw per part,
## however many bodies, which is the "flat with the crowd" the body is meant to cost.
##
## What that buys and what it costs:
##
##   - one draw per part, whatever the count, and `visible_instance_count` trims the tail;
##   - NO per-instance frustum culling: the batch is culled as one AABB, so a goblin behind the
##     camera is still submitted (its triangles are depth-rejected, but they are walked);
##   - no instance uniforms: each body's cell light rides the per-instance CUSTOM DATA, read by
##     assets/shaders/body_batch.gdshader (docs/rendering-notes.md). `item_light` is still per
##     body -- a body is lit by the cell it stands in -- it just arrives a different way.
##
## A body registers once and then writes its own six transforms and its own light every frame
## (scripts/goblin_model.gd), so nothing here walks a crowd: this node owns the buffers and the
## slots, not the bodies. It is parented to the root, at identity, because the instance
## transforms are WORLD transforms -- a batch under any node with a transform would draw the
## crowd somewhere else.
##
## Only a MOB's body is batched: the player's own body is one instance, and the crowd is the
## problem.

## Instance slots grown at a time. `instance_count` is a high-water mark that reallocates the
## buffers, so it is grown in steps rather than per body; `visible_instance_count` is what the
## frame actually draws.
const SLOT_CHUNK := 32

## Part name -> the MultiMeshInstance3D drawing it. Empty until the first body registers, and
## then the rig is FIXED: a batch belongs to one rig and one material.
var _parts: Dictionary = {}
var _material: Material = null
## Slots handed out: `_used` is one past the highest, and `_free` is the holes below it.
var _used := 0
var _free: Array[int] = []
var _capacity := 0


## A slot for one body, drawing the rig `meshes` (part name -> ArrayMesh) with `material`.
## The first registration builds the six MultiMeshInstance3Ds; every later one takes a slot.
func register(meshes: Dictionary, material: Material) -> int:
	if _parts.is_empty():
		_material = material
		for part in meshes:
			var multi := MultiMesh.new()
			# ORDER MATTERS: these describe the buffers, and `instance_count` is what allocates
			# them. Set afterwards, they would be ignored.
			multi.transform_format = MultiMesh.TRANSFORM_3D
			# The per-instance light rides this: a MultiMesh has no instance uniforms, so
			# `INSTANCE_CUSTOM` is where a body's cell light goes.
			multi.use_custom_data = true
			multi.mesh = meshes[part]
			var drawn := MultiMeshInstance3D.new()
			drawn.name = String(part)
			drawn.multimesh = multi
			drawn.material_override = material
			# No shadow pass: this project draws the sun's shadow map off, and a crowd's
			# bounds are what its one culling test uses.
			drawn.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			add_child(drawn)
			_parts[part] = drawn
	var slot := _used
	if not _free.is_empty():
		slot = _free.pop_back()
	else:
		_used += 1
		_ensure_capacity(_used)
	_blank(slot)
	return slot


## A body leaving: its slot goes back, and nothing is drawn where it was.
func unregister(slot: int) -> void:
	if slot < 0:
		return
	_blank(slot)
	_free.append(slot)
	while _used > 0 and _free.has(_used - 1):
		_free.erase(_used - 1)
		_used -= 1
	_show()


## One part of one body, this frame: where it is in the world, and the light the body is seen
## in (its eased `item_light` -- rgb block light, a sky light).
func write(slot: int, part: String, where: Transform3D, light: Vector4) -> void:
	var drawn := _parts.get(part) as MultiMeshInstance3D
	if drawn == null:
		return
	var multi: MultiMesh = drawn.multimesh
	if multi == null or slot < 0 or slot >= multi.instance_count:
		return
	multi.set_instance_transform(slot, where)
	multi.set_instance_custom_data(slot, Color(light.x, light.y, light.z, light.w))


## How many slots this batch has drawn, for a probe that wants to know the crowd's size.
func used_slots() -> int:
	return _used


## The instance transform and custom data the frame is drawing for one body's part, read back
## the way a probe checks that the batch really carries the crowd (and its light).
func read_transform(slot: int, part: String) -> Variant:
	var drawn := _parts.get(part) as MultiMeshInstance3D
	if drawn == null or drawn.multimesh == null:
		return null
	return drawn.multimesh.get_instance_transform(slot)


func read_light(slot: int, part: String) -> Variant:
	var drawn := _parts.get(part) as MultiMeshInstance3D
	if drawn == null or drawn.multimesh == null:
		return null
	return drawn.multimesh.get_instance_custom_data(slot)


## Grow every part's buffer to hold `count` slots. Growing reallocates (and the new slots are
## born at the origin, at full size), so the whole range is blanked here: a body's own write
## then puts its parts where they are, and nothing is drawn twice while it does.
func _ensure_capacity(count: int) -> void:
	if count <= _capacity:
		_show()
		return
	var grown := ceili(float(count) / float(SLOT_CHUNK)) * SLOT_CHUNK
	for part in _parts:
		var drawn := _parts[part] as MultiMeshInstance3D
		drawn.multimesh.instance_count = grown
	_capacity = grown
	for slot in range(0, grown):
		_blank(slot)
	_show()


## A slot that draws nothing: a zero-scaled instance at the origin, whatever is left in the
## rest of its data.
func _blank(slot: int) -> void:
	for part in _parts:
		var drawn := _parts[part] as MultiMeshInstance3D
		var multi: MultiMesh = drawn.multimesh
		if slot < multi.instance_count:
			multi.set_instance_transform(slot, Transform3D(Basis().scaled(Vector3.ZERO),
				Vector3.ZERO))
			multi.set_instance_custom_data(slot, Color(0.0, 0.0, 0.0, 0.0))


func _show() -> void:
	for part in _parts:
		var drawn := _parts[part] as MultiMeshInstance3D
		drawn.multimesh.visible_instance_count = _used
