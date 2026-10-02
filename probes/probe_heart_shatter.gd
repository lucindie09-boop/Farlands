extends SceneTree
## The damage shatter, measured in the rendered frame.
##
## A hit takes red off a heart, and that red does not blink out -- it is thrown
## off. This probe pins what a frame can be read for:
##
##   - at the instant of the hit the red is all still on the screen, and none of
##     it has fallen clear of the heart: the pixels are where the heart drew
##     them, they are just no longer the heart;
##   - a beat later the red that left the heart is below it -- at least a third of
##     what the hit freed, and never more than it freed;
##   - the airborne red descends from frame to frame -- the lowest of it is lower
##     than it was -- and by the time the effect is over the frame is the new
##     state alone;
##   - healing drops nothing, and neither does a frame with no change at all:
##     the shatter is damage, not a redraw.
##
## The counts come from the sprite, not from this probe: heart_full.png holds 34
## red texels, heart_half.png 20, and 14 of the full heart's are not red in the
## half one. They are checked as a floor and a ceiling rather than an equality
## only because the spray overlaps itself -- two shards over one another paint one
## pixel. A shard is one texel and is never turned, so it can never paint a pixel
## its own texel did not.
##
## Runs WINDOWED through probes/run_probe_shot.sh (a screenshot from the dummy
## renderer is blank); the shots land in user://heart_shots/.
##
##   probes/run_probe_shot.sh probes/probe_heart_shatter.gd [timeout]

const SHOT_DIR := "user://heart_shots"
const HEART_COUNT := 10
const HEART_TEXELS := 9
const MAX_HEALTH := 20
## The art's red texels by heart state: empty, half, full.
const HEART_RED := [0, 20, 34]
## The hit-time count may lose this much of its red to the spray overlapping
## itself.
const RED_FLOOR := 0.7
## How far above the row the scan reaches, in units: a pixel thrown off the top
## of a heart rises about four, and has not left the row until it comes back.
const HEADROOM := 12
## How far below the row's own rect a pixel can be and still count as "not fallen
## clear of the heart yet": the throw is hard enough that the red at the bottom
## of a heart is a pixel or two past that line within the two frames the at-hit
## shot takes.
const CLEAR_UNITS := 5
## How far outside the row's own rect a thrown pixel can still be counted: the
## throw is 38 units/s, and everything is measured well inside its first second.
const MARGIN := 24

var main: Node3D = null
var bar: Control = null
var player: Node = null
var ui: Node = null
var death_screen: Node = null
var failures := 0

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene := load("res://Main.tscn") as PackedScene
	if scene == null:
		_fail("main.tscn is missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)
	await process_frame

	# A black field behind the HUD, and every other HUD surface out of the way:
	# the only red in the frame is then the hearts' own, wherever it is. The
	# field is on the canvas layer below the HUD, so the hearts and their pixels
	# draw over it exactly as they draw over the world.
	var backdrop := ColorRect.new()
	backdrop.color = Color.BLACK
	backdrop.set_anchors_preset(Control.PRESET_FULL_RECT)
	backdrop.mouse_filter = Control.MOUSE_FILTER_IGNORE
	main.add_child(backdrop)
	for name in ["HUD/GodRaysOverlay", "HUD/FPSCounter", "HUD/Inventory",
			"HUD/CraftingTableMenu", "HUD/Chat", "HUD/Wand", "HUD/ShaderOverlay",
			"HUD/SettingsMenu", "HUD/Crosshair", "HUD/Compass", "HUD/DeathScreen",
			"Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay"]:
		# Only what actually draws is a CanvasItem: the wand's node is a plain
		# Node whose visual is in the 3D world, which the field already covers.
		var node := main.get_node_or_null(name)
		if node is CanvasItem:
			(node as CanvasItem).visible = false
	bar = main.get_node_or_null("HUD/Healthbar")
	player = main.get_node_or_null("Player")
	ui = root.get_node_or_null("UIScale")
	death_screen = main.get_node_or_null("HUD/DeathScreen")
	if bar == null or player == null or ui == null:
		_fail("no Healthbar, Player or UIScale in the scene")
		quit(1)
		return

	# Frozen where it spawned and with an empty hotbar: a player in free fall
	# would take fall damage in the middle of a case, and an item's icon can
	# carry red of its own inside the band this probe reads.
	player.call("set_physics_process", false)
	player.call("clear_inventory")

	# The world is not what this probe measures: the smallest render distance it
	# accepts keeps the frames coming at the speed of the HUD alone. It is set
	# twice because the settings menu loads the saved config -- the render
	# distance and the GUI scale the frame is read at -- over the first frames
	# and overwrites the first setting with the saved one.
	var cm := main.get_node_or_null("ChunkManager")
	if cm:
		cm.call("set_render_distance", 2)
	await _frames(2)
	if cm:
		cm.call("set_render_distance", 2)
	await _frames(10)
	if cm:
		cm.call("set_render_distance", 2)
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	print("probe: scene up, render distance %s"
		% [cm.call("get_render_distance") if cm else "no ChunkManager"])

	var bands := _bands()
	print("probe: GUI scale %.0f, window %s, the full row's red is %d texels"
		% [float(ui.get("value")), DisplayServer.window_get_size(), _hearts_red(MAX_HEALTH)])
	var opening := await _capture("opening")
	_check("opening: the row", opening, bands[0], _hearts_red(MAX_HEALTH) * _texel_px(),
		_hearts_red(MAX_HEALTH) * _texel_px(), "the ten full hearts")
	_check("opening: below the row", opening, bands[1], 0, 0, "nothing has been hit")

	# 20 -> 19: the last heart is drained to its half, and 14 texels come off it.
	await _hit(20, 19, "full_to_half")
	# 19 -> 18: that heart empties out, 20 texels.
	await _hit(19, 18, "half_to_empty")
	# 18 -> 16: a whole heart at once, 34 texels.
	await _hit(18, 16, "heart_lost_whole")
	# 16 -> 17 and 17 -> 20: health going up is not damage.
	await _heal(16, 17, "heal_half")
	await _heal(17, 20, "heal_full")
	# 20 -> 0: a death takes all ten hearts' red at once, 340 texels.
	await _hit(20, 0, "death")

	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)


