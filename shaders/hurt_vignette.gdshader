shader_type canvas_item;

// The screen's edge when something has taken health off the player, and the
// heartbeat that stays there while the health is low. Both are the same number:
// how much of the tint to draw. The SHAPE is an edge vignette that reaches the
// middle not at all, because a flat screen wash reads as a filter over the game,
// while a red edge reads as the body the game is attached to.
//
// Driven by scripts/hurt_overlay.gd, which owns the timing: `flash` is one hit's
// decay and `pulse` is the low-health beat. Nothing here knows what a hit or a
// heart is.

uniform vec4 tint : source_color = vec4(0.62, 0.04, 0.03, 1.0);
// 0..1: one hit, fading out.
uniform float flash : hint_range(0.0, 1.0) = 0.0;
// 0..1: the low-health beat.
uniform float pulse : hint_range(0.0, 1.0) = 0.0;
// How fast the edge fills in from the border. Higher = a narrower rim.
uniform float edge_power : hint_range(1.0, 6.0) = 2.4;
// How much of a hit also tints the middle, so a heavy blow is felt in the centre
// of the view too. Not 0: a rim alone can be missed while looking at the floor.
uniform float wash : hint_range(0.0, 1.0) = 0.22;

void fragment() {
    // 0 in the middle, 1 at the edge midpoints, so the rim is the same on any
    // aspect ratio (a screen-space distance in pixels would be oval on a wide
    // window).
    vec2 p = UV * 2.0 - 1.0;
    float edge = pow(clamp(length(p), 0.0, 1.0), edge_power);
    float amount = flash * mix(wash, 1.0, edge) + pulse * edge;
    COLOR = vec4(tint.rgb, clamp(amount, 0.0, 1.0) * tint.a);
}
