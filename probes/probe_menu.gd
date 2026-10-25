extends SceneTree
## Checks the title screen and the block-curtain transition.
##
## The curtain is measured in real physics frames, because its whole claim is that
## the ENGINE decides the motion: a sheet of rigid bodies falls, collides, piles up
## and goes to sleep, and only then does the screen hold still and swap. So the
## probe asks the bodies what happened — how many spun, whether any are stacked on
## another, whether any interpenetrate, whether every one of them is asleep — and
## times the hold itself rather than a constant.
##
## The live half boots the real scene against the real world and asks the menu
## layer what it did to the player, the HUD and the settings menu.
##
## Run: bash probes/run_probe.sh probes/probe_menu.gd

# Loaded at run time, not preloaded: a preload compiles before the autoloads exist,
# and a script that names the UIScale singleton cannot compile that early.
var Curtain: GDScript
var Title: GDScript

var ok := true


func _initialize() -> void:
	_run()


func _check(condition: bool, message: String) -> void:
	if condition:
		print("PROBE ok: ", message)
	else:
		ok = false
		print("PROBE FAIL: ", message)


func _run() -> void:
	Curtain = load("res://scripts/ui_block_curtain.gd")
	Title = load("res://scripts/title_screen.gd")
	await _check_title_scrim()
	await _check_title_rows()
	await _check_update_log()
	await _check_world_page()
	await _check_curtain()
	await _check_curtain_fuse()
	await _check_curtain_skip()
	await _check_reduce_motion()
	await _check_live_menu()
	print("PROBE menu ", "passed" if ok else "FAILED")
	quit(0 if ok else 1)


## The scrim is one plate, so there are seven things to check: it is dark where the labels
## start, it is gone a little past the buttons, it is gone everywhere else, it is a gradient
## rather than a handful of tones, that gradient carries per-square noise, squares lift off it
## and travel right while the holes heal behind them, and Reduce Motion stops the wind without
## touching the noise. The tone checks matter because a plate held flat and dropped over a short
## tail has two tones plus two or three cells of tail, which reads as a blob of four colours;
## the noise is checked PER SQUARE rather than on the ramp, because the ramp by construction
## cannot vary down a column.
func _check_title_scrim() -> void:
	var title: Control = Title.new()
	root.add_child(title)
	await process_frame
	title.size = Vector2(1920, 1080)
	title._layout()
	# The autoload by look-up, not by name: this script compiles before autoloads exist,
	# so naming `UIScale` here would fail to compile the probe itself.
	var ui: Node = root.get_node_or_null("UIScale")
	var s: float = float(ui.get("value")) if ui != null else 1.0
	var column_end: float = (title._margin + Title.COLUMN) * s
	var reach: float = (title._margin + Title.COLUMN + Title.PLATE_TAIL) * s
	_check(title._scrim_shade(0.0) >= 0.5,
		"the plate is darkest at the left edge (%.2f)" % title._scrim_shade(0.0))
	_check(title._scrim_shade(title._margin * s) >= 0.35,
		"the labels start on the plate (%.2f)" % title._scrim_shade(title._margin * s))
	_check(title._scrim_shade(reach + 2.0) <= 0.01,
		"the plate is gone a little past the buttons (%.0f px)" % reach)
	_check(title._scrim_shade(960.0) <= 0.01, "the middle of the screen is undarkened")
	_check(title._scrim_shade(1900.0) <= 0.01, "the right edge is undarkened")
	# One sample per drawn square, which is what the eye gets: a plate with a short tail has
	# four of them and a ramp has two dozen.
	var cell: float = title._cell_size()
	var tones := {}
	var sample := cell * 0.5
	while sample < reach:
		tones[snappedf(title._scrim_shade(sample), 0.001)] = true
		sample += cell
	_check(tones.size() >= 10,
		"the plate is a gradient, not a handful of tones (%d over %.0f px)" % [tones.size(), reach])
	var lightens := true
	var last: float = title._scrim_shade(0.0)
	var probe_x := 0.0
	while probe_x < reach:
		probe_x += 4.0
		var now: float = title._scrim_shade(probe_x)
		if now > last + 0.001:
			lightens = false
		last = now
	_check(lightens, "the plate only ever lightens towards the world")

	# The noise is per square, so the question is whether neighbouring squares actually
	# differ. A plate that only ramps has one tone per column, and no amount of sampling
	# inside a column finds a second one - so a 6 x 6 block of plate is thirty-six squares
	# and should be close to thirty-six tones, not six.
	var block_tones := {}
	for row in range(10, 16):
		for col in range(2, 8):
			block_tones[snappedf(title._cell_alpha(col, row), 0.001)] = true
	_check(block_tones.size() >= 24,
		"the plate carries per-square noise (%d tones in a 6x6 block)" % block_tones.size())
	var column_tones := {}
	for row in range(10, 20):
		column_tones[snappedf(title._cell_alpha(3, row), 0.001)] = true
	_check(column_tones.size() >= 6,
		"the noise varies down a column too (%d tones)" % column_tones.size())

	# The wind: squares leave the plate to the right and the plate heals behind them. The probe
	# steps the same `_process` the screen runs, so what it measures is the real timing.
	title._lifted.clear()
	title._healing.clear()
	for i in range(40):
		title._process(1.0 / 30.0)
	_check(not title._lifted.is_empty(),
		"squares lift off the plate (%d in the air)" % title._lifted.size())
	if not title._lifted.is_empty():
		var lift: Dictionary = title._lifted.values()[0]
		var at_start: Vector2 = lift["origin"] + lift["velocity"] * float(lift["age"])
		title._process(0.1)
		var at_now: Vector2 = lift["origin"] + lift["velocity"] * float(lift["age"])
		_check(at_now.x > at_start.x + 1.0,
			"a lifted square travels to the right (%.0f px)" % (at_now.x - at_start.x))
		_check(absf(lift["velocity"].y) > 0.0, "the wind is not perfectly level")
	for i in range(60):
		title._process(1.0 / 30.0)
	_check(not title._healing.is_empty(),
		"the plate heals behind it (%d fading back in)" % title._healing.size())

	# Reduce Motion keeps the noise and stops the wind.
	title._motion = false
	title._lifted.clear()
	title._healing.clear()
	for i in range(60):
		title._process(1.0 / 30.0)
	_check(title._lifted.is_empty(), "Reduce Motion leaves the plate still")
	_check(not title._jitter.is_empty(), "Reduce Motion keeps the noise")
	title._motion = true
	title.queue_free()
	await process_frame


