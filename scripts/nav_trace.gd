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
##
## The file opens with what BUILD it came from -- the engine, the hash and timestamp of
## `scripts/goblin.gd`, and the timestamp, size and name of every extension library in `res/bin` --
## because one round was spent reading a trace of a fix that was in the tests and NOT in the
## extension the game had loaded, and nothing in the file said which binary had been running. A
## tally of why routes were dropped sits beside the plan counts, for the same reason: the reasons
## exist as event lines, but a session's shape is a number ("dropped 3: two stuck, one lip").
##
## The PLAYER has a track of his own, above the samples: every step he takes, with the 3x3 of column
## heights around his feet. Those lines exist to answer the one question the goblin's lines cannot --
## what does the player DO across the corner the walker calls a wall? He evidently crosses it, and
## his own line through it (exact feet, the step between lines, and the wall, the hole and the
## ground drawn as numbers) is the answer rather than a guess about it.
## Where the session's trace lands. A fixed name, overwritten every session: the last run is the
## one that is there, and the last run is the one a report is about.
const TRACE_PATH := "user://nav_trace.txt"
## A headless session is a probe, and a probe writes its own file: the probes run in this same user
## directory, so a run of them after a test overwrote the very trace the test was being read from
## (2026-11-27 12:38: the game's session replaced by a stub world at `(0, 294, 0)`).
const TRACE_PATH_HEADLESS := "user://nav_trace_probe.txt"
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
## How far above and below the feet the player's column map looks, so a three-high wall and a
## four-deep pit both read as numbers instead of "nothing in reach".
const PLAYER_SCAN_UP := 6
const PLAYER_SCAN_DOWN := 8
## Lines kept for the player's own track: a few minutes of walking, and bounded like the others.
const MAX_PLAYER_LINES := 800
## How far the player has to move, in blocks, before another step line is worth writing.
const PLAYER_STEP := 0.2
## Cells of a route spelled out in full before the rest is a count. Six is enough to see what the
## head of the plan is doing, which is where a route that climbs something it cannot climb shows.
const PATH_CELLS_SHOWN := 6

## Test builds only: a probe that builds a recorder to check its lines must not write over the real
## session's trace -- `user://nav_trace.txt` is one name, and the player's last chase lives in it.
static var muted := false
## Whether a recorder has been created this session. Two goblins spawned on the same frame both
## find an empty root, so the "only one" guard has to live here rather than in the tree.
static var _claimed := false

## Plans asked for and answered, and whatever the goblin's own rules have to say (a chase
## starting, a chase ending).
var _events: Array[String] = []
## One line per sample: the player's position and every goblin's, with what it is doing.
var _samples: Array[String] = []
## One line per step the player takes, with the blocks around his feet at that moment.
var _player_lines: Array[String] = []
var _last_player_at := Vector3.INF
var _last_player_map := ""
## The world, found once: `/root/Main/ChunkManager`, the node every goblin reads as well.
var _world_cache: Node = null
## Why routes were dropped, counted by the goblin's own reason word. Kept beside the plan counts so
## a session can be read at a glance, as the event lines alone do not say how often each happened.
var _drops: Dictionary = {}
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


## The session's own provenance: the engine, the goblin script, and every extension library Godot
## could see. Nothing here is guessed -- a missing file prints as such rather than as a zero, since
## "the DLL is not where I think" is exactly the thing this is for.
##
## Built when the file is written, not when the node appears: the answer cannot change during a
## session, and it used to be paid for on the frame after the first goblin spawned -- a read and a
## SHA-256 of `scripts/goblin.gd` and of every DLL in `res/bin` (5.6 MB here), which is a visible
## hitch in the middle of a spawn for a set of lines nobody reads until the file exists.
func _build_lines() -> Array[String]:
	var out: Array[String] = []
	out.append("engine %s" % str(Engine.get_version_info().get("string", "?")))
	out.append("script %s  %s" % [_stamp("res://scripts/goblin.gd"), "res://scripts/goblin.gd"])
	var dir := DirAccess.open("res://bin")
	if dir == null:
		out.append("library n/a (res://bin is not readable in this build)")
	else:
		for file in dir.get_files():
			if file.begins_with("~") or not (file.ends_with(".dll") or file.ends_with(".so") 					or file.ends_with(".dylib")):
				continue
			out.append("library %s  res://bin/%s" % [_stamp("res://bin/" + file), file])
	return out


