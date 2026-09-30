extends Label

# Track elapsed time to avoid updating the text every single frame
var timer: float = 0.0
const UPDATE_INTERVAL: float = 0.25 # Update 4 times a second

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
	
	if timer >= UPDATE_INTERVAL:
		# Performance.get_monitor fetches the exact engine metrics
		var current_fps = Performance.get_monitor(Performance.TIME_FPS)
		
		# Update text string smoothly
		text = "FPS: " + str(int(current_fps))
		
		# Reset timer tracking loop
		timer = 0.0
	
	# Print performance report to console periodically
	if perf_timer >= PERF_UPDATE_INTERVAL and chunk_manager:
		print(chunk_manager.get_performance_report())
		perf_timer = 0.0
