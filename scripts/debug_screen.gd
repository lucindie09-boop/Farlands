extends Control

# F3 debug screen, in three sections -- Minecraft's own screen, split by what the
# numbers are about:
#
#   GENERAL      how the game is running: frame rate, body position, speed, the
#                biome under it and the render distance it is streaming.
#   PERFORMANCE  what this process is spending on that frame: frame time, the
#                engine's own CPU cost of it and the rest of the frame, CPU and
#                GPU utilization, VRAM in use against the card's total, and the
#                engine's share of memory.
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
# the DRM busy file on Linux. The card's total video memory comes from the same
# object for the same reason: Godot reports what the engine has ALLOCATED, never
# the capacity to read it against. An older DLL without the binding leaves those
# rows at "-" rather than breaking the screen.
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

# Chunk width in blocks (AGENTS.md: chunk size is 32x32x32), so the render
# distance can also be read as the span of world it streams.
const CHUNK_WIDTH := 32

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
# Published with the frame rate, over the same window (see _close_window).
var _cpu_time_line := "CPU time: -"
var _gpu_time_line := "GPU time: -"
# The CPU half accumulates inside the window: the controller's process clock is
# read every frame and diffed, so the cost is of the frames that actually ran.
var _cpu_total_us := 0
var _cpu_count := 0
var _cpu_clock_seeded := false
var _cpu_ms_total := 0.0
var _cpu_samples := 0
# The GPU half: the engine's measured render time when the renderer reports one,
# accumulated over the same window. Zero reads mean no measurement (headless, or a
# driver without timestamps) and the row falls back to the frame remainder.
var _viewport_rid := RID()
var _gpu_measured_ms_total := 0.0
var _gpu_measured_samples := 0
# The biome row, resampled only when the body crosses into another column.
var _biome_line := "Biome: -"
var _biome_x := 2147483647
var _biome_z := 2147483647