## One file as a reader can tell two builds apart by: the hash of what is in it, and when it was
## last written. A hash alone cannot say "the DLL I just built"; a timestamp alone cannot say "the
## DLL whose source I am reading". Both, cut short, is what fits on a line.
##
## The hash comes from `FileAccess.get_sha256`, which hashes the file in the engine: the obvious
## `sha256_text()` of `get_file_as_string()` builds the whole file as a String first (2.8 MB of
## DLL, twice over here), and that string is the cost this session's first spawn was paying.
func _stamp(path: String) -> String:
	var digest := FileAccess.get_sha256(path)
	if digest.is_empty():
		return "unreadable"
	return "%s %s" % [digest.substr(0, 8), _when(FileAccess.get_modified_time(path))]


func _when(unix: int) -> String:
	if unix <= 0:
		return "unknown"
	return Time.get_datetime_string_from_unix_time(unix, true)


## A route given up on, by the reason the walker gave. The event line already says when; this says
## how often, which is the number a session is judged by.
func note_drop(reason: String) -> void:
	_drops[reason] = int(_drops.get(reason, 0)) + 1


func _process(delta: float) -> void:
	_track_player()
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
func plan_asked(job: int, role: String, from: Vector3, to: Vector3, expansions: int,
		max_ms: float) -> void:
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
		% [job, role, found, truncated, nodes.size(), end, _path_text(nodes),
		   float(result.get("ms", 0.0)), int(result.get("expansions", 0)),
		   result.get("error", "")])


## The route as a reader needs it: the first few cells (as feet, like `end`), then how many were
## left out. The END is printed beside it because that is the number a chase is judged by; the HEAD
## says whether the walk was ever aimed at the climb at all.
func _path_text(nodes: PackedVector3Array) -> String:
	var out := ""
	for i in nodes.size():
		if i >= PATH_CELLS_SHOWN:
			out += " (+%d)" % (nodes.size() - PATH_CELLS_SHOWN)
			break
		if i > 0:
			out += ">"
		var cell: Vector3 = nodes[i]
		out += _cell_text(Vector3(cell.x, cell.y + 1.0, cell.z))
	return out


## The session is over: write it out, and say where it went with the tail of it. `_exit_tree` is
## the last moment the tree is still whole, so this is where a session-end report belongs.
func _exit_tree() -> void:
	if muted:
		return
	# The session's provenance, made here because this is the only place it is read.
	var header := _build_lines()
	var lines: Array[String] = ["# Farlands nav trace"]
	lines.append("plans %d: found %d, truncated %d, failed %d"
		% [_plans, _found, _truncated, _failed])
	if not _drops.is_empty():
		var dropped: Array[String] = []
		for reason in _drops.keys():
			dropped.append("%s %d" % [reason, int(_drops[reason])])
		lines.append("route drops: " + ", ".join(dropped))
	lines.append_array(header)
	lines.append("")
	lines.append("# events -- plans asked for and answered, and the chase's own decisions")
	lines.append_array(_events)
	lines.append("")
	lines.append("# player -- every step: exact feet, the cell, the step since the last line, and the "
		+ "3x3 of column tops around the feet")
	lines.append("#   rows are z-1, z, z+1 and columns x-1, x, x+1; 0 = the floor under the feet, "
		+ "1 = a block at the feet's level, -1 = a hole one deep, X = nothing in reach")
	lines.append_array(_player_lines)
	lines.append("")
	lines.append("# samples -- once a second: the player, then every goblin")
	lines.append_array(_samples)

	var path := TRACE_PATH_HEADLESS if DisplayServer.get_name() == "headless" else TRACE_PATH
	var out := FileAccess.open(path, FileAccess.WRITE)
	if out == null:
		print("NavTrace: could not write %s (%s)"
			% [path, error_string(FileAccess.get_open_error())])
		return
	for line in lines:
		out.store_line(line)
	out.close()

	print("--- Farlands nav trace: %d plans, found %d, truncated %d, failed %d ---"
		% [_plans, _found, _truncated, _failed])
	for line in header:
		print(line)
	print("full trace: %s" % ProjectSettings.globalize_path(path))
	for line in _tail(_events, CONSOLE_TAIL):
		print(line)
	for line in _tail(_samples, CONSOLE_SAMPLES):
		print(line)


