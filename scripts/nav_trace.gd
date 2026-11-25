extends Node
## The session's pathfinding trace: every plan a goblin asked for, what came back, and once a
## second where the player and every goblin were while it happened. Written out when the game
## closes.
##
## It exists because a route is invisible. A goblin standing three blocks under its target, one
## that walks the long way round when it did not need to, and one whose plan came back truncated
## all look the same in the world, and they are three different causes -- only the plan says
## which. So the asks and the answers are recorded as they happen, with their own numbers
## (found, truncated, the cell the route ENDS on, expansions, milliseconds and the search's own
## error), and the world is sampled so the numbers can be read against two positions.
##
## Nothing here changes what the game does. A line is string formatting, sampling is a 1 Hz roll
## on the frame clock, both lists are capped, and the file is written once, at the end.
##
## It is one node for the whole session, a child of the root named `NavTrace`, installed by the
## first goblin to wake (`_install_trace` in `scripts/goblin.gd`). A session with no goblin in it
## records nothing, and a goblin that finds no trace in the tree records nothing either -- which
## is what a probe or a stub world wants.

## Where the session's trace lands. A fixed name, overwritten every session: the last run is the
## one that is there, and the last run is the one a report is about.
const TRACE_PATH := "user://nav_trace.txt"
## Lines kept per list. A line is one event or one sample, and an hour of play is 3600 samples, so
## this holds a whole session and still bounds a pathological one.
const MAX_LINES := 4000
## How often the world is sampled, in seconds.
const SAMPLE_SECONDS := 1.0
## Lines of each list echoed to the console when the game closes: enough to read the end of the
## last chase off the terminal, without pasting thousands of lines into it.
const CONSOLE_TAIL := 40
## Samples echoed with them: the last handful of positions is what the events are read against.
const CONSOLE_SAMPLES := 6

## Whether a recorder has been created this session. Two goblins spawned on the same frame both
## find an empty root, so the "only one" guard has to live here rather than in the tree.
static var _claimed := false

## Plans asked for and answered, and whatever the goblin's own rules have to say (a chase
## starting, a chase ending).
var _events: Array[String] = []
## One line per sample: the player's position and every goblin's, with what it is doing.
var _samples: Array[String] = []
var _plans := 0
var _found := 0
var _truncated := 0
var _failed := 0
var _start_ms := 0
var _next_sample := 0.0


## Claims the session's recorder. Answers false when one already exists, so the caller installs
## nothing -- see `_install_trace` in `scripts/goblin.gd`.
static func claim() -> bool:
	if _claimed:
		return false
	_claimed = true
	return true


func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	_events.append("session %s" % Time.get_datetime_string_from_system())


func _process(delta: float) -> void:
	_next_sample -= delta
	if _next_sample > 0.0:
		return
	_next_sample = SAMPLE_SECONDS
	_sample()


## One event, stamped with the seconds since the trace started.
func add_line(text: String) -> void:
	_append(_events, "%7.2fs %s" % [_now(), text])


## A plan going out, tagged with the job id so the ask and the answer can be read together even
## with other plans in flight. `role` is the body's speed state: chasing, fleeing, idle. The
## budget is the one thing the answer cannot show — `ms` is what was SPENT, not what was allowed —
## and it is the number a chase escalates (see CHASE_BUDGET_LIMIT_MS in scripts/goblin.gd).
func plan_asked(job: int, role: String, from: Vector3, to: Vector3, expansions := 0,
		max_ms := 0.0) -> void:
	_plans += 1
	add_line("ASK  %-4d %-8s from=%s to=%s budget=%dexp/%.0fms"
		% [job, role, _cell_text(from), _cell_text(to), expansions, max_ms])


