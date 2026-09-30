extends Node

# Global GUI scale shared by every HUD surface. One unit is one logical pixel:
# at scale k a widget `n` units across is `n * k` screen pixels, so every
# surface stays on the same grid and the whole HUD grows together.
#
# The scale is an integer by construction -- a fractional scale resamples every
# texture and font -- so `set_scale` rounds and clamps rather than trusting the
# caller (the config file is the one writer that can hold a 2.5).
var value: float = 2.0

# Use this rather than assigning `value`: it is the only place the rounding and
# the clamp live, so the invariant cannot drift.
func set_scale(next: float) -> void:
	value = clampf(roundf(next), 1.0, 8.0)

## The pixel offset that centers a `panel_units`-wide panel in a `viewport_px`
## -wide viewport, snapped to whole units. Layout must go through this rather
## than `(viewport - panel_px) / 2.0`: the naive form lands the panel (and every
## slot inside it) on a half pixel whenever the viewport is an odd number of
## pixels, which shows up as a one-pixel seam and resampled art.
func centered_origin(viewport_px: float, panel_units: float) -> float:
	return floorf((floorf(viewport_px / value) - panel_units) * 0.5) * value

## The same snap for a panel anchored to the far edge: `inset_units` in from it.
## The hotbar uses inset 0 (its bottom edge sits on the screen's).
func edge_origin(viewport_px: float, panel_units: float, inset_units: float) -> float:
	return (floorf(viewport_px / value) - panel_units - inset_units) * value