## The world's own cell queries, found once. A probe or a stub world has none, and the map is then
## written as `?` rather than as a guess.
func _world() -> Node:
	if _world_cache != null and is_instance_valid(_world_cache):
		return _world_cache
	var tree := get_tree()
	if tree == null or tree.root == null:
		return null
	_world_cache = tree.root.get_node_or_null(NodePath("/root/Main/ChunkManager"))
	return _world_cache


## The player's movement, step by step, with the blocks around his feet.
##
## Written when his feet CELL changes, when he has moved a fifth of a block, or when the blocks
## around him have changed under him -- a gap he stood beside, a block he placed. That is fine
## enough to see a jump (the feet rise and come down between two lines) and coarse enough that a
## whole crossing costs a dozen lines.
func _track_player() -> void:
	var tree := get_tree()
	if tree == null:
		return
	var scene := tree.current_scene
	if scene == null:
		return
	var body := scene.get_node_or_null(NodePath("Player")) as Node3D
	if body == null:
		return
	var at := body.global_position
	var map := column_map(at)
	var step := Vector3.INF
	if _last_player_at != Vector3.INF:
		step = at - _last_player_at
	var moved := _last_player_at == Vector3.INF or at.distance_to(_last_player_at) >= PLAYER_STEP
	var cell_changed := _last_player_at == Vector3.INF \
		or floori(at.x) != floori(_last_player_at.x) \
		or floori(at.y) != floori(_last_player_at.y) \
		or floori(at.z) != floori(_last_player_at.z)
	if not moved and not cell_changed and map == _last_player_map:
		return
	_last_player_at = at
	_last_player_map = map
	var step_text := "first" if step == Vector3.INF \
		else "(%.2f, %.2f, %.2f)" % [step.x, step.y, step.z]
	_append(_player_lines, "%7.2fs player feet=(%.2f, %.2f, %.2f) cell=%s step=%s 3x3=%s"
		% [_now(), at.x, at.y, at.z, _cell_text(at), step_text, map])
	while _player_lines.size() > MAX_PLAYER_LINES:
		_player_lines.remove_at(0)


## The 3x3 of column tops around a FEET position, as one readable grid: rows are z-1, z, z+1 and
## columns are x-1, x, x+1. `0` is the floor under those feet, `1` a block at their own level (a
## step up), `2` a two-block wall, `-1` a hole one deep, `X` nothing solid within reach. Anything
## that is not air counts as a block -- a liquid is not a floor. Both read it, because it is one
## question about one place: the player's step track and a goblin's own sample line.
func column_map(at: Vector3) -> String:
	var world := _world()
	if world == null or not world.has_method("get_block"):
		return "?"
	var x := floori(at.x)
	var y := floori(at.y)
	var z := floori(at.z)
	var rows: Array[String] = []
	for dz in [-1, 0, 1]:
		var row: Array[String] = []
		for dx in [-1, 0, 1]:
			var top := 255
			for dy in range(PLAYER_SCAN_UP, -PLAYER_SCAN_DOWN, -1):
				if int(world.call("get_block", x + dx, y + dy, z + dz)) != 0:
					top = dy
					break
			# The floor under the feet is (feet - 1), and that is the 0 of this grid.
			row.append("X" if top == 255 else str(top + 1))
		rows.append(",".join(row))
	return "[%s]" % " / ".join(rows)


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
