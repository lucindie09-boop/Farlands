extends SceneTree
## Windowed probe for the wand's middle-click menu: it OPENS it and measures where
## the panel and every widget inside it landed.
##
## There is no screenshot to read here (a stray PNG proves nothing about layout),
## so this checks the two things the change is about, numerically:
##
##   - the panel is centred: the gap on the left equals the gap on the right, and
##     the gap above equals the gap below
##   - the panel is large, and NOTHING inside it hangs outside its rect, on either
##     page, because a widget wider than its panel is a clipped menu
##
##   PROBE_W=1920 PROBE_H=1080 .freebuff/run_probe_shot.sh .freebuff/probe_wand_shot.gd [timeout]

var wand: Node = null
var failures := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	var size := Vector2i(1920, 1080)
	if OS.get_environment("PROBE_W").is_valid_int():
		size.x = int(OS.get_environment("PROBE_W"))
	if OS.get_environment("PROBE_H").is_valid_int():
		size.y = int(OS.get_environment("PROBE_H"))
	# A window is required, not just a scene: --headless gives a 64x64 dummy
	# viewport that ignores window_set_size (assigning root.size does not take
	# either), and every number here is about where things land in a real window.
	# run_probe_shot.sh refuses to start while the player's game is running, which
	# is the right trade - the alternative used to delete their user:// folder.
	DisplayServer.window_set_size(size)

	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	wand = main.get_node_or_null("HUD/Wand")
	if wand == null:
		print("PROBE FAIL: no Wand node under HUD")
		quit(1)
		return

	for i in range(60):
		await process_frame

	var ui_scale: Node = root.get_node_or_null("UIScale")
	print("probe: window %s  ui_scale %s" % [DisplayServer.window_get_size(), ui_scale.get("value") if ui_scale else -1.0])

	wand.call("open_menu")
	for i in range(8):
		await process_frame
	_report("functions")
	await _page("files")

	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: " + message)


## The panel is found by walking the menu, not by a child index: it sits inside
## a centring container, and an index would break the moment that changes.
func _panel() -> Control:
	var menu: Control = wand.get_node_or_null("WandMenu")
	if menu == null:
		return null
	return _find_panel(menu)


func _find_panel(node: Node) -> Control:
	for child in node.get_children():
		if child is PanelContainer:
			return child
		var found := _find_panel(child)
		if found != null:
			return found
	return null


## Switches the menu to another page and measures that one too: both pages are
## built from the same panel, and a difference between them is the bug.
func _page(name: String) -> void:
	wand.set("_menu_page", name)
	wand.call("_refresh_menu")
	for i in range(8):
		await process_frame
	_report(name)