## The rows are the game's own button, and the one thing this screen adds is the colour each
## one turns while it is current. A stylebox tint is a MULTIPLY of the texture's own fill, so
## the check multiplies it back: what the button draws is the hex the screen means, not a
## modulate near it. It also pins the empty focus box, because Godot draws that stylebox OVER
## the state's, and a filled one would hide the press without failing anything else.
func _check_title_rows() -> void:
	var title: Control = Title.new()
	root.add_child(title)
	await process_frame
	# In column order: the primary action, the two modes, Settings, Quit. Five is the
	# documented ceiling for this screen (docs/main-menu.md).
	var wanted := [Color("68944e"), Color("54907a"), Color("6f6a8c"), Color("5b7391"),
		Color("934d47")]
	var labels := ["PLAY", "SINGLEPLAYER", "MULTIPLAYER", "SETTINGS", "QUIT"]
	_check(title._buttons.size() == labels.size(),
		"the column is %d rows: %s" % [labels.size(), _labels(title)])
	for i in range(mini(title._buttons.size(), labels.size())):
		_check(String(title._buttons[i].text) == labels[i],
			"row %d is %s (%s)" % [i, labels[i], title._buttons[i].text])
	var fill: Color = Title.BUTTON_FILL
	var drop: float = Title.PRESSED_DARKEN
	for i in range(title._buttons.size()):
		var btn: Button = title._buttons[i]
		var hover := btn.get_theme_stylebox("hover") as StyleBoxTexture
		var got := Color(hover.modulate_color.r * fill.r, hover.modulate_color.g * fill.g,
			hover.modulate_color.b * fill.b)
		_check(got.is_equal_approx(wanted[i]),
			"%s turns its own colour (%s)" % [btn.text, got.to_html(false)])
		var down := btn.get_theme_stylebox("pressed") as StyleBoxTexture
		var pressed := Color(down.modulate_color.r * fill.r, down.modulate_color.g * fill.g,
			down.modulate_color.b * fill.b)
		var darker := Color(wanted[i].r * drop, wanted[i].g * drop, wanted[i].b * drop)
		_check(pressed.is_equal_approx(darker), "%s presses darker than it hovers" % btn.text)
		_check(btn.get_theme_stylebox("focus") is StyleBoxEmpty,
			"%s has no focus fill to cover its press" % btn.text)
	title.queue_free()
	await process_frame


## The cover: real bodies, real collisions, a real settle, then hold and swap.
func _labels(title: Control) -> String:
	var out := PackedStringArray()
	for btn in title._buttons:
		out.append(String(btn.text))
	return ", ".join(out)