## One hit, read at the moments the effect has: before it, at the instant of it,
## while the pixels are in the air, and after they are gone.
func _hit(from_health: int, to_health: int, label: String) -> void:
	var bands := _bands()
	var px := _texel_px()
	var full := _hearts_red(from_health) * px
	var left := _hearts_red(to_health) * px
	var lost := _hearts_red(from_health) - _hearts_red(to_health)
	player.call("set_health", from_health)
	await _frames(3)
	var before := await _capture("%s_before" % label)
	_check("%s: before" % label, before, bands[0], full, full, "the row as it was")
	_check("%s: below before" % label, before, bands[1], 0, 0, "nothing has been hit")

	player.call("set_health", to_health)
	# The death overlay tints the whole frame, and the effect is in it: hide it
	# again after the died signal has shown it.
	if death_screen:
		death_screen.visible = false
	# At the instant of the hit the frame is the frame it was: the pixels are
	# still where the heart drew them, they have just started moving. It is read
	# a frame in, and the shatter has already taken its first step by then, so what
	# is asserted is that nothing has fallen clear of the heart: the red at the
	# bottom of a heart is a pixel or two past the row's own line within those
	# frames, and that is the throw being felt, not a shard in the wrong place.
	var at_hit := await _capture("%s_hit" % label, 1)
	_check("%s: at the hit" % label, at_hit, bands[0], int(full * RED_FLOOR), full,
		"all of the red, %d of it the hit took away" % lost)
	_check("%s: clear at the hit" % label, at_hit, bands[2], 0, 0,
		"nothing has fallen clear of the heart yet")

	# A third of a second in, the throw has carried most of the shards clear of
	# the row and still holds nearly all of them on the screen: the row band holds
	# the new state plus the red still in the air, and that red is below the row.
	await _wait(0.35)
	var flying := await _capture("%s_flying" % label)
	_check("%s: in the air" % label, flying, bands[0], left,
		left + lost * px,
		"the heart's new state plus the %d texels still falling" % lost)
	_check("%s: red below the row" % label, flying, bands[1], lost * px / 3,
		lost * px, "the red the hit took, gone from the heart")

	# Gravity: the same red is lower a moment later. Shards are still crossing the
	# row's line in that window, so the reading is the leading edge -- the lowest
	# red in the band, which no shard can overtake.
	await _wait(0.08)
	var falling := await _capture("%s_falling" % label)
	_check("%s: still falling" % label, falling, bands[1], 1,
		lost * px, "the same shards")
	_descent(label, flying, falling, bands[1])

	# And once the effect is over -- every shard's own life at most, and they are
	# not all the same length -- the frame holds the new state and nothing else.
	await _wait(0.85)
	var after := await _capture("%s_after" % label)
	_check("%s: after" % label, after, bands[0], left, left, "the heart's new state alone")
	_check("%s: below after" % label, after, bands[1], 0, 0, "the pixels are gone")


## Health going up is not damage: the row gains red, and nothing ever leaves it.
func _heal(from_health: int, to_health: int, label: String) -> void:
	var bands := _bands()
	var px := _texel_px()
	player.call("set_health", from_health)
	await _frames(3)
	var before := await _capture("%s_before" % label)
	_check("%s: before" % label, before, bands[0], _hearts_red(from_health) * px,
		_hearts_red(from_health) * px, "the row as it was")

	player.call("set_health", to_health)
	var now := await _capture("%s_now" % label)
	_check("%s: healed" % label, now, bands[0], _hearts_red(to_health) * px,
		_hearts_red(to_health) * px, "the row as it is now")
	_check("%s: nothing below" % label, now, bands[1], 0, 0, "healing drops nothing")

	await _wait(0.50)
	var later := await _capture("%s_later" % label)
	_check("%s: still nothing below" % label, later, bands[1], 0, 0, "healing drops nothing")


