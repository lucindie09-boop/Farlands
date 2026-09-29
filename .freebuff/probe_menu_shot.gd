extends SceneTree
## Screenshots + metrics for the settings menu, so a layout change can be looked
## at instead of guessed at.
##
## Runs WINDOWED (the dummy driver in --headless has no viewport texture to
## capture), so run it through .freebuff/run_probe_shot.sh, which snapshots and
## restores user:// around it.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_menu_shot.gd [width] [height]
##
## Then read .freebuff/menu_shots/*.png.

const SHOT_DIR := "user://menu_shots"
const PAGES := ["pause", "settings", "controls", "shaders", "tools", "skin_maker", "block_maker"]

var menu: Control = null


func _initialize() -> void:
	_run()


func _run() -> void:
	var size := Vector2i(1280, 720)
	if OS.get_environment("PROBE_W").is_valid_int():
		size.x = int(OS.get_environment("PROBE_W"))
	if OS.get_environment("PROBE_H").is_valid_int():
		size.y = int(OS.get_environment("PROBE_H"))
	DisplayServer.window_set_size(size)

	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	menu = main.get_node_or_null("HUD/SettingsMenu")
	if menu == null:
		print("PROBE FAIL: no SettingsMenu node under HUD")
		quit(1)
		return

	# Let the world land and a few frames render before looking at anything.
	for i in range(60):
		await process_frame

	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	menu.call("_open")
	# Autoloads are reachable by node, not by identifier, from a --script probe.
	var ui_scale: Node = root.get_node_or_null("UIScale")
	print("probe: window %s  ui_scale %.2f  menu_scale %.2f"
		% [DisplayServer.window_get_size(), ui_scale.get("value") if ui_scale else -1.0,
		   menu.call("_ui_scale")])

	for page in PAGES + shader_page_names():
		menu.call("_show_page", page)
		for i in range(4):
			await process_frame
		var img: Image = root.get_texture().get_image()
		if img == null:
			print("PROBE FAIL: no viewport image (running headless?)")
			quit(1)
			return
		var path := "%s/%s_%dx%d.png" % [SHOT_DIR, page, size.x, size.y]
		img.save_png(path)
		print("probe: shot %s" % path)
		_report(page)
		check_metrics(page)
		check_bars(page)
		if page == "settings":
			await _shoot_settings_scroll(size)

	check_scroll()
	print("probe: %d failures" % _failures)
	quit(1 if _failures > 0 else 0)


## The bars the pages hang their title and their actions on: one cobblestone tile
## is BAR_TILE_SCALE interface units wide (16 px of block texture x 2.5), tiled
## and repeated across the whole strip. The bare pause menu has no bars at all.
const BAR_TILE_UNITS := 40.0
# No page wears a bar any more: the title and the Back/Done actions stand
# directly on the dimmed world, which is why every page here is bare. The check
# stays because a bar creeping back onto one page is exactly the regression it
# was written for.
const BARRED_PAGES: Array = []

func check_bars(page_name: String) -> void:
	var page: Control = menu.get("_pages")[page_name]
	var u: float = menu.call("_ui_scale")
	var bars: Array = []
	_collect_bars(page, bars)
	if not BARRED_PAGES.has(page_name):
		if not bars.is_empty():
			_fail("%s: found %d bar(s) on a page that should have none" % [page_name, bars.size()])
		return
	if bars.is_empty():
		_fail("%s: no title/action bar" % page_name)
		return
	for bar in bars:
		var tex: Texture2D = bar.texture
		if tex == null:
			_fail("%s: a bar has no texture" % page_name)
			continue
		if absf(tex.get_width() - BAR_TILE_UNITS * u) > 0.5 or tex.get_width() != tex.get_height():
			_fail("%s: bar tile is %s, want a %s square" % [page_name, tex.get_size(), BAR_TILE_UNITS * u])
		if bar.stretch_mode != TextureRect.STRETCH_TILE:
			_fail("%s: bar is not tiled" % page_name)
		if bar.texture_repeat != CanvasItem.TEXTURE_REPEAT_ENABLED:
			_fail("%s: bar does not repeat its texture" % page_name)