## The update log is a card, not floating text: the right-hand sky is the brightest thing on
## the screen, and grey labels laid straight on it fail contrast. So the checks are that the
## card exists where the emptiness was, that each entry fits at the body size (the card clips
## rather than wraps), and that the composite of the card over the brightest sky still carries
## the entries at 4.5:1 - the AA figure for normal text, measured against the brightest frame
## rather than against a convenient one.
func _check_update_log() -> void:
	var title: Control = Title.new()
	root.add_child(title)
	await process_frame
	title.size = Vector2(1920, 1080)
	title.open(true, 12345, "0.1", Color(1.0, 0.76, 0.42), true,
		[{"name": "MY WORLD", "seed": 12345, "when": int(Time.get_unix_time_from_system()) - 7200}])
	# `open` re-takes the size from the viewport, which in a headless run is 64 x 64. The
	# screen's real geometry is the one the window sets, so set it after opening.
	title.size = Vector2(1920, 1080)
	title._layout()
	# The autoload by look-up, not by name, for the same reason as the scrim check.
	var ui: Node = root.get_node_or_null("UIScale")
	var s: float = maxf(float(ui.get("value")) if ui != null else 1.0, 1.0)
	var rect: Rect2 = title._log_rect
	_check(rect.size.x > 0.0, "the update log has a card (%.0f x %.0f)" % [rect.size.x, rect.size.y])
	# The column starts just under the wordmark's underline. At 62 units of air the band read
	# as a missing row, so the gap is bounded here rather than left to the constant alone.
	var underline_y: float = (title._wordmark_y + Title.WORDMARK_UNITS * 1.30) * s
	var gap: float = title._rows_y * s - underline_y
	_check(gap > 0.0 and gap <= 30.0 * s,
		"the column starts under the wordmark, not a third down (%.0f px gap)" % gap)
	_check(not title._log_entries.is_empty(),
		"the log lists the build's entries (%d)" % title._log_entries.size())
	# The card belongs to the top right: on the wordmark's line, on the right margin, in the
	# upper half, and clear of the column - the one part of the screen with nothing behind it
	# but sky, so it balances the wordmark without covering the world.
	_check(rect.position.x > title.size.x * 0.5, "the card is in the right-hand half")
	_check(rect.position.y < title.size.y * 0.5, "the card is in the upper half")
	var column_right: float = (title._margin + Title.COLUMN) * s
	_check(rect.position.x >= column_right,
		"the card sits right of the column (%.0f >= %.0f)" % [rect.position.x, column_right])
	_check(absf(rect.end.x - (title.size.x - title._margin * s)) <= title._cell,
		"the card's right edge is on the margin (%.0f)" % rect.end.x)
	# The log is the second panel in the right-hand stack: the profile card is on the wordmark's
	# line and the log starts under it, one cell down.
	var profile: Rect2 = title._profile_rect
	_check(absf(profile.position.y - title._wordmark_y * s) <= title._cell,
		"the profile card's top is on the wordmark's line (%.0f)" % profile.position.y)
	_check(absf(profile.end.x - (title.size.x - title._margin * s)) <= title._cell,
		"the profile card's right edge is on the margin (%.0f)" % profile.end.x)
	_check(absf(profile.size.x - Title.COLUMN * s) <= title._cell,
		"the profile card is the column's width (%.0f)" % profile.size.x)
	_check(rect.position.y >= profile.end.y,
		"the log sits under the profile card (%.0f >= %.0f)" % [rect.position.y, profile.end.y])
	_check(rect.position.y - profile.end.y <= title._cell * 2.0,
		"they are one stack, not two islands (%.0f apart)" % (rect.position.y - profile.end.y))
	_check(absf(rect.size.x - Title.COLUMN * s) <= title._cell,
		"the card is the column's width (%.0f of %.0f)" % [rect.size.x, Title.COLUMN * s])
	# Snapped to the scrim's grid on every edge, so the panel lines up with the squares behind
	# it rather than cutting across them.
	var cell: float = title._cell
	var off_grid := maxf(maxf(fposmod(rect.position.x, cell), fposmod(rect.position.y, cell)),
		maxf(fposmod(rect.size.x, cell), fposmod(rect.size.y, cell)))
	_check(off_grid < 0.01, "the card's edges land on the square grid (%.2f px off)" % off_grid)
	var body := int(Title.LOG_BODY_UNITS * s)
	var widest := 0.0
	for entry in title._log_entries:
		widest = maxf(widest, Title.MUNRO_FONT.get_string_size(String(entry),
			HORIZONTAL_ALIGNMENT_LEFT, -1, body).x)
	# The bevel is inside the card's rect, so it comes off the text's room as well.
	var room: float = rect.size.x - 4.0 * s - Title.LOG_PAD * s - 6.0 * s
	_check(widest <= room, "every entry fits without clipping (%.0f of %.0f)" % [widest, room])

	# The frame is the button texture's own bevel, so the three tones are read back out of
	# `button.png` rather than trusted: outline from its left edge, the lit edge one texel in,
	# and the shade from its bottom edge (the right edge carries a slightly different one).
	var img: Image = Title.BUTTON_TEX.get_image()
	var mid := img.get_height() / 2
	var edge := img.get_pixel(0, mid)
	var lit := img.get_pixel(1, mid)
	var shade := img.get_pixel(10, img.get_height() - 3)
	_check(edge.is_equal_approx(Title.LOG_EDGE_OUTER),
		"the card's outline is the button's (#%s)" % edge.to_html(false))
	_check(shade.is_equal_approx(Title.LOG_EDGE_SHADE),
		"the card's shade is the button's (#%s)" % shade.to_html(false))
	# The lit tone is the button's, pulled down: at full strength #9b9b9b on a nearly black card
	# reads as a bright outline rather than a bevel. It still has to stay lighter than the
	# shade, or the frame loses the lit-from-above reading that all four edges agree on.
	_check(Title.LOG_EDGE_LIGHT.v < lit.v,
		"the card's lit edge is pulled down from the button's (#%s, was #%s)" % [
			Title.LOG_EDGE_LIGHT.to_html(false), lit.to_html(false)])
	_check(Title.LOG_EDGE_LIGHT.v > Title.LOG_EDGE_SHADE.v,
		"the lit edge still reads lighter than the shade")

	# The brightest sky this world can put behind the card, composited out of the same two
	# numbers the draw uses, then the two luminances turned into a WCAG ratio.
	var sky := Color(0.765, 0.835, 0.945)
	var tint: Color = Title.SCRIM_TINT
	var a: float = Title.LOG_PLATE_ALPHA
	var card := Color(sky.r * (1.0 - a) + tint.r * a, sky.g * (1.0 - a) + tint.g * a,
		sky.b * (1.0 - a) + tint.b * a)
	var ratio := (_relative_luminance(Title.LOG_TEXT) + 0.05) / (_relative_luminance(card) + 0.05)
	_check(ratio >= 4.5, "the entries hold %.2f:1 on the brightest sky" % ratio)

	# The profile card: three rows, real Buttons, inside the card - and each one answers in the
	# footer, because there is nothing behind them yet.
	_check(title._profile_rows.size() == Title.PROFILE_ROWS.size(),
		"the profile card has %d rows" % title._profile_rows.size())
	for i in range(title._profile_rows.size()):
		var row: Button = title._profile_rows[i]
		_check(row.visible and row is Button, "row %d is a live button" % i)
		_check(String(row.text) == String(Title.PROFILE_ROWS[i][0]),
			"row %d is %s" % [i, row.text])
		var inside: Rect2 = Rect2(row.position, row.size)
		_check(profile.encloses(inside), "row %d sits inside the card" % i)
	_check(absf(profile.size.y - (Title.PROFILE_HEADER + 3.0 * Title.PROFILE_ROW_H
		+ Title.PROFILE_PAD) * s) <= title._cell,
		"the card is its header plus three rows (%.0f tall)" % profile.size.y)
	title._on_profile_row(1)
	_check(String(title._footer.text).contains("CAPE"),
		"a profile row answers in the footer: %s" % title._footer.text)
	title._process(Title.NOTE_SECONDS + 0.1)
	_check(String(title._footer.text).contains("v0.1"), "the answer clears itself")

	# The file names the running build, or the card is advertising another one.
	var version := String(ProjectSettings.get_setting("application/config/version", "dev"))
	_check(title._log_version == version,
		"the log's version matches the build (%s / %s)" % [title._log_version, version])
	title.queue_free()
	await process_frame


