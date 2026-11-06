extends Label

# How often the readout repaints, in seconds. The settings menu owns the value
# ("FPS Counter Update Interval"); the node only renders at it, so a slider drag
# changes the cadence on the spot. This default is what the scene starts with and
# what that row's reset restores.
const DEFAULT_UPDATE_INTERVAL: float = 0.25
var update_interval: float = DEFAULT_UPDATE_INTERVAL

# Track elapsed time to avoid updating the text every single frame
var timer: float = 0.0
# Frames drawn since the last repaint. The count is the reading, not the engine's
# Performance.TIME_FPS: that monitor is a rolling average of about a second, so at
# an interval shorter than that it would repeat a stale number, and at a longer one
# it would report the last second rather than the interval that was asked for.
var _frames: int = 0

# The label's font in GUI units: 15 units is the 30 px the scene used to pin, so
# the readout tracks the GUI scale like the rest of the HUD instead of staying
# the same pixel size at every setting.
const FONT_UNITS: float = 15.0

var _last_ui_scale: float = -1.0

var chunk_manager: Node3D
var perf_timer: float = 0.0
const PERF_UPDATE_INTERVAL: float = 2.0 # Update performance report every 2 seconds

func _ready() -> void:
	add_theme_font_override("font", load("res://fonts/munro.ttf"))
	chunk_manager = get_node_or_null("/root/Main/ChunkManager")
	_apply_font_size()

func _apply_font_size() -> void:
	_last_ui_scale = UIScale.value
	add_theme_font_size_override("font_size", int(round(FONT_UNITS * UIScale.value)))

func _process(delta: float) -> void:
	if not is_equal_approx(UIScale.value, _last_ui_scale):
		_apply_font_size()
	timer += delta
	perf_timer += delta
	_frames += 1

	# The floor keeps a hand-edited zero from repainting on every frame; the
	# menu's own slider stops at 0.05 s.
	if timer >= maxf(update_interval, 0.01):
		# Update text string smoothly
		text = "FPS: " + str(int(round(float(_frames) / timer)))

		# Reset the window the next reading will average over
		timer = 0.0
		_frames = 0

	# Print performance report to console periodically
	if perf_timer >= PERF_UPDATE_INTERVAL and chunk_manager:
		print(chunk_manager.get_performance_report())
		perf_timer = 0.0