func _report(label: String) -> void:
	var menu: Control = wand.get_node_or_null("WandMenu")
	var panel := _panel()
	if menu == null or panel == null:
		_fail("%s: no menu or panel" % label)
		return
	var view := root.get_visible_rect().size
	var r := panel.get_global_rect()
	var views := Vector2(view)
	print("probe: %s panel=%.0fx%.0f at (%.0f,%.0f)  viewport=%s"
		% [label, r.size.x, r.size.y, r.position.x, r.position.y, views])
	print("probe: %s gaps: left=%.0f right=%.0f top=%.0f bottom=%.0f  visible=%s"
		% [label, r.position.x, views.x - r.end.x, r.position.y, views.y - r.end.y, menu.visible])

	var out: Array[Control] = []
	_collect(panel, out)

	# The close is a SQUARE icon button, at the right-hand end of the tab row: an
	# icon stretched onto a non-square button stops being the icon, which is why
	# its own size is asserted rather than just its presence.
	var close_btn: Button = menu.find_child("MenuClose", true, false) as Button
	if close_btn == null:
		_fail("%s: no close button" % label)
	else:
		var cir := close_btn.get_global_rect()
		print("probe: %s close icon %.0fx%.0f at (%.0f,%.0f) right_inset=%.0f"
			% [label, cir.size.x, cir.size.y, cir.position.x, cir.position.y, r.end.x - cir.end.x])
		if absf(cir.size.x - cir.size.y) > 1.0:
			_fail("%s: the close button is %.0fx%.0f, not square" % [label, cir.size.x, cir.size.y])
		if cir.position.y - r.position.y > r.size.y * 0.15:
			_fail("%s: the close button is %.0f px down the panel" % [label, cir.position.y - r.position.y])

	# Every button is drawn with the root's texture filter, and that filter has to
	# be NEAREST: the interface art is pixel art and the canvas default is linear,
	# which is a menu that looks blurry rather than a menu that looks wrong, so
	# nothing else would catch it.
	if menu.texture_filter != CanvasItem.TEXTURE_FILTER_NEAREST:
		_fail("%s: the menu root filters textures %d, not NEAREST" % [label, menu.texture_filter])
	for control in out:
		if control is Button and control.texture_filter not in [
				CanvasItem.TEXTURE_FILTER_PARENT_NODE, CanvasItem.TEXTURE_FILTER_NEAREST]:
			_fail("%s: button '%s' overrides the filter with %d"
				% [label, (control as Button).text, control.texture_filter])
	# Every CARD wears the square button art as its background, stretched and NOT
	# 9-sliced: the bevel is meant to scale with the card, which only happens if
	# the whole 20x20 image is stretched. A card left on a flat stylebox, or on a
	# 9-slice whose 1 px margins stay 1 px, both read as a different interface.
	var cards := 0
	for control in out:
		if not (control is Button):
			continue
		var cs: Vector2 = (control as Button).get_global_rect().size
		if absf(cs.x - cs.y) > 1.0 or cs.x < 100.0:
			continue   # the tabs and the icon button are not cards
		cards += 1
		var box: StyleBox = (control as Button).get_theme_stylebox("normal")
		if not (box is StyleBoxTexture):
			_fail("%s: card '%s' has no texture background" % [label, (control as Button).text])
			continue
		var art := box as StyleBoxTexture
		if art.texture == null or art.texture.resource_path != "res://textures/gui/button_square_large.png":
			_fail("%s: card '%s' uses %s, not button_square_large.png"
				% [label, (control as Button).text, art.texture.resource_path if art.texture else "<null>"])
		if art.texture_margin_left > 0.0 or art.texture_margin_right > 0.0 \
				or art.texture_margin_top > 0.0 or art.texture_margin_bottom > 0.0:
			_fail("%s: card '%s' is 9-sliced, so its bevel will not scale with it"
				% [label, (control as Button).text])
		print("probe: %s card '%s' background=%s %.0fx%.0f"
			% [label, (control as Button).text, art.texture.resource_path.get_file(), cs.x, cs.y])
		# The card clips its text, so a card shrunk below what its label needs is a
		# name that silently loses its tail. Measured with the font the card draws
		# with rather than assumed, since the two only agree by accident.
		var font: Font = (control as Button).get_theme_font("font")
		var font_size: int = (control as Button).get_theme_font_size("font_size")
		var need: float = font.get_string_size((control as Button).text,
			HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
		print("probe: %s card '%s' label %.0f px in %.0f px" % [label, (control as Button).text, need, cs.x])
		if need > cs.x:
			_fail("%s: card '%s' needs %.0f px for its name in a %.0f px card"
				% [label, (control as Button).text, need, cs.x])
	print("probe: %s cards=%d" % [label, cards])
	if cards == 0:
		_fail("%s: no card was found to check" % label)

	# The panel is a pane, not a wall: half transparent, so the world behind it
	# stays readable. The stylebox is asked rather than the constant, because it
	# is the stylebox the renderer draws.
	var panel_style: StyleBox = panel.get_theme_stylebox("panel")
	if panel_style == null:
		_fail("%s: the panel has no stylebox" % label)
	else:
		var alpha: float = panel_style.bg_color.a
		print("probe: %s panel background alpha=%.2f" % [label, alpha])
		if alpha <= 0.0 or alpha >= 1.0:
			_fail("%s: the panel background is opaque (alpha %.2f)" % [label, alpha])
	if not menu.visible:
		_fail("%s: the menu is not visible" % label)
	if absf(r.position.x - (views.x - r.end.x)) > 1.0:
		_fail("%s: horizontally off centre by %.0f px" % [label, r.position.x - (views.x - r.end.x)])
	if absf(r.position.y - (views.y - r.end.y)) > 1.0:
		_fail("%s: vertically off centre by %.0f px" % [label, r.position.y - (views.y - r.end.y)])
	if r.position.x < 0.0 or r.position.y < 0.0 or r.end.x > views.x or r.end.y > views.y:
		_fail("%s: the panel runs off the window: %s" % [label, r])

	# Every widget inside the panel, against the panel's own rect: a card or a row
	# wider than the panel is a menu that draws past its border. Anything inside a
	# ScrollContainer is exempt - being taller than the panel is what a scrolling
	# list is FOR, and the container clips it - so those are reported, not judged.
	var buttons := 0
	for control in out:
		var cr := control.get_global_rect()
		var scrolled := _inside_scroll(control, panel)
		if scrolled:
			print("probe: %s  (scrolled) %s '%s' = %.0fx%.0f"
				% [label, control.get_class(), control.text if control is Button else "", cr.size.x, cr.size.y])
		elif cr.size.x > r.size.x + 1.0 or cr.size.y > r.size.y + 1.0:
			_fail("%s: %s is %s, bigger than the panel %s" % [label, control.get_class(), cr.size, r.size])
		elif cr.position.x < r.position.x - 1.0 or cr.end.x > r.end.x + 1.0 \
				or cr.position.y < r.position.y - 1.0 or cr.end.y > r.end.y + 1.0:
			_fail("%s: %s at %s hangs outside the panel %s" % [label, control.get_class(), cr, r])
		if control is Button:
			buttons += 1
			print("probe: %s  %s '%s' = %.0fx%.0f at (%.0f,%.0f)"
				% [label, control.get_class(), control.text, cr.size.x, cr.size.y, cr.position.x, cr.position.y])
	print("probe: %s widgets=%d buttons=%d" % [label, out.size(), buttons])

	# The tabs are the top of the panel and they ARE the page title, so there is
	# no title label to check: both tabs exist, and they sit at the top.
	var tabs: Array[Control] = []
	for control in out:
		if control is Button and (control as Button).text == "BUILDING TOOLS" \
				or control is Button and (control as Button).text == "SCHEMATICS":
			tabs.append(control)
	if tabs.size() != 2:
		_fail("%s: found %d of the two tabs" % [label, tabs.size()])
	else:
		var tab_top: float = min(min(tabs[0].get_global_rect().position.y,
			tabs[1].get_global_rect().position.y), INF)
		print("probe: %s tabs at y=%.0f, %.0f px below the panel top"
			% [label, tab_top, tab_top - r.position.y])
		if tab_top - r.position.y > r.size.y * 0.15:
			_fail("%s: the tabs start %.0f px down a %.0f px panel"
				% [label, tab_top - r.position.y, r.size.y])

	# The state line: the bottom left corner, which is the whole point of where it
	# is - it is what the menu is about, so it stays put while the tabs change.
	var state := _label_starting(panel, "build:")
	if state == null:
		_fail("%s: no build line" % label)
		return
	var sr := state.get_global_rect()
	print("probe: %s build line '%s'=%.0fx%.0f at (%.0f,%.0f) left_inset=%.0f bottom_inset=%.0f"
		% [label, state.text, sr.size.x, sr.size.y, sr.position.x, sr.position.y,
		   sr.position.x - r.position.x, r.end.y - sr.end.y])
	if sr.position.x - r.position.x > 60.0:
		_fail("%s: the build line is %.0f px in from the panel's left edge"
			% [label, sr.position.x - r.position.x])
	if r.end.y - sr.end.y > r.size.y * 0.15:
		_fail("%s: the build line ends %.0f px above the panel's bottom"
			% [label, r.end.y - sr.end.y])


## The first label whose text starts with `prefix`, spacers aside.
func _label_starting(panel: Control, prefix: String) -> Label:
	var labels: Array[Control] = []
	_collect(panel, labels)
	for control in labels:
		if control is Label and (control as Label).text.begins_with(prefix):
			return control
	return null


## True when the control is clipped by a ScrollContainer between it and the panel.
func _inside_scroll(control: Control, panel: Control) -> bool:
	var node: Node = control.get_parent()
	while node != null and node != panel:
		if node is ScrollContainer:
			return true
		node = node.get_parent()
	return false


func _collect(node: Node, out: Array[Control]) -> void:
	for child in node.get_children():
		if child is Control:
			out.append(child)
		_collect(child, out)