## The row band -- the hearts plus the headroom a thrown pixel rises into, plus
## everything below them -- the part of it below the row's own rect, which is
## where a pixel has to be to have left the heart it came from, and the part of
## that which is clear of the heart as well.
func _bands() -> Array:
	var scale := float(ui.get("value"))
	var origin: Vector2 = bar.call("_heart_origin", 0, scale) + bar.global_position
	var last: Vector2 = bar.call("_heart_origin", HEART_COUNT - 1, scale) + bar.global_position
	var left := int(round(origin.x - MARGIN * scale))
	var right := int(round(last.x + (HEART_TEXELS + MARGIN) * scale))
	var top := int(round(origin.y - HEADROOM * scale))
	var bottom := int(round(bar.size.y))
	var row := Rect2i(left, top, right - left, bottom - top)
	var below_top := int(round(origin.y + HEART_TEXELS * scale))
	var below := Rect2i(left, below_top, right - left, bottom - below_top)
	var clear_top := int(round(origin.y + (HEART_TEXELS + CLEAR_UNITS) * scale))
	var clear := Rect2i(left, clear_top, right - left, bottom - clear_top)
	return [row, below, clear]


## How many device pixels one texel of the art covers at the current scale.
func _texel_px() -> int:
	var scale := float(ui.get("value"))
	return int(round(scale * scale))


## How many red texels the row draws at `health`: the art's own counts, heart by
## heart, which is the same rule the healthbar follows.
func _hearts_red(health: int) -> int:
	var total := 0
	for i in range(HEART_COUNT):
		var half_hearts_left := health - i * 2
		var state := 0
		if half_hearts_left >= 2:
			state = 2
		elif half_hearts_left == 1:
			state = 1
		total += HEART_RED[state]
	return total


# --- Reading the frame ---------------------------------------------------------

## The red pixels of `rect`: their count, their mean y and the lowest of them.
## The frame's bytes are read directly rather than through get_pixel, which is
## what makes a few thousand pixels a scan nobody notices.
func _red_in(image: Image, rect: Rect2i) -> Dictionary:
	var x0 := clampi(rect.position.x, 0, image.get_width())
	var x1 := clampi(rect.end.x, 0, image.get_width())
	var y0 := clampi(rect.position.y, 0, image.get_height())
	var y1 := clampi(rect.end.y, 0, image.get_height())
	var data := image.get_data()
	var count := 0
	var sum_y := 0.0
	var lowest := -1
	for y in range(y0, y1):
		var base := y * image.get_width()
		for x in range(x0, x1):
			var i := (base + x) * 4
			# The hearts' own red. Everything else over the field is black, the
			# hotbar's brown art, or the warm grey of an emptied heart.
			if data[i] > 128 and data[i + 1] < 100 and data[i + 2] < 100:
				count += 1
				sum_y += float(y)
				lowest = maxi(lowest, y)
	return {"count": count, "mean_y": sum_y / maxf(float(count), 1.0), "lowest": lowest}


func _check(label: String, image: Image, rect: Rect2i, low: int, high: int, note: String) -> void:
	var count: int = int(_red_in(image, rect)["count"])
	if count < low or count > high:
		_fail("%s: %d red pixels, want %d..%d (%s)" % [label, count, low, high, note])
		return
	print("probe: %-28s %5d red pixels (%s)" % [label, count, note])


## Gravity, read off the leading edge: the lowest red pixel below the row. Every
## shard is under the same acceleration, so the one out in front of the fall
## stays in front until it is off the screen, and it can only be lower than it
## was. The mean y is printed alongside as the bulk's own reading.
func _descent(label: String, earlier: Image, later: Image, rect: Rect2i) -> void:
	var a := _red_in(earlier, rect)
	var b := _red_in(later, rect)
	if b["count"] == 0:
		_fail("%s: no red left below the row to fall" % label)
		return
	if b["lowest"] <= a["lowest"]:
		_fail("%s: the leading red did not descend (row %d -> %d)"
			% [label, a["lowest"], b["lowest"]])
		return
	print("probe: %-28s leading red row %d -> %d (mean y %.1f -> %.1f): it is falling"
		% [label + ": descent", a["lowest"], b["lowest"], a["mean_y"], b["mean_y"]])


# --- Driving the frame ---------------------------------------------------------

func _frames(count: int) -> void:
	for i in range(count):
		await process_frame


func _wait(seconds: float) -> void:
	await create_timer(seconds).timeout


## Let the frame settle, hand back what it drew, and keep it.
func _capture(shot: String, frames := 2) -> Image:
	await _frames(frames)
	var image: Image = root.get_texture().get_image()
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	image.save_png("%s/%s.png" % [SHOT_DIR, shot])
	return image


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)
