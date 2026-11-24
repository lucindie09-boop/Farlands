extends Node
## Stub for the goblin spawn probe: one aim raycast, the two light calls and a body with
## no world to move through. The tool reads `success`, `position` and `place_position`
## from the ray, pushes its material through `apply_item_lighting`, and asks `get_light_at`
## for the cell a goblin's middle is in.

const CELL_LIGHT := Vector4(0.25, 0.5, 0.125, 0.875)

var calls := 0
var light_calls := 0
var lighting_pushes := 0
## Cleared by the probe to check the miss path.
var hit := true


func raycast_from_camera(_max_distance: float) -> Dictionary:
	calls += 1
	if not hit:
		return {"success": false}
	# Aimed at the top face of a block at (10, 63, -6): a block would go into the
	# cell above it.
	return {
		"success": true,
		"position": Vector3(10, 63, -6),
		"place_position": Vector3(10, 64, -6),
	}


func apply_item_lighting(material: ShaderMaterial) -> void:
	lighting_pushes += 1
	# The real one hands the world's uniforms over; the probe only needs to know the
	# tool asked for them on this material.
	if material != null:
		material.set_shader_parameter("alpha_scissor", 0.0)


func get_light_at(_world_x: int, _world_y: int, _world_z: int) -> Vector4:
	light_calls += 1
	return CELL_LIGHT


## A body with no resolver: its shove, its hurt window and its tick are real, its
## movement through terrain is not (that is probes/probe_goblin_roam.gd's job, over a
## real world). `make_mob_body` is what the goblin asks for at `_ready`.
func make_mob_body() -> RefCounted:
	return MobBody.new()


## The liquid animator is an autoload that finds this node by the world's own name and
## asks for the layer it animates; a stub has no texture array, and "no such layer" is
## exactly what it answers to that.
func get_texture_layer_info(_texture_name: String) -> Dictionary:
	return {}
