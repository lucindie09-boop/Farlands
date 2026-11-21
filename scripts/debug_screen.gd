extends Control

# F3 debug screen, in three sections -- Minecraft's own screen, split by what the
# numbers are about:
#
#   GENERAL      how the game is running: frame rate, body position, speed.
#   PERFORMANCE  what this machine is spending on that frame: frame time, CPU and
#                GPU utilization, VRAM in use, and the engine's share of memory.
#   SPECS        what the machine is: CPU, GPU, RAM and the build's version.
#
# F3 shows it, F3 again hides it, and a launch starts with it hidden. Session
# state only, like the chunk borders: a debug readout is not a setting.
#
# Cadence, and why it is split this way:
#
#   GENERAL,     recomposed once per simulation tick (READOUT_INTERVAL, 20 times
#   PERFORMANCE  a second), which is as often as any of them can change: the
#                body's position, its speed, the OS counters and the memory gauge
#                are all still between two ticks, so a per-frame refresh would
#                repaint the same numbers 5 to 50 times over. A refresh that
#                lands between two ticks simply repeats the previous numbers.
#   FPS, frame   counted over a whole second and published once per window. A
#                shorter window is a jitter rather than a rate, and a longer one
#                reports a number from a second ago. The opening reading is the
#                engine's rolling average so the lines are not blank for that
#                first second.
#   SPECS        read once in _ready; hardware and build numbers cannot change
#                while the game runs.
#
# CPU and GPU utilization are the one thing Godot cannot report: Performance has
# no such monitor, and its TIME_PROCESS is a rolling average of the frame rather
# than a busy share. They come from the native SystemStats object this extension
# registers (src/debug/system_stats_*): GetSystemTimes and the PDH "GPU Engine"
# counters on Windows -- the counters Task Manager reads -- and /proc/stat plus
# the DRM busy file on Linux. An older DLL without the binding leaves both rows
# at "-" rather than breaking the screen.
#
# The separate FPS counter is hidden while this is up: it sits in the same corner
# and this screen already reports the frame rate.

const TOGGLE_ACTION := "toggle_debug_screen"

# The text in GUI units: 8 units is the settings menu's label size, small enough
# for the whole readout in the corner and a whole number of pixels at every GUI
# scale (the font is the project's own munro, so a fractional size resamples it).
const FONT_UNITS := 8.0
const OUTLINE_UNITS := 1.0
const MARGIN_UNITS := 3.0
# Between one section and the next, in units. The blank line is the section break;
# the heading above each one names it.
const SECTION_GAP_UNITS := 5.0

const FPS_WINDOW := 1.0

# PlayerSim steps at a fixed 20 ticks per second (AGENTS.md, Player physics) and
# its velocity is per tick, so blocks per second is the magnitude times this.
const TICKS_PER_SECOND := 20.0

# The readouts the simulation feeds are sampled at its own rate, one refresh per
# tick. Every one of them is constant between two ticks.
const READOUT_INTERVAL := 1.0 / TICKS_PER_SECOND

const ROW_COLOR := Color(1, 1, 1, 1)
# Headings are dimmer than their rows: the section break has to read at a glance
# without competing with the numbers under it.
const HEADING_COLOR := Color(0.70, 0.74, 0.80, 1)
const OUTLINE_COLOR := Color(0, 0, 0, 0.85)

const FONT: Font = preload("res://fonts/munro.ttf")

var _left_column: VBoxContainer
var _right_column: VBoxContainer

var _general_heading: Label
var _general_body: Label
var _performance_heading: Label
var _performance_body: Label
var _specs_heading: Label
var _specs_body: Label

# The native CPU/GPU sampler, or null on a build without the binding.
var _stats: RefCounted = null

# SPECS, composed once.
var _cpu_line := ""
var _gpu_line := ""
var _ram_line := ""
var _version_line := ""

# GENERAL, refreshed every tick.
var _fps_line := ""
# PERFORMANCE: the frame time is published with the frame rate, the rest every
# tick.
var _frame_line := ""
var _memory_line := "Mem: -"
var _vram_line := "VRAM: -"
var _vram_peak_mb := 0.0