## The plan coming back. Counted as found, truncated (a budget ran out) or failed (nowhere to go),
## which are the three answers a chase has to be able to tell apart.
func plan_answered(job: int, role: String, result: Dictionary) -> void:
	var found := bool(result.get("found", false))
	var truncated := bool(result.get("truncated", false))
	if found:
		_found += 1
	elif truncated:
		_truncated += 1
	else:
		_failed += 1
	var nodes: PackedVector3Array = result.get("nodes", PackedVector3Array())
	var end := "none"
	if not nodes.is_empty():
		# The planner hands back SUPPORT cells -- the block under the feet -- so the end of the
		# route is stated as the feet cell standing on it: the number a reader compares to the
		# target's own, and the one that says "three blocks under the player" when it is wrong.
		var last: Vector3 = nodes[nodes.size() - 1]
		end = _cell_text(Vector3(last.x, last.y + 1.0, last.z))
	add_line("PLAN %-4d %-8s found=%s truncated=%s nodes=%d end=%s path=%s ms=%.1f exp=%d error=\"%s\""
		% [job, role, found, truncated, nodes.size(), end, _path_text(nodes, 6),
		   float(result.get("ms", 0.0)), int(result.get("expansions", 0)),
		   result.get("error", "")])


## The route as a reader needs it: the first few cells (as feet, like `end`), then how many were
## left out. The END is printed beside it because that is the number a chase is judged by; the HEAD
## says whether the walk was ever aimed at the climb at all.
func _path_text(nodes: PackedVector3Array, show: int) -> String:
	var out := ""
	for i in nodes.size():
		if i >= show:
			out += " (+%d)" % (nodes.size() - show)
			break
		if i > 0:
			out += ">"
		var cell: Vector3 = nodes[i]
		out += _cell_text(Vector3(cell.x, cell.y + 1.0, cell.z))
	return out


## The session is over: write it out, and say where it went with the tail of it. `_exit_tree` is
## the last moment the tree is still whole, so this is where a session-end report belongs.
func _exit_tree() -> void:
	var lines: Array[String] = ["# Farlands nav trace"]
	lines.append("plans %d: found %d, truncated %d, failed %d"
		% [_plans, _found, _truncated, _failed])
	lines.append("")
	lines.append("# events -- plans asked for and answered, and the chase's own decisions")
	lines.append_array(_events)
	lines.append("")
	lines.append("# samples -- once a second: the player, then every goblin")
	lines.append_array(_samples)

	var out := FileAccess.open(TRACE_PATH, FileAccess.WRITE)
	if out == null:
		print("NavTrace: could not write %s (%s)"
			% [TRACE_PATH, error_string(FileAccess.get_open_error())])
		return
	for line in lines:
		out.store_line(line)
	out.close()

	print("--- Farlands nav trace: %d plans, found %d, truncated %d, failed %d ---"
		% [_plans, _found, _truncated, _failed])
	print("full trace: %s" % ProjectSettings.globalize_path(TRACE_PATH))
	for line in _tail(_events, CONSOLE_TAIL):
		print(line)
	for line in _tail(_samples, CONSOLE_SAMPLES):
		print(line)


## The player's position and every goblin's state, once a second. A goblin is asked for its own
## line (`trace_state`), so the recorder needs to know nothing about how a mob works.
func _sample() -> void:
	var tree := get_tree()
	if tree == null:
		return
	var at := "none"
	var scene := tree.current_scene
	if scene != null:
		var body := scene.get_node_or_null(NodePath("Player"))
		if body is Node3D:
			at = _cell_text((body as Node3D).global_position)
	var line := "%7.2fs player=%s" % [_now(), at]
	for other in tree.get_nodes_in_group("goblin"):
		if other is Node3D and other.has_method("trace_state"):
			line += " | " + str(other.call("trace_state"))
	_append(_samples, line)


func _append(lines: Array[String], text: String) -> void:
	lines.append(text)
	if lines.size() > MAX_LINES:
		lines.remove_at(0)


func _tail(lines: Array[String], count: int) -> Array[String]:
	var out: Array[String] = []
	for i in range(maxi(lines.size() - count, 0), lines.size()):
		out.append(lines[i])
	return out


func _now() -> float:
	return float(Time.get_ticks_msec() - _start_ms) / 1000.0


## A position as the CELL it is in, rounded down like the planner's own conversion, so a trace
## number can be compared to a goal cell directly rather than to a body's centre.
func _cell_text(at: Vector3) -> String:
	return "(%d, %d, %d)" % [floori(at.x), floori(at.y), floori(at.z)]