## WCAG relative luminance: the sRGB curve per channel, then the 0.2126/0.7152/0.0722
## weights. Written out because `Color` has no channelwise decode, and a contrast claim
## computed any other way is a guess.
func _relative_luminance(c: Color) -> float:
	return 0.2126 * _srgb_to_linear(c.r) + 0.7152 * _srgb_to_linear(c.g) \
		+ 0.0722 * _srgb_to_linear(c.b)


func _srgb_to_linear(c: float) -> float:
	return c / 12.92 if c <= 0.04045 else pow((c + 0.055) / 1.055, 2.4)


## The world list is a page of the title screen: SINGLEPLAYER swaps the column, one row per
## save plus BACK, and the footer becomes the selected world's detail instead of the launch's
## seed. Escape is the same gesture as BACK, and a picked world is the layer's business.
func _check_world_page() -> void:
	var title: Control = Title.new()
	root.add_child(title)
	await process_frame
	title.size = Vector2(1920, 1080)
	var world := {"name": "MY WORLD", "seed": 12345,
		"when": int(Time.get_unix_time_from_system()) - 7200}
	title.open(true, 12345, "0.1", Color(1.0, 0.76, 0.42), true, [world])
	title.size = Vector2(1920, 1080)
	title._layout()

	title._on_singleplayer()
	_check(title._page == Title.PAGE_WORLDS, "Singleplayer opens the world list")
	_check(title._buttons.size() == 2, "the list is one world and Back (%s)" % _labels(title))
	if title._buttons.size() == 2:
		_check(String(title._buttons[0].text) == "MY WORLD", "the world row is named")
		_check(String(title._buttons[1].text) == "BACK", "Back is the last row")
	_check(String(title._footer.text).contains("seed 12345"),
		"the footer is the selected world's detail: %s" % title._footer.text)
	_check(String(title._footer.text).contains("2 h ago"),
		"the detail says when it was last saved: %s" % title._footer.text)
	_check(title._buttons[0].has_focus(), "the world row holds focus on the list")

	# The quit row does not exist on this page, so arming it must do nothing rather than
	# index the list's rows.
	title._on_quit()
	_check(not title._quit_armed, "Quit is not armed on a page with no Quit row")

	# A picked world is the same signal as the primary action.
	var hits := [0]
	title.world_chosen.connect(func(): hits[0] += 1)
	title._on_world(0)
	_check(hits[0] == 1, "picking a world asks the layer to enter it")
	title._on_world(7)
	_check(hits[0] == 1, "a world index that is not there does nothing")

	title._on_back()
	_check(title._page == Title.PAGE_TITLE, "Back returns to the title")
	_check(title._buttons.size() == 5, "the title column is back to five rows")
	_check(String(title._footer.text).contains("v0.1"),
		"the footer returns to the launch's own status: %s" % title._footer.text)

	# Multiplayer answers in the footer, and the answer expires on its own.
	title._on_multiplayer()
	_check(String(title._footer.text).contains("MULTIPLAYER"),
		"Multiplayer says it is not here: %s" % title._footer.text)
	title._process(Title.NOTE_SECONDS + 0.1)
	_check(String(title._footer.text).contains("v0.1"),
		"the note clears itself: %s" % title._footer.text)

	# Escape is the same gesture as Back.
	title._on_singleplayer()
	var esc := InputEventAction.new()
	esc.action = "ui_cancel"
	esc.pressed = true
	title._unhandled_key_input(esc)
	_check(title._page == Title.PAGE_TITLE, "Escape leaves the world list")

	# A first run with no save still has a page, and it says so.
	var empty: Control = Title.new()
	root.add_child(empty)
	await process_frame
	empty.size = Vector2(1920, 1080)
	empty.open(false, null, "0.1", Color(1.0, 0.76, 0.42), true, [])
	empty.size = Vector2(1920, 1080)
	empty._layout()
	empty._on_singleplayer()
	_check(empty._buttons.size() == 2 and String(empty._buttons[0].text) == "NO WORLDS YET",
		"no saves reads as no worlds: %s" % _labels(empty))
	empty.queue_free()
	title.queue_free()
	await process_frame


