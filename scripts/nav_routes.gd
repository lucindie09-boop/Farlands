extends RefCounted
## The planner's finished routes, for everything that asks for one.
##
## `ChunkManager.poll_paths()` hands back every plan finished since the last call and
## empties the queue as it does, so two callers polling in the same frame take each
## other's results: a strolling goblin would lose its route to the debug overlay, or
## the overlay its route to the goblin. One drain per frame fills this table instead,
## keyed by job id, and each caller takes only its own.

## Finished plans not yet collected, by job id.
static var _finished: Dictionary = {}
## The physics frame the table was last filled on.
static var _drained_frame := -1


## Fill the table from the engine, once per frame however many callers ask. Safe to
## call every tick: a second call in the same frame costs the frame comparison.
static func poll(chunk_manager: Node) -> void:
	var frame := Engine.get_physics_frames()
	if frame == _drained_frame:
		return
	_drained_frame = frame
	for entry in chunk_manager.poll_paths():
		_finished[int(entry.get("id", 0))] = entry
	# A plan whose owner died before collecting it would otherwise sit here for the
	# session. Plans are few and short-lived, so a table this large means the owners
	# are gone rather than that the plans are late.
	if _finished.size() > 64:
		_finished.clear()


## The finished plan for `job`, or an empty dictionary while it is still running.
## Taking one removes it: a route is collected once.
static func take(job: int) -> Dictionary:
	if job == 0:
		return {}
	var result: Dictionary = _finished.get(job, {})
	_finished.erase(job)
	return result
