extends Node
## The crowd's mutual push, solved once per tick for every registered body.
##
## Bodies are boxes, and two boxes in one place is not something the reference allows: it pushes a
## pair apart on the axis their centres are furthest apart, by at most 0.05 a tick
## (`Entity::push`), and the world's own collision does the rest. The arithmetic is
## `src/engine/entity_push.cpp`, and `tests/test_entity_push.cpp` pins the batch against the
## per-pair form.
##
## WHAT THIS REPLACED is the reason it exists. Each goblin used to do the whole solve itself, once
## a tick: a walk of `get_nodes_in_group("goblin")`, an instance-id comparison per pair and a
## `Bodies.push_pair` call that built a Dictionary — at a hundred goblins that is 10,000 group
## visits and 5,000 native calls per tick, 200,000 and 100,000 per second, from scripts, on the
## main thread. The dropped items' solver already had the answer (`ChunkManager.solve_item_bodies`:
## gather every body into flat arrays, one call per substep), and this is the same shape for mobs.
##
## The player is an entry like any other: `scripts/goblin.gd`'s `_player()`, put LAST in the array,
## so the last delta is theirs and is pushed once instead of once per goblin leaning on them.
##
## ONCE PER PHYSICS FRAME, not once per call. Every registered body ticks on the same frames (they
## all advance on the same delta), so the first one to tick solves the crowd and the rest of that
## frame's calls are no-ops. The frame is the guard rather than the tick number because a body
## spawned later has its own count — two groups with different numbers would each think they were
## first and solve the crowd twice on the same tick.

## The sim's own body box, the same one `goblin.gd` moves and `Bodies` tests.
const BODY := Vector3(0.6, 1.8, 0.6)

## Every registered body sim, in registration order (the player is not in here: they are appended
## per solve, so a player who is respawned is simply found again).
var _bodies: Array = []
## The physics frame the crowd was last solved on.
var _last_frame := -1


func _ready() -> void:
	_last_frame = -1


## A goblin's `MobBody`. Registration is idempotent: a body that is already here is not added twice.
func register(sim: RefCounted) -> void:
	if sim == null or _bodies.has(sim):
		return
	_bodies.append(sim)


## Called from the goblin's `_exit_tree`: a dead body's sim must not be solved, or pushed, after
## its node is gone.
func unregister(sim: RefCounted) -> void:
	_bodies.erase(sim)


## Solves every registered body against every other, plus the player, once per tick.
func push_tick(_tick: int) -> void:
	if _bodies.is_empty():
		return
	if not ClassDB.class_exists("Bodies") or not ClassDB.class_has_method("Bodies", "push_pairs"):
		return
	var frame := Engine.get_physics_frames()
	if frame == _last_frame:
		return
	_last_frame = frame

	var positions := PackedVector3Array()
	var sims: Array = []
	for sim in _bodies:
		if sim == null:
			continue
		positions.push_back(sim.get_tick_position())
		sims.append(sim)
	# Bodies that went away without unregistering (a freed node) are dropped here as well, so the
	# list cannot grow across a session of spawns and kills.
	if sims.size() != _bodies.size():
		_bodies = sims.duplicate()
	if sims.is_empty():
		return

	var player := _player()
	var has_player := player != null and player.has_method("push")
	if has_player:
		positions.push_back(player.global_position)
	var sizes := PackedVector3Array()
	sizes.resize(positions.size())
	for i in positions.size():
		sizes[i] = BODY

	var deltas: PackedVector3Array = Bodies.push_pairs(positions, sizes)
	if deltas.size() != positions.size():
		return
	for i in sims.size():
		sims[i].push(deltas[i])
	if has_player:
		player.call("push", deltas[deltas.size() - 1])


## The player, if this world has one (a preview or a probe may not). Found per solve rather than
## cached, because a scene rebuild replaces the node and a cached reference would be a dead one.
func _player() -> Node3D:
	var scene := get_tree().current_scene
	if scene == null:
		return null
	return scene.get_node_or_null("Player") as Node3D
