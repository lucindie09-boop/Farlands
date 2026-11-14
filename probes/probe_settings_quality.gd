extends SceneTree
## Builds the Sky page's rows and round-trips the Render code, to prove the
## God Ray Quality slider exists, ranges 25-200, and survives FR export/import.

func _initialize() -> void:
	_run()

func _run() -> void:
	var main: Node3D = load("res://main.tscn").instantiate()
	root.add_child(main)
	for i in range(40):
		await process_frame
	var menu: Node = main.get_node("HUD/SettingsMenu")
	var found := false
	for section in menu.call("_build_sky_sections"):
		for row in section[1]:
			if String(row[0]) == "God Ray Quality":
				found = true
				var slider: HSlider = row[1].get_meta("slider")
				print("PROBE Q row value=%.0f min=%.0f max=%.0f step=%.0f" % [
					slider.value, slider.min_value, slider.max_value, slider.step])
	print("PROBE Q found=%s" % str(found))
	var overlay: Node = main.get_node("HUD/GodRaysOverlay")
	overlay.quality = 150
	var code: String = menu.call("_export_render_code")
	overlay.quality = 100
	var imported: bool = menu.call("_import_render_code", code)
	for i in range(3):
		await process_frame
	print("PROBE Q code=%s imported=%s quality=%d" % [code, str(imported), overlay.quality])
	quit(0 if found and imported and overlay.quality == 150 else 1)
