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
## When each entry arrived, by job id, so an ownerless plan can be aged out rather than
## the table being cleared wholesale.
static var _arrived: Dictionary = {}
## The physics frame the table was last filled on.
static var _drained_frame := -1
## How long a finished plan is kept, in physics frames: two seconds. A live body collects its plan
## on its next tick, so anything older than this has no owner left.
const PLAN_MAX_AGE_FRAMES := 120
## A hard bound, so a session that leaks plans cannot grow the table without end. When it is
## reached the OLDEST entries go first.
const MAX_FINISHED := 512


## Fill the table from the engine, once per frame however many callers ask. Safe to
## call every tick: a second call in the same frame costs the frame comparison.
static func poll(chunk_manager: Node) -> void:
	var frame := Engine.get_physics_frames()
	if frame == _drained_frame:
		return
	_drained_frame = frame
	for entry in chunk_manager.poll_paths():
		var id := int(entry.get("id", 0))
		if id == 0:
			continue
		_finished[id] = entry
		_arrived[id] = frame
	_expire(frame)


## The finished plan for `job`, or an empty dictionary while it is still running.
## Taking one removes it: a route is collected once.
static func take(job: int) -> Dictionary:
	if job == 0:
		return {}
	var result: Dictionary = _finished.get(job, {})
	_finished.erase(job)
	_arrived.erase(job)
	return result


## Drop the entries that have waited too long, oldest first, and stop at the hard bound.
##
## The table used to be CLEARED once it held more than 64 plans — and that is not an expiry. A
## burst of a hundred chases re-planning in one frame put more than 64 results in it, the clear took
## every one of them, and the plans of goblins that were alive and asking went with it: each of those
## bodies then waited on a job that had already been answered, `_job` stayed set, and it never asked
## again (`PLAN_TIMEOUT_TICKS` in `scripts/goblin.gd` is the backstop for a lost answer, but the
## table should not be losing them). AGE is what "nobody is coming for this" actually means.
static func _expire(frame: int) -> void:
	if _arrived.is_empty():
		return
	# `keys()` is a copy, so erasing through it is safe.
	for id in _arrived.keys():
		if frame - int(_arrived[id]) > PLAN_MAX_AGE_FRAMES:
			_arrived.erase(id)
			_finished.erase(id)
	if _arrived.size() <= MAX_FINISHED:
		return
	var ids: Array = _arrived.keys()
	ids.sort_custom(func(a, b): return int(_arrived[a]) < int(_arrived[b]))
	for i in range(ids.size() - MAX_FINISHED):
		_arrived.erase(ids[i])
		_finished.erase(ids[i])