var _player: Node3D = null
var _fps_counter: CanvasItem = null
# Whether the FPS counter was visible when this screen was opened, so closing it
# restores what was there rather than assuming it should show.
var _fps_counter_was_visible := true

var _open := false
var _frames := 0
var _window := 0.0
var _frame_ms_total := 0.0
var _fps := 0
var _readout_timer := 0.0
var _last_scale := -1.0

func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	# Found relative to this node rather than by an absolute path: the counter is
	# the HUD sibling this screen replaces while it is up, and the player is the
	# HUD's own sibling (HUD -> Main -> Player), so a probe's fixture works too.
	_fps_counter = get_parent().get_node_or_null("FPSCounter") as CanvasItem
	var main := get_parent().get_parent()
	if main != null:
		_player = main.get_node_or_null("Player") as Node3D
	_build_sections()
	_build_specs_lines()
	_stats = _create_stats()
	_apply_scale()
	visible = false
	set_process(false)

func _unhandled_input(event: InputEvent) -> void:
	if not event.is_action_pressed(TOGGLE_ACTION) or event.is_echo():
		return
	# Gameplay only, like the other debug keys (chunk_borders.gd): with the mouse
	# released the key belongs to whatever is open.
	if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	set_open(not _open)

func _process(delta: float) -> void:
	if not is_equal_approx(UIScale.value, _last_scale):
		_apply_scale()
	_frames += 1
	_window += delta
	_frame_ms_total += delta * 1000.0
	if _window >= FPS_WINDOW:
		_fps = int(round(float(_frames) / _window))
		_fps_line = "FPS: %d" % _fps
		_frame_line = "Frame: %.2f ms" % (_frame_ms_total / float(maxi(_frames, 1)))
		_frames = 0
		_window = 0.0
		_frame_ms_total = 0.0
	_readout_timer += delta
	if _readout_timer >= READOUT_INTERVAL:
		# Subtract rather than zero, so the refresh keeps the tick rate's phase on
		# average instead of drifting a little later every frame. One refresh is
		# enough however late it is: two ticks in one frame is one visible step,
		# because the latest state is the only one there is to read.
		_readout_timer = maxf(_readout_timer - READOUT_INTERVAL, 0.0)
		_refresh()

## Toggle from code as well as from the key (a probe drives this).
func set_open(on: bool) -> void:
	if on == _open:
		return
	_open = on
	visible = on
	set_process(on)
	if _fps_counter != null:
		# The counter and this screen share the corner, and this screen reports
		# the frame rate: two readouts over one another is not a reading.
		if on:
			_fps_counter_was_visible = _fps_counter.visible
			_fps_counter.visible = false
		else:
			_fps_counter.visible = _fps_counter_was_visible
	if not on:
		return
	# The current numbers, not the last ones from the previous time it was up.
	# The rate and frame time open on the engine's rolling average so the lines
	# are readings from the first frame; the first counted window replaces them.
	var engine_fps := maxf(float(Engine.get_frames_per_second()), 1.0)
	_fps = int(engine_fps)
	_fps_line = "FPS: %d" % _fps
	_frame_line = "Frame: %.2f ms" % (1000.0 / engine_fps)
	_frames = 0
	_window = 0.0
	_frame_ms_total = 0.0
	_readout_timer = 0.0
	_refresh()

func is_open() -> bool:
	return _open

# ---------------------------------------------------------------------------
# Sections
# ---------------------------------------------------------------------------