var _player: Node3D = null
# The world's streaming radius, read from the node rather than a copy of it: the
# settings menu can change it mid-session.
var _chunk_manager: Node = null
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
		_chunk_manager = main.get_node_or_null("ChunkManager")
	# The GPU row's measurement has to be switched on for this viewport; the engine
	# answers 0 for a viewport nobody asked to measure, which is most of why it read
	# 0 here for so long (the other reason was --headless: a dummy driver has no GPU
	# timestamps to read).
	_viewport_rid = get_viewport().get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(_viewport_rid, true)
	_build_sections()
	# Before the SPECS lines: the RAM row carries the modules' own description,
	# which only the native object can read.
	_stats = _create_stats()
	_build_specs_lines()
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
	_sample_frame_costs()
	if _window >= FPS_WINDOW:
		_close_window()
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
	# The CPU clock's baseline belongs to the screen: opening it again must not
	# count the hidden frames' work.
	_cpu_clock_seeded = false
	_cpu_ms_total = 0.0
	_cpu_samples = 0
	_gpu_measured_ms_total = 0.0
	_gpu_measured_samples = 0
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

	# Capacity in GB (the OS reports bytes, and "32 GB" is how RAM is sold), then
	# the modules' own description -- "DDR4-3200 (2x 16 GB)" -- from the firmware.
	var total_mb := float(OS.get_memory_info().get("physical", 0)) / (1024.0 * 1024.0)
	if total_mb <= 0.0:
		_ram_line = "RAM: -"
	else:
		_ram_line = "RAM: %d GB" % int(round(total_mb / 1024.0))
		if _stats != null:
			var spec := str(_stats.call("get_memory_spec"))
			if not spec.is_empty():
				_ram_line = "%s %s" % [_ram_line, spec]

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
		_update_biome(at)
	_general_body.text = "\n".join(
		PackedStringArray([_fps_line, xyz, speed, _biome_line, _render_line()]))
	_memory_line = _format_memory()
	_vram_line = _format_vram()
	_performance_body.text = "\n".join(PackedStringArray([
		_frame_line,
		_cpu_time_line,
		_gpu_time_line,
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

## One frame's CPU cost, for the one-second window: the controller's process
## clock is cumulative, so the frames since the last read are the difference over
## the frames that ran. A clock that went backwards has been reset (the perf
## report does that), and that read only seeds the new baseline.
func _sample_frame_costs() -> void:
	# GPU first: the measured value is the last completed frame's, and a renderer
	# without timestamp queries answers 0, which is not a frame that cost nothing.
	if _viewport_rid.is_valid():
		var gpu_ms := RenderingServer.viewport_get_measured_render_time_gpu(_viewport_rid)
		if gpu_ms > 0.0:
			_gpu_measured_ms_total += gpu_ms
			_gpu_measured_samples += 1
	if _chunk_manager == null or not _chunk_manager.has_method("get_process_clock"):
		return
	var clock: Dictionary = _chunk_manager.call("get_process_clock")
	var total_us := int(clock.get("total_us", 0))
	var count := int(clock.get("count", 0))
	var frames := count - _cpu_count
	var elapsed_us := total_us - _cpu_total_us
	_cpu_total_us = total_us
	_cpu_count = count
	var seeded := _cpu_clock_seeded
	_cpu_clock_seeded = true
	if not seeded or frames <= 0 or elapsed_us < 0:
		return
	_cpu_ms_total += (float(elapsed_us) / 1000.0) / float(frames)
	_cpu_samples += 1

## Frame rate, frame time, CPU time and the rest of the frame are one picture and
## are published together: a rate averaged over a second beside a cost from the
## last 50 ms would disagree with itself. The CPU side is the engine's own measured
## C++ work for the frame (the perf report's "process_total"); everything else in
## the frame is what the GPU and the script layer took, which is the row beside it.
func _close_window() -> void:
	var frames := maxi(_frames, 1)
	var frame_ms := _frame_ms_total / float(frames)
	_fps = int(round(float(_frames) / maxf(_window, 0.0001)))
	_fps_line = "FPS: %d" % _fps
	_frame_line = "Frame: %.2f ms" % frame_ms
	if _cpu_samples > 0:
		var cpu_ms := _cpu_ms_total / float(_cpu_samples)
		_cpu_time_line = "CPU time: %.3f ms" % cpu_ms
		if _gpu_measured_samples > 0:
			# The engine timed the GPU. Preferred: it is the card's own work, so a
			# vsynced frame does not report its idle as GPU time.
			_gpu_time_line = "GPU time: %.3f ms" % (
				_gpu_measured_ms_total / float(_gpu_measured_samples))
		else:
			# Nothing measured it, so the row is the frame less our C++ work -- the
			# GPU plus the script layer, and v-sync's idle wait along with them. The
			# suffix says so rather than letting the two look alike.
			_gpu_time_line = "GPU time: %.3f ms (est)" % maxf(frame_ms - cpu_ms, 0.0)
	_frames = 0
	_window = 0.0
	_frame_ms_total = 0.0
	_cpu_ms_total = 0.0
	_cpu_samples = 0
	_gpu_measured_ms_total = 0.0
	_gpu_measured_samples = 0

## The biome under the body, from the same estimator generation uses. Resampled
## only when the position crosses into another column: one sample is a whole
## column's climate and lattice work, far too heavy for a 20 Hz refresh.
func _update_biome(at: Vector3) -> void:
	if _chunk_manager == null or not _chunk_manager.has_method("get_biome_at"):
		_biome_line = "Biome: -"
		return
	var x := int(floor(at.x))
	var z := int(floor(at.z))
	if x == _biome_x and z == _biome_z:
		return
	_biome_x = x
	_biome_z = z
	_biome_line = "Biome: %s" % str(_chunk_manager.call("get_biome_at", x, z))

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

## The streaming radius in the units it is set in (`ChunkManager.render_distance`,
## chunks) and in the blocks it covers, which is the scale the coordinates beside
## it are in.
func _render_line() -> String:
	if _chunk_manager == null:
		return "RD: -"
	var chunks := int(_chunk_manager.get("render_distance"))
	if chunks <= 0:
		return "RD: -"
	return "RD: %d chunks (%d blocks)" % [chunks, chunks * CHUNK_WIDTH]

## What the engine has on the card against what the card has, in MB. The total
## comes from the native sampler because Godot has no capacity monitor; a build
## without it (or a platform that cannot say) shows the used figure alone.
func _format_vram() -> String:
	var used_mb := Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / (1024.0 * 1024.0)
	if used_mb <= 0.0:
		# The headless renderer keeps no video memory at all; that is nothing to
		# measure rather than a card with nothing on it.
		return "VRAM: -"
	var total_mb := -1.0
	if _stats != null:
		total_mb = float(_stats.call("get_vram_total"))
	if total_mb <= 0.0:
		return "VRAM: %d MB" % int(round(used_mb))
	return "VRAM: %d / %d MB" % [int(round(used_mb)), int(round(total_mb))]