func _check_curtain() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	# One frame before sizing: `_ready` runs on the first idle frame, and it sets the
	# node's own size from the viewport. Sizing and playing before it runs is a race
	# the probe would lose silently, with the whole process loop switched off.
	await process_frame
	curtain.size = Vector2(1920, 1080)

	_check(Curtain.CELL_COLOR == Color(0, 0, 0, 1), "the wall is pure black")

	var hits := [0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.play_cover(false)
	var side: float = 1920.0 / float(Curtain.GRID_COLS)
	var rows: int = int(ceil(1080.0 / side)) + int(Curtain.EXTRA_ROWS)
	_check(curtain._bodies.size() == int(Curtain.GRID_COLS) * rows,
		"the sheet is %d x %d bodies (%d)" % [Curtain.GRID_COLS, rows, curtain._bodies.size()])
	_check(not curtain._bodies.is_empty() and curtain._bodies[0] is RigidBody2D,
		"the blocks are rigid bodies")

	# Tumble, sampled while they are still in the air.
	for i in range(10):
		await physics_frame
	var spun := 0
	for body in curtain._bodies:
		if absf(body.rotation) > 0.02:
			spun += 1
	_check(spun > curtain._bodies.size() / 3,
		"the blocks tumble as they fall (%d of %d)" % [spun, curtain._bodies.size()])

	# The pile settles when the engine says so, not when a timer says so.
	var started := Time.get_ticks_msec()
	while curtain._phase == Curtain.PHASE_COVER and Time.get_ticks_msec() - started < 10000:
		await physics_frame
	var settle_seconds := float(Time.get_ticks_msec() - started) / 1000.0
	_check(curtain._phase == Curtain.PHASE_HOLD, "the pile settles")
	_check(settle_seconds >= 0.3 and settle_seconds < 5.0,
		"the fall decided the timing: %.2f s to settle" % settle_seconds)
	_check(curtain._all_settled(), "every body has landed at the settle")

	# Collision, measured rather than assumed: at least one block is resting on
	# another, and no two are driven into each other. The overlap test is a real
	# separating-axis check, because the pile is free: two blocks side by side in the
	# same row are legitimately level, so "close in y" says nothing on its own.
	var stacked := 0
	var deepest := -INF
	for i in range(curtain._bodies.size()):
		var a: RigidBody2D = curtain._bodies[i]
		for j in range(i + 1, curtain._bodies.size()):
			var b: RigidBody2D = curtain._bodies[j]
			var overlap := _overlap_depth(a, b, side * float(Curtain.PHYS_SCALE))
			deepest = maxf(deepest, overlap)
			if absf(a.position.x - b.position.x) <= side * 0.5 and overlap <= 0.0:
				var dy := absf(a.position.y - b.position.y)
				if dy > side * 0.8 and dy < side * 1.2:
					stacked += 1
	_check(stacked > 0, "blocks rest on blocks (%d stacked pairs)" % stacked)
	# A hard landing is allowed to squash the solver a little - the blocks are black
	# on black, so a few pixels of overlap is invisible - but not to merge them: more
	# than a sixth of a block means the pile is being driven through itself.
	var collider := side * float(Curtain.PHYS_SCALE)
	_check(deepest <= collider * 0.15,
		"no two blocks are driven into each other (deepest %.1f px of %.0f)" % [deepest, collider])

	# The hold: the pile must stand still while the wall closes.
	var settled_at := Time.get_ticks_msec()
	var standing := []
	for body in curtain._bodies:
		standing.append(body.position)
	for i in range(12):
		await physics_frame
	_check(curtain._phase == Curtain.PHASE_HOLD, "still holding after the settle")
	var moved := 0.0
	for i in range(curtain._bodies.size()):
		moved = maxf(moved, (curtain._bodies[i].position - standing[i]).length())
	_check(moved < 0.5, "no block moves during the hold (worst %.2f px)" % moved)

	# The squares swell until the viewport is covered, so the size is measured rather
	# than assumed: it is whatever this pile needed.
	var seal_at := Time.get_ticks_msec()
	while not curtain._sealed and Time.get_ticks_msec() - seal_at < 2500:
		await physics_frame
	var sealed_in := float(Time.get_ticks_msec() - settled_at) / 1000.0
	_check(curtain._sealed,
		"the wall closes while it holds (%.2f s in, squares at %.2f)" % [sealed_in, curtain._draw_scale])
	_check(curtain._draw_scale < float(Curtain.MAX_SCALE),
		"it closed by swelling, not by reaching the ceiling")

	# Filling the screen, exactly: every scanline across the viewport, not a sample grid.
	# A sampled check is what passed a wall with a visible hairline in it.
	var exact: Dictionary = curtain._scanline_coverage(1.0, 0.2)
	_check(int(exact.gaps) == 0 and exact.ratio >= 0.99999,
		"the wall covers every scanline before the swap (%d gaps, %.4f)" % [int(exact.gaps), exact.ratio])

	while hits[0] == 0 and Time.get_ticks_msec() - settled_at < 4000:
		await physics_frame
	var hold_seconds := float(Time.get_ticks_msec() - settled_at) / 1000.0
	_check(hits[0] == 1, "the cover emits covered once (%d)" % hits[0])
	_check(hold_seconds >= 0.5, "the wall stood still for %.2f s before the swap" % hold_seconds)
	_check(curtain._sealed_at >= 0.0 and curtain._hold - curtain._sealed_at >= 0.45,
		"the closed wall itself held still for %.2f s" % (curtain._hold - curtain._sealed_at))
	_check(curtain._grid_up, "the pile is replaced by a lattice before the drop")

	# Reveal: the lattice drops out of frame.
	var revealed := [0]
	curtain.revealed.connect(func(): revealed[0] += 1)
	curtain.play_reveal()
	var reveal_at := Time.get_ticks_msec()
	while revealed[0] == 0 and Time.get_ticks_msec() - reveal_at < 4000:
		await physics_frame
	_check(revealed[0] == 1, "reveal emits revealed once (%d)" % revealed[0])
	_check(float(Time.get_ticks_msec() - reveal_at) / 1000.0 <= 2.0,
		"the pile falls out of frame (%.2f s)" % (float(Time.get_ticks_msec() - reveal_at) / 1000.0))
	_check(not curtain.visible and curtain._bodies.is_empty(),
		"the curtain hides itself and frees the bodies")
	curtain.queue_free()


## The transition cannot strand itself. A wall that will not close is the failure the
## player sees as "nothing happened until I clicked", so the hold has a wall-clock fuse:
## the lattice goes up and the beat runs out anyway. Sabotaged here by moving the whole
## pile off the side of the screen, where no swell can ever cover anything.
func _check_curtain_fuse() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	await process_frame
	curtain.size = Vector2(1280, 720)
	var hits := [0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.play_cover(false)
	var started := Time.get_ticks_msec()
	while curtain._phase != Curtain.PHASE_HOLD and Time.get_ticks_msec() - started < 8000:
		await physics_frame
	for body in curtain._bodies:
		body.position.x = 6000.0
		body.freeze = true
	var covered_in := -1.0
	while hits[0] == 0 and Time.get_ticks_msec() - started < 12000:
		await physics_frame
		covered_in = float(Time.get_ticks_msec() - started) / 1000.0
	_check(hits[0] == 1, "a wall that cannot close still covers, with no input (%.2f s)" % covered_in)
	_check(curtain._grid_up, "the fuse hands over to the lattice")
	_check(hits[0] == 1 and covered_in <= 8.0, "the fuse is bounded (%.2f s)" % covered_in)
	curtain.play_reveal()
	var reveal_at := Time.get_ticks_msec()
	while curtain.visible and Time.get_ticks_msec() - reveal_at < 4000:
		await physics_frame
	_check(not curtain.visible, "the sabotage still ends in a reveal")
	curtain.queue_free()
	await process_frame


## Separating-axis overlap of two rotated squares: how far one is inside the
## other, in pixels. Negative means they are apart.
func _overlap_depth(a: RigidBody2D, b: RigidBody2D, side: float) -> float:
	var right_a := Vector2.RIGHT.rotated(a.rotation)
	var down_a := Vector2.DOWN.rotated(a.rotation)
	var right_b := Vector2.RIGHT.rotated(b.rotation)
	var down_b := Vector2.DOWN.rotated(b.rotation)
	var axes := [right_a, down_a, right_b, down_b]
	var delta := b.position - a.position
	var depth := INF
	for axis in axes:
		var ra: float = (absf(axis.dot(right_a)) + absf(axis.dot(down_a))) * side * 0.5
		var rb: float = (absf(axis.dot(right_b)) + absf(axis.dot(down_b))) * side * 0.5
		depth = minf(depth, ra + rb - absf(delta.dot(axis)))
	return depth


## Skip is the escape hatch, and it is thrown rather than cut: the wall goes up at once
## and comes apart away from where the player pressed.
func _check_curtain_skip() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	await process_frame
	curtain.size = Vector2(1280, 720)
	var hits := [0]
	var revealed := [0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.revealed.connect(func(): revealed[0] += 1)
	curtain.play_cover(false)
	# Pressed while the sheet is still falling: the case that used to skip with nothing
	# on screen to throw.
	for i in range(4):
		await physics_frame
	var press := Vector2(320.0, 240.0)
	curtain.skip(press)
	_check(hits[0] == 1, "skip emits covered at once (%d)" % hits[0])
	_check(curtain._phase == Curtain.PHASE_REVEAL and curtain._grid_up,
		"skip raises the whole wall and starts throwing it")
	var thrown: int = curtain._cells.size()
	_check(thrown > 12, "the blast is the wall, not a few blocks (%d)" % thrown)
	for i in range(8):
		await process_frame
	var away := 0
	for cell in curtain._cells:
		var home: Vector2 = Vector2(cell.x, cell.y) + Vector2(cell.size, cell.size) * 0.5
		if ((home + cell.offset) - press).length() > (home - press).length() + 2.0:
			away += 1
	_check(away >= thrown * 9 / 10,
		"the blocks fly away from the press (%d of %d)" % [away, thrown])
	var started := Time.get_ticks_msec()
	while revealed[0] == 0 and Time.get_ticks_msec() - started < 3000:
		await process_frame
	_check(revealed[0] == 1, "the blast finishes the transition")
	_check(float(Time.get_ticks_msec() - started) / 1000.0 <= 1.6,
		"the blast is quick (%.2f s)" % (float(Time.get_ticks_msec() - started) / 1000.0))
	_check(not curtain.visible, "the curtain hides itself after the blast")
	curtain.queue_free()


## Reduce motion draws no bodies at all and fades both ways, holding the same beat.
func _check_reduce_motion() -> void:
	var curtain: Control = Curtain.new()
	root.add_child(curtain)
	await process_frame
	curtain.size = Vector2(1920, 1080)
	var hits := [0, 0]
	curtain.covered.connect(func(): hits[0] += 1)
	curtain.revealed.connect(func(): hits[1] += 1)
	curtain.play_cover(true)
	var started := Time.get_ticks_msec()
	while hits[0] == 0 and Time.get_ticks_msec() - started < 3000:
		await physics_frame
	_check(hits[0] == 1 and curtain._bodies.is_empty(),
		"reduce motion covers by fading, with no bodies")
	_check(float(Time.get_ticks_msec() - started) / 1000.0 >= 0.5,
		"reduce motion holds the same beat")
	curtain.play_reveal()
	started = Time.get_ticks_msec()
	while hits[1] == 0 and Time.get_ticks_msec() - started < 3000:
		await physics_frame
	_check(hits[1] == 1, "reduce motion reveals by fading back")
	curtain.queue_free()


## The live half: boot the real scene, then drive the menu the way a player does.
func _check_live_menu() -> void:
	var main: Node = (load("res://Main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	current_scene = main
	await process_frame
	await process_frame

	var menu: Node = root.get_node_or_null("MenuLayer")
	_check(menu != null, "the menu layer autoload exists")
	if menu == null:
		return
	await process_frame
	await process_frame

	var player: Node = main.get_node("Player")
	var hotbar: Node = main.get_node("HUD/Hotbar")
	var title: Control = menu._title
	_check(title != null and title.visible, "the title screen is up at boot")
	_check(player.is_settings_open(), "the title freezes the player")
	_check(not hotbar.visible, "the title hides the hotbar")
	var viewport: Vector2 = root.get_visible_rect().size
	_check(title.size == viewport,
		"the title fills the viewport (%.0f x %.0f of %.0f x %.0f)" % [
			title.size.x, title.size.y, viewport.x, viewport.y])

	var primary := String(title._buttons[0].text)
	var has_world: bool = title.has_world()
	_check(primary == ("CONTINUE" if has_world else "PLAY"),
		"the primary action names the save state: %s (world=%s)" % [primary, has_world])
	var live_seed = menu._current_seed()
	_check(String(title._footer.text).contains("seed %d" % int(live_seed)) if live_seed != null
		else String(title._footer.text).contains("new world"),
		"the footer names the world: %s" % title._footer.text)
	_check(title._buttons[0].has_focus(), "the primary action holds focus on open")


	# Quit is two presses: the first arms the row, and it disarms on its own.
	title._on_quit()
	_check(String(title._buttons[title._quit_index].text).begins_with("QUIT?"),
		"the first Quit press only arms it: %s" % title._buttons[title._quit_index].text)
	title._process(3.0)
	_check(String(title._buttons[title._quit_index].text) == "QUIT",
		"an armed Quit disarms itself")

	# Settings hands the screen over and comes back to the title.
	menu._on_settings()
	await process_frame
	await process_frame
	var settings: Node = menu._settings_menu
	_check(settings != null and settings.is_open, "Settings opens the existing menu")
	_check(not title.visible, "the title steps back behind Settings")
	if settings != null:
		settings._close()
	await process_frame
	await process_frame
	_check(title.visible, "closing Settings returns to the title")
	_check(player.is_settings_open(), "the player is still frozen after Settings")

	# Reduce Motion is the transition's accessibility path: the settings menu owns
	# the switch, this layer reads the file. Round-trip it through a real save.
	settings._reduce_motion = true
	settings._save_settings()
	_check(menu._reduce_motion(), "Reduce Motion round-trips through settings.cfg")
	settings._reduce_motion = false
	settings._save_settings()
	_check(not menu._reduce_motion(), "Reduce Motion turns back off")

	# Start: cover, release, reveal.
	menu._on_start()
	var started := Time.get_ticks_msec()
	while menu._busy and Time.get_ticks_msec() - started < 12000:
		await process_frame
	_check(not menu._busy, "the transition finished")
	_check(not player.is_settings_open(), "Play hands the player control back")
	_check(hotbar.visible, "Play restores the hotbar")
	_check(not title.visible, "the title is gone after Play")
	_check(not menu._curtain.visible, "the curtain is gone after the reveal")
	_check_quit_wired()
	# Free the menu's own nodes before quitting as well: they live under the
	# autoload, and a quit from a coroutine leaves a CanvasLayer's children to be
	# reported as leaked at exit, which looks like a defect in the probe.
	menu.queue_free()
	main.queue_free()
	await process_frame
	await process_frame


## Quit is the one action not exercised above: assert the wiring exists instead of
## clicking it, which would end the probe before it reported.
func _check_quit_wired() -> void:
	var menu: Node = root.get_node_or_null("MenuLayer")
	_check(menu._title.quit_pressed.get_connections().size() > 0, "Quit is wired to the layer")
