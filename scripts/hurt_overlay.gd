extends ColorRect

## The screen's own reaction to being hurt (shaders/hurt_vignette.gdshader).
##
## The player already leans when a hit lands -- engine-side, on the camera
## (engine/player_damage.hpp's hurt_lean_view) -- so this is the other half of
## the same moment: a red edge that flashes with the hit and fades, and a
## heartbeat under it that stays while the health is low. Both are read from state
## the game already keeps: the `damaged` signal for the hit and the `health`
## property for the beat, so nothing here invents a second notion of hurt and the
## two cannot disagree with the health bar.
##
## It draws over the world and under the death screen (Main.tscn puts it there),
## and takes no input: it is a filter over the frame, not a control.

## How much health counts as low, in half-hearts. Two hearts, which is where the
## health bar's own warning begins to read as an emergency.
const LOW_HEALTH := 4
## Seconds a hit's flash takes to fade. Short enough to be a flinch, long enough
## not to strobe when something hits repeatedly.
const FLASH_SECONDS := 0.32
## The smallest flash, so even a half-heart hit is seen.
const FLASH_FLOOR := 0.35
## A hit of this many half-hearts brings the flash to full. Bigger hits are not
## brighter than full.
const FLASH_FULL_AT := 6.0
## Seconds per heartbeat while the health is low.
const BEAT_PERIOD := 0.75

var _player: Node = null
var _flash := 0.0
var _pulse := 0.0
var _beat_phase := 0.0
var _push_flash := -1.0
var _push_pulse := -1.0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	if material == null:
		var shader_material := ShaderMaterial.new()
		shader_material.shader = load("res://shaders/hurt_vignette.gdshader")
		material = shader_material

	_player = get_node_or_null("/root/Main/Player")
	if _player != null and _player.has_signal("damaged"):
		_player.damaged.connect(_on_damaged)
	_push()


func _process(delta: float) -> void:
	_flash = maxf(0.0, _flash - delta / FLASH_SECONDS)

	# The beat only runs while there is health left to lose: at zero the death
	# screen takes the frame, and a heart that keeps beating under it is a lie.
	var beat := 0.0
	var health := _health()
	if health > 0 and health <= LOW_HEALTH:
		_beat_phase = fmod(_beat_phase + delta / BEAT_PERIOD, 1.0)
		# Closer to death beats harder: the beat carries the urgency, not just
		# the fact of being low.
		var urgency := 1.0 - float(health) / float(LOW_HEALTH)
		beat = _beat(_beat_phase) * (0.38 + 0.30 * urgency)
	# Falls away with the last beat instead of cutting off the instant health
	# crosses the line.
	_pulse = beat if beat >= _pulse else maxf(beat, _pulse - delta * 2.0)
	_push()


## One hit, as its size after the reduction chain: a scratch flashes less than a
## creeper, and nothing flashes longer than FLASH_SECONDS.
func _on_damaged(amount: int) -> void:
	var strength := clampf(float(amount) / FLASH_FULL_AT, 0.0, 1.0)
	_flash = maxf(_flash, maxf(FLASH_FLOOR, strength))


## Two thumps per period, the second softer: a heart, not a metronome. `phase` is
## 0..1 across the period.
func _beat(phase: float) -> float:
	return maxf(_thump(phase, 0.0, 0.10), _thump(phase, 0.22, 0.09) * 0.55)


## A thump that starts at `at` of the period and decays over `width`: sharp on
## the leading edge, gone before the next one, which is what a beat is.
func _thump(phase: float, at: float, width: float) -> float:
	var t := phase - at
	if t < 0.0:
		t += 1.0
	var x := clampf(1.0 - t / maxf(width, 0.001), 0.0, 1.0)
	return x * x


func _health() -> int:
	if _player == null:
		return 0
	return int(_player.get("health"))


## Pushes the two numbers into the shader, but only when they moved: a per-frame
## uniform write is a per-frame material update for a value that is zero most of
## the time.
func _push() -> void:
	if _flash != _push_flash:
		_push_flash = _flash
		(material as ShaderMaterial).set_shader_parameter("flash", _flash)
	if _pulse != _push_pulse:
		_push_pulse = _pulse
		(material as ShaderMaterial).set_shader_parameter("pulse", _pulse)