## GENERAL and PERFORMANCE stack on the left; SPECS hangs on the right edge. The
## two columns are containers rather than hand-placed labels, so a section's
## height follows its own text.
func _build_sections() -> void:
	_left_column = _make_column()
	_right_column = _make_column()
	# The right column is anchored to the screen's right edge and grows leftwards,
	# so its right-aligned rows share one edge however long each one is.
	_right_column.anchor_left = 1.0
	_right_column.anchor_right = 1.0
	_right_column.grow_horizontal = Control.GROW_DIRECTION_BEGIN

	_general_heading = _make_heading("GENERAL", false)
	_general_body = _make_body(false)
	_performance_heading = _make_heading("PERFORMANCE", false)
	_performance_body = _make_body(false)
	_specs_heading = _make_heading("SPECS", true)
	_specs_body = _make_body(true)

	_add_section(_left_column, _general_heading, _general_body)
	_add_section(_left_column, _performance_heading, _performance_body)
	_add_section(_right_column, _specs_heading, _specs_body)

func _make_column() -> VBoxContainer:
	var column := VBoxContainer.new()
	column.mouse_filter = Control.MOUSE_FILTER_IGNORE
	column.add_theme_constant_override("separation", 0)
	add_child(column)
	return column

## A heading and the rows under it are one section: their own container, so the
## gap between sections is the column's separation and the heading cannot drift
## away from its body.
func _add_section(column: VBoxContainer, heading: Label, body: Label) -> void:
	var section := VBoxContainer.new()
	section.mouse_filter = Control.MOUSE_FILTER_IGNORE
	section.add_theme_constant_override("separation", 0)
	section.add_child(heading)
	section.add_child(body)
	column.add_child(section)

func _make_heading(text: String, right_aligned: bool) -> Label:
	var label := _make_label(right_aligned)
	label.text = text
	label.add_theme_color_override("font_color", HEADING_COLOR)
	return label

func _make_body(right_aligned: bool) -> Label:
	var label := _make_label(right_aligned)
	label.add_theme_color_override("font_color", ROW_COLOR)
	return label

func _make_label(right_aligned: bool) -> Label:
	var label := Label.new()
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	label.add_theme_font_override("font", FONT)
	label.add_theme_color_override("font_outline_color", OUTLINE_COLOR)
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT if right_aligned else HORIZONTAL_ALIGNMENT_LEFT
	return label

func _all_labels() -> Array:
	return [_general_heading, _general_body, _performance_heading, _performance_body,
		_specs_heading, _specs_body]

## The FPS counter's own style, at this screen's smaller size: the project font,
## white rows under dimmer headings, everything in units times the GUI scale.
func _apply_scale() -> void:
	_last_scale = UIScale.value
	var scale_factor := UIScale.value
	var font_size := int(round(FONT_UNITS * scale_factor))
	var outline := maxi(1, int(round(OUTLINE_UNITS * scale_factor)))
	var margin := MARGIN_UNITS * scale_factor
	var gap := SECTION_GAP_UNITS * scale_factor
	for label in _all_labels():
		if label == null:
			continue
		label.add_theme_font_size_override("font_size", font_size)
		label.add_theme_constant_override("outline_size", outline)
	if _left_column != null:
		_left_column.offset_left = margin
		_left_column.offset_top = margin
		_left_column.offset_right = margin
		_left_column.offset_bottom = margin
		_left_column.add_theme_constant_override("separation", int(round(gap)))
	if _right_column != null:
		_right_column.offset_left = -margin
		_right_column.offset_right = -margin
		_right_column.offset_top = margin
		_right_column.offset_bottom = margin
		_right_column.add_theme_constant_override("separation", int(round(gap)))

# ---------------------------------------------------------------------------
# Text
# ---------------------------------------------------------------------------

func _create_stats() -> RefCounted:
	# ClassDB rather than a typed reference: the rows degrade to "-" when the
	# loaded DLL predates the binding, instead of the screen failing to compile.
	if not ClassDB.class_exists("SystemStats"):
		return null
	return ClassDB.instantiate("SystemStats") as RefCounted