func _collect_bars(node: Node, out: Array) -> void:
	for child in node.get_children():
		if child is TextureRect and is_equal_approx(child.anchor_right, 1.0):
			out.append(child)
		_collect_bars(child, out)


## GUI scale 2 means a widget is 20 interface units tall with 8-unit text, so at
## ui_scale 2 every widget this probe can see is 40 px tall with 16 px text and a
## width that is one of the interface widths times the scale. Anything else is
## the "some buttons are a different size" bug.
const UNIT_FONT := 8.0
const UNIT_H := 20.0
const WIDTHS := [20.0, 60.0, 100.0, 150.0, 170.0, 180.0, 200.0, 400.0]
## The settings page carries exactly these category headings — a sub-heading
## between them is the "subtitles" the layout dropped. Controls is its own page.
const CATEGORY_HEADINGS := ["General", "Block Outline", "Crosshair",
	"Advanced Rendering", "Render"]
const CONTROLS_HEADINGS := ["Controls"]
const TOOLS_HEADINGS: Array = []

var _failures := 0


func check_metrics(page_name: String) -> void:
	var u: float = menu.call("_ui_scale")
	var page: Control = menu.get("_pages")[page_name]
	var widgets: Array = []
	_collect(page, widgets, true)
	for w in widgets:
		var rect: Rect2 = w["rect"]
		var font: float = w["font"]
		if absf(font - UNIT_FONT * u) > 0.5:
			_fail("%s: \"%s\" has font %s, want %s" % [page_name, w["text"], font, UNIT_FONT * u])
		# An icon button declares its own square size (the row reset is 20 units,
		# the heading icons 75% of that), so check it is square at that width.
		var icon_w: float = w.get("icon_w", 0.0)
		if icon_w > 0.0:
			if absf(rect.size.x - icon_w * u) > 0.5 or absf(rect.size.y - icon_w * u) > 0.5:
				_fail("%s: icon \"%s\" is %s, want a %s square"
					% [page_name, w["text"], rect.size, icon_w * u])
			continue
		if absf(rect.size.y - UNIT_H * u) > 0.5:
			_fail("%s: \"%s\" is %s tall, want %s" % [page_name, w["text"], rect.size.y, UNIT_H * u])
		# An expand-fill widget legitimately takes the space its row has left, so
		# only fixed-width widgets have to land on an interface width.
		if w["expand"]:
			continue
		var ok := false
		for width in WIDTHS:
			if absf(rect.size.x - width * u) <= 0.5:
				ok = true
		if not ok:
			_fail("%s: \"%s\" is %s wide, not an interface width" % [page_name, w["text"], rect.size.x])


## The point of the layout is that a page longer than the box scrolls instead of
## running off the screen: every settings page must own a vertical-only
## ScrollContainer, and the long ones must have something to scroll.
# The settings page is one long list now, so shoot it at four scroll positions:
# a category that only exists below the fold is otherwise never looked at.
func _shoot_settings_scroll(size: Vector2i) -> void:
	var scroll := _find_scroll(menu.get("_pages")["settings"])
	if scroll == null:
		_fail("settings: no ScrollContainer")
		return
	var span: float = maxf(0.0, scroll.get_v_scroll_bar().max_value - scroll.size.y)
	if span <= 0.0:
		_fail("settings: nothing to scroll (all categories should not fit)")
		return
	for pct in [25, 50, 75, 100]:
		scroll.scroll_vertical = int(round(span * float(pct) / 100.0))
		for i in range(3):
			await process_frame
		var img: Image = root.get_texture().get_image()
		if img != null:
			img.save_png("%s/settings_scroll%02d_%dx%d.png" % [SHOT_DIR, pct, size.x, size.y])
	scroll.scroll_vertical = 0


