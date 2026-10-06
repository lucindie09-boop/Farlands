extends SceneTree
## Does the Sky Tint setting reach what the world is drawn with?
##
## The setting is a toggle on `sky_light_warmth` -- the tint every sky-lit surface is
## multiplied by (shaders/item_lighting.gdshaderinc): ON is the sun's own colour with
## its warm midday CREAM (the shipped look), OFF is the same curve with a white
## zenith, so the ground renders at its own texture at noon. Both are warm at the
## horizon, so the toggle is about midday and nothing else.
##
## What it holds: the default, the cream while ON, white while OFF, and that the two
## curves MEET once the sun is down at the horizon -- which is the property that makes
## this a tint toggle rather than a switch that flattens every sunset.
##
## The numbers are read off the uniform the world's materials are handed, through the
## same call the terrain is fed through (ChunkManager.apply_item_lighting on a scratch
## material), so they are the value the ground is lit with and not a copy of the
## setting. That works without a renderer, and it needs no scene at all: it
## instantiates a bare ChunkManager and never adds it to the tree, so nothing is
## generated, meshed or saved, and it leaves the running game's world alone.
##
##   probes/run_probe.sh probes/probe_sky_tint.gd 120
##
## (run_probe.sh is fine but will refuse while a game is open; this probe writes
## nothing, so the plain line is enough when one is:
##   "$GODOT" --headless --path . --script res://probes/probe_sky_tint.gd )

const NOON := 0.5          # day_time: the sun highest (get_sun_elevation == 1)
const HORIZON_TIME := 0.25 # day_time: the sun ON the horizon (elevation == 0)
## The world's midday cream, from DayNightCycle::get_sun_color() at the zenith.
const CREAM := Color(1.0, 0.85, 0.6, 1.0)
const TOL := 0.02

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


## The world's own `sky_light_warmth`, read off the call the terrain material is fed
## through: a scratch material, so the probe cannot disturb a real one.
func _world_warmth(cm: Node) -> Color:
	var scratch := ShaderMaterial.new()
	scratch.shader = load("res://shaders/item_shader.gdshader")
	cm.apply_item_lighting(scratch)
	return scratch.get_shader_parameter("sky_light_warmth")


func _near(a: Color, b: Color) -> bool:
	return absf(a.r - b.r) <= TOL and absf(a.g - b.g) <= TOL and absf(a.b - b.b) <= TOL


func _warm(c: Color) -> bool:
	return c.g < c.r and c.b < 0.6 * c.r


func _run() -> void:
	# A bare manager, never added to the tree: it has its controller (made in the
	# constructor) and so a day/night cycle and an environment to read, and no world
	# behind it to generate, update or save.
	var cm: Node = ClassDB.instantiate("ChunkManager")
	if cm == null:
		print("PROBE FAIL: ChunkManager is not registered")
		quit(1)
		return
	cm.set("day_night_cycle_enabled", true)
	cm.set("day_time", NOON)

	_ok("setting: the sky tint is on out of the box", bool(cm.get("sky_tint_enabled")),
		"default is %s" % str(cm.get("sky_tint_enabled")))

	var on: Color = _world_warmth(cm)
	_ok("tint on: the world's midday cream", _near(on, CREAM), "the world is handed %s at noon" % str(on))

	cm.set("sky_tint_enabled", false)
	var off: Color = _world_warmth(cm)
	_ok("tint off: a white zenith", _near(off, Color.WHITE), "the world is handed %s at noon" % str(off))
	_ok("tint off: it actually moved", not _near(on, off), "%s at noon" % str(on))

	# ...and the end that must NOT move: the sun's own warmth as it drops.
	cm.set("day_time", HORIZON_TIME)
	var off_horizon: Color = _world_warmth(cm)
	_ok("tint off: still warm at the horizon", _warm(off_horizon), "handed %s" % str(off_horizon))
	cm.set("sky_tint_enabled", true)
	var on_horizon: Color = _world_warmth(cm)
	_ok("tint on: warm at the horizon too", _warm(on_horizon), "handed %s" % str(on_horizon))
	_ok("the two tints meet at the horizon", _near(on_horizon, off_horizon),
		"%s vs %s" % [str(on_horizon), str(off_horizon)])

	# The setting is also a property, which is what the settings row writes through.
	cm.set("sky_tint_enabled", true)
	_ok("setting: it flips back", bool(cm.get("sky_tint_enabled")))

	print("PROBE sky tint: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