## The hardware and build facts: read once, because none of them can change while
## the game runs.
func _build_specs_lines() -> void:
	var version := str(ProjectSettings.get_setting("application/config/version", ""))
	if version.is_empty():
		version = "dev"
	var engine_info := Engine.get_version_info()
	var engine_text := "%d.%d.%d" % [
		int(engine_info.get("major", 0)),
		int(engine_info.get("minor", 0)),
		int(engine_info.get("patch", 0)),
	]
	_version_line = "Version: Farlands %s (Godot %s)" % [version, engine_text]

	var cpu_name := OS.get_processor_name()
	if cpu_name.is_empty():
		cpu_name = "unknown"
	_cpu_line = "CPU: %dx %s" % [OS.get_processor_count(), cpu_name]

	var gpu_name := RenderingServer.get_video_adapter_name()
	var gpu_vendor := RenderingServer.get_video_adapter_vendor()
	if gpu_name.is_empty():
		_gpu_line = "GPU: unknown"
	elif gpu_vendor.is_empty() or gpu_name.contains(gpu_vendor):
		# Some drivers already put the vendor in the adapter name; showing it
		# twice reads as two cards.
		_gpu_line = "GPU: %s" % gpu_name
	else:
		_gpu_line = "GPU: %s (%s)" % [gpu_name, gpu_vendor]

	var total_mb := float(OS.get_memory_info().get("physical", 0)) / (1024.0 * 1024.0)
	_ram_line = "RAM: %d MB" % int(round(total_mb)) if total_mb > 0.0 else "RAM: -"

	if _specs_body != null:
		_specs_body.text = "\n".join(
			PackedStringArray([_cpu_line, _gpu_line, _ram_line, _version_line]))

## One refresh of every value the machine feeds: the body's position and speed,
## the OS counters, the memory and VRAM gauges. Label.set_text is a no-op on equal
## text, so a tick that changed nothing costs the composition alone.
func _refresh() -> void:
	if _general_body == null or _performance_body == null:
		return
	var xyz := "XYZ: -"
	var speed := "Speed: -"
	if _player != null and _player.has_method("get_velocity"):
		var at := _player.global_position
		# call(), because the method is the C++ binding's and not on Node3D: a
		# statically typed call is a parse error, and this is the one place the
		# screen reaches into the controller.
		var velocity: Vector3 = _player.call("get_velocity")
		xyz = "XYZ: %.3f / %.3f / %.3f" % [at.x, at.y, at.z]
		speed = "Speed: %.2f blocks/s" % (velocity.length() * TICKS_PER_SECOND)
	_general_body.text = "\n".join(PackedStringArray([_fps_line, xyz, speed]))

	_memory_line = _format_memory()
	_vram_line = _format_vram()
	_performance_body.text = "\n".join(PackedStringArray([
		_frame_line,
		_usage_line("CPU", "get_cpu_usage"),
		_usage_line("GPU", "get_gpu_usage"),
		_vram_line,
		_memory_line,
	]))

## A native percentage, or "-" when the platform has no counter or the binding is
## not in the loaded DLL: an unknown reading must not look like an idle machine.
func _usage_line(prefix: String, method: String) -> String:
	if _stats == null:
		return "%s: -" % prefix
	var percent := float(_stats.call(method))
	if percent < 0.0:
		return "%s: -" % prefix
	return "%s: %d%%" % [prefix, int(round(percent))]

func _format_memory() -> String:
	var used_mb := Performance.get_monitor(Performance.MEMORY_STATIC) / (1024.0 * 1024.0)
	var total_mb := float(OS.get_memory_info().get("physical", 0)) / (1024.0 * 1024.0)
	if total_mb <= 0.0:
		return "Mem: %d MB" % int(round(used_mb))
	return "Mem: %d / %d MB (%d%%)" % [
		int(round(used_mb)),
		int(round(total_mb)),
		int(round(100.0 * used_mb / total_mb)),
	]

func _format_vram() -> String:
	var used_mb := Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / (1024.0 * 1024.0)
	if used_mb <= 0.0:
		# The headless renderer keeps no video memory at all; that is nothing to
		# measure rather than a card with nothing on it.
		return "VRAM: -"
	_vram_peak_mb = maxf(_vram_peak_mb, used_mb)
	return "VRAM: %d MB (peak %d MB)" % [
		int(round(used_mb)),
		int(round(_vram_peak_mb)),
	]