func check_scroll() -> void:
	for page_name in ["settings", "controls", "shaders", "tools"] + shader_page_names():
		var page: Control = menu.get("_pages")[page_name]
		var want: Array = CATEGORY_HEADINGS
		if page_name == "controls":
			want = CONTROLS_HEADINGS
		elif page_name == "tools":
			want = TOOLS_HEADINGS
		elif page_name == "shaders":
			want = shader_headings()
		elif page_name.begins_with("shader_"):
			# A shader's own page is rows with no heading above them: the page's
			# title already names the shader.
			want = []
		check_headings(page_name, page, want)
		var scroll := _find_scroll(page)
		if scroll == null:
			_fail("%s: no ScrollContainer" % page_name)
			continue
		if scroll.horizontal_scroll_mode != ScrollContainer.SCROLL_MODE_DISABLED:
			_fail("%s: scrolls horizontally" % page_name)
		var content: Control = scroll.get_child(0)
		if content.get_combined_minimum_size().y > scroll.size.y + 0.5:
			if scroll.get_v_scroll_bar().max_value <= scroll.size.y:
				_fail("%s: content overflows but the bar cannot move" % page_name)

## The shaders page's headings: the category and nothing else, because each
## shader's uniforms live on a page of their own (opened by the icon button on
## its row), so one shader's knobs cannot bury the next shader.
func shader_headings() -> Array:
	return ["Shader Effects"]


## The page a shader's own uniforms live on, one per registry entry: its name is
## the registry's `id`, so adding a shader is a JSON entry and nothing else.
func shader_page_names() -> Array:
	var out: Array = []
	var overlay: Node = menu.get_node_or_null("../ShaderOverlay")
	if overlay == null:
		return out
	for definition in overlay.call("get_definitions"):
		out.append("shader_" + String(definition["id"]))
	return out


## Every heading on a settings page, in order, must be its category list: no
## group heading above one, none between the categories.
func check_headings(page_name: String, page: Control, want: Array) -> void:
	var seen: Array = []
	_walk_headings(page, seen)
	if seen != want:
		_fail("%s: headings are %s, want %s" % [page_name, seen, want])


func _walk_headings(node: Node, out: Array) -> void:
	for child in node.get_children():
		if child is Label and String(child.text) != "" and child.has_meta("is_heading"):
			out.append(String(child.text))
		_walk_headings(child, out)


func _find_scroll(node: Node) -> ScrollContainer:
	for child in node.get_children():
		if child is ScrollContainer:
			return child
		var found := _find_scroll(child)
		if found != null:
			return found
	return null


func _fail(message: String) -> void:
	_failures += 1
	print("PROBE FAIL: %s" % message)


## Every Button on the page, with the numbers that say whether it matches the
## interface size every other widget is supposed to share.
func _report(page_name: String) -> void:
	var page: Control = menu.get("_pages")[page_name]
	var rows: Array = []
	_collect(page, rows)
	var title := ""
	var fonts := {}
	for row in rows:
		fonts[int(row["font"])] = int(fonts.get(int(row["font"]), 0)) + 1
	print("probe: %s has %d widgets, font sizes %s" % [page_name, rows.size(), fonts])
	if rows.size() > 0:
		var first: Dictionary = rows[0]
		print("probe:   first %s at %s size %s text \"%s\""
			% [first["kind"], first["rect"].position, first["rect"].size, first["text"]])


## `ours_only` skips widgets this menu does not size itself: the colour picker's
## C++-built internals and the gallery's card-sized click targets.
func _collect(node: Node, out: Array, ours_only := false) -> void:
	if ours_only and (node is ColorPicker or node is ColorPickerButton
			or String(node.name).begins_with("Card_")):
		return
	for child in node.get_children():
		if ours_only and child is Button and child.flat:
			continue
		if child is Button or child is HSlider:
			var font := 0.0
			if child is Button:
				font = child.get_theme_font_size("font_size")
			else:
				font = child.get_theme_font_size("font_size")
			out.append({
				"kind": "slider" if child is HSlider else "button",
				"rect": child.get_global_rect(),
				"text": child.text if child is Button else "",
				"font": font,
				"expand": bool(child.size_flags_horizontal & Control.SIZE_EXPAND_FILL),
				"icon_w": float(child.get_meta("icon_w", 0.0)) if child is Button else 0.0,
			})
		_collect(child, out, ours_only)
