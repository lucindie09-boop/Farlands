extends RefCounted

## The interface's shared art, sliced so it can be any size.
##
## Godot draws a StyleBoxTexture's nine-slice corners at the texture margin's own
## size, in SCREEN pixels - the margin is not multiplied by the GUI scale. So a
## 4-texel bevel on a button at scale 3 would come out 4 px wide while every
## thing around it is three times that, which is a hairline rather than a bevel.
## The fix is the one `settings_menu._slider_track_style` already used: resample
## the art to the GUI scale with NEAREST (one texel becomes one unit, so the
## pixel art keeps its hard edges) and measure the margins in those texels. The
## caps then hold their size and only the middle stretches - the 9-slice the
## rest of the interface's art has never had.
##
## Deliberately NOT for the square cards (wand.gd's tiles, settings_menu's icon
## buttons): those are square art drawn onto a square box, so they scale evenly
## and there is no stretch to slice.

const BUTTON_TEX: Texture2D = preload("res://assets/textures/gui/button.png")
## The tooltip's box: a gold line outside a darker one around a dark fill.
const TOOLTIP_TEX: Texture2D = preload("res://assets/textures/gui/tooltip.png")
## The dark framed box: the tool screens' panel, so the Skin Maker, the Block
## Maker and the Liquid Texture Lab sit on the same surface as the menus.
const PANEL_TEX: Texture2D = preload("res://assets/textures/gui/effect_background.png")
const SLIDER_TRACK_TEX: Texture2D = preload("res://assets/textures/gui/slider_button.png")
const SLIDER_THUMB_TEX: Texture2D = preload("res://assets/textures/gui/slider.png")

## `button.png`'s rounded cap, in its own 200 x 20 texels: columns 4..195 are
## identical, so the four columns at each end are the only ones that carry the
## corner. The same number is the cap on all four sides - the art is drawn at its
## own 20-texel height, so at native size every patch is 1:1 and the sliced draw
## is pixel-identical to the plain one.
const BUTTON_CAP := 4

## `effect_background.png`'s frame, measured the same way on its 32 x 32 texels
## (columns and rows 4..27 are identical).
const PANEL_CAP := 4

## `slider_button.png`'s rounded end, on its 200 x 20 texels (columns 4..195 are
## identical). The track is what a text field wears, so a name reads as a groove
## in the row rather than as a button that happens to hold text.
const SLIDER_CAP := 4

## `tooltip.png` is 100 x 100 with a two-texel frame (the gold line and the
## darker one inside it) around a flat fill, so two texels each side are the
## caps: the frame keeps its thickness and only the middle stretches.
const TOOLTIP_CAP := 2
## The box is the text plus the padding, with nothing fixed: every name gets the
## box its own letters need, so the tooltip changes size from one slot to the
## next. What is held across names is the PADDING, taken from the reference pair
## (a 66 x 16 text in an 82 x 32 box) - 8 px a side at scale 2, which is the
## 4 units below. "Gravel" is 37 px of Munro at the size below, so its box is
## 53 x 32; a longer name is wider still.
const TOOLTIP_PAD := 4.0
## The tooltip's text, in units: 7 art texels of Munro is 14 px at scale 2, whose
## line is 16 px - the height above - so a one-line box is 32 px tall.
const TOOLTIP_FONT_UNITS := 7.0

# One texture per (art, scale). Settings pages build dozens of buttons each, and
# a 200 x 20 image resized to scale 4 is not free; the art never changes, only
# the scale it is drawn at.
static var _scaled: Dictionary = {}
static var _thumbs: Dictionary = {}

## `tex` resampled to the GUI scale with NEAREST, so one art texel is one unit.
static func scaled(tex: Texture2D, ui_scale: float) -> Texture2D:
	var s := maxf(1.0, roundf(ui_scale))
	var key := "%s@%.0f" % [tex.resource_path, s]
	if _scaled.has(key):
		return _scaled[key]
	# A copy: get_image() hands back the texture's own Image for the whole
	# session, so resizing it in place would make the next scale resize the
	# already-scaled art (scale 4 asked after scale 2 came out 8x).
	var img: Image = tex.get_image().duplicate()
	img.resize(maxi(1, roundi(img.get_width() * s)), maxi(1, roundi(img.get_height() * s)),
			Image.INTERPOLATE_NEAREST)
	var out := ImageTexture.create_from_image(img)
	# The resampled copy is an ImageTexture, so it has no resource_path: carry the
	# art's name so a style or a probe can still say which texture it is.
	out.resource_name = tex.resource_path.get_file()
	_scaled[key] = out
	return out

## `tex` as a stylebox whose `cap_texels` corners keep their art size and whose
## middle stretches: a nine-slice at `ui_scale`. `tint` is the state's colour,
## so one helper serves the normal, hover and pressed boxes.
static func nine_slice(tex: Texture2D, cap_texels: int, ui_scale: float,
		tint := Color.WHITE) -> StyleBoxTexture:
	var s := maxf(1.0, roundf(ui_scale))
	var box := StyleBoxTexture.new()
	box.texture = scaled(tex, s)
	var cap := float(cap_texels) * s
	box.texture_margin_left = cap
	box.texture_margin_right = cap
	box.texture_margin_top = cap
	box.texture_margin_bottom = cap
	# The margins are the ART's, not padding: the label keeps the whole box, so
	# a narrow button's text lands where it did before it was sliced.
	box.set_content_margin_all(0.0)
	box.modulate_color = tint
	return box

## The house button, sliced: what every button in the interface wears.
static func button(ui_scale: float, tint := Color.WHITE) -> StyleBoxTexture:
	return nine_slice(BUTTON_TEX, BUTTON_CAP, ui_scale, tint)

## The dark panel the tool screens sit on, with `content_units` of padding.
static func panel(ui_scale: float, tint := Color.WHITE,
		content_units := 8.0) -> StyleBoxTexture:
	var box := nine_slice(PANEL_TEX, PANEL_CAP, ui_scale, tint)
	box.set_content_margin_all(content_units * maxf(1.0, roundf(ui_scale)))
	return box

## The slider's track, pre-scaled to the GUI scale like the button art: the
## track is drawn at the stylebox's minimum height (the sum of the vertical
## margins), so with the art at its own 20 texels the margins are half its
## scaled height and the track comes out exactly 20 * scale tall. The 1-texel
## end caps stay 1 px, which is what keeps the track's rounded ends from
## sampling past the art (a margin larger than the source draws a black seam).
static func slider_track(ui_scale: float) -> StyleBoxTexture:
	var tex := scaled(SLIDER_TRACK_TEX, ui_scale)
	var style := StyleBoxTexture.new()
	style.texture = tex
	style.texture_margin_left = 1.0
	style.texture_margin_right = 1.0
	var half := tex.get_height() / 2.0
	style.texture_margin_top = half
	style.texture_margin_bottom = half
	return style

## The grabber, at the GUI scale. A themed ICON cannot be modulated the way a
## stylebox can, so the highlight and disabled handles are the same pixels
## multiplied up or down.
static func slider_thumb(ui_scale: float, tint := 1.0) -> Texture2D:
	var s := maxf(1.0, roundf(ui_scale))
	var key := "%.0f@%.2f" % [s, tint]
	if _thumbs.has(key):
		return _thumbs[key]
	var img: Image = SLIDER_THUMB_TEX.get_image().duplicate()
	img.resize(maxi(1, roundi(img.get_width() * s)), maxi(1, roundi(img.get_height() * s)),
			Image.INTERPOLATE_NEAREST)
	if not is_equal_approx(tint, 1.0):
		img.convert(Image.FORMAT_RGBA8)
		for y in range(img.get_height()):
			for x in range(img.get_width()):
				var c := img.get_pixel(x, y)
				img.set_pixel(x, y, Color(minf(c.r * tint, 1.0), minf(c.g * tint, 1.0),
						minf(c.b * tint, 1.0), c.a))
	var out := ImageTexture.create_from_image(img)
	out.resource_name = SLIDER_THUMB_TEX.resource_path.get_file()
	_thumbs[key] = out
	return out

## The house slider: the track texture, its own square thumb, and none of the
## engine's second fill behind the handle. `center_grabber` stays off so the
## thumb travels the track's width minus its own, instead of hanging half of it
## off each end.
static func style_slider(slider: HSlider, ui_scale: float) -> void:
	slider.add_theme_stylebox_override("slider", slider_track(ui_scale))
	slider.add_theme_stylebox_override("grabber_area", StyleBoxEmpty.new())
	slider.add_theme_stylebox_override("grabber_area_highlight", StyleBoxEmpty.new())
	slider.add_theme_icon_override("grabber", slider_thumb(ui_scale))
	slider.add_theme_icon_override("grabber_highlight", slider_thumb(ui_scale, 1.2))
	slider.add_theme_icon_override("grabber_disabled", slider_thumb(ui_scale, 0.7))
	slider.add_theme_constant_override("grabber_offset", 0)
	slider.add_theme_constant_override("center_grabber", 0)

## A button in the house art: normal, the brighter hover, the pressed dim, a
## darker disabled, and no focus box (the interface marks focus with its own
## furniture, and a filled focus box would hide the press).
## `padding_units` puts that much room either side of the label inside the
## bevel; without it the text starts on the art's own edge, which is fine where
## the caller sets a width and cramped where the label alone sizes the button.
static func style_button(btn: Button, ui_scale: float,
		font_color := Color.WHITE, padding_units := 0.0) -> void:
	var s := maxf(1.0, roundf(ui_scale))
	var states := {
		"normal": button(ui_scale),
		"hover": button(ui_scale, Color(1.2, 1.2, 1.2)),
		"pressed": button(ui_scale, Color(0.75, 0.75, 0.75)),
		"disabled": button(ui_scale, Color(0.55, 0.55, 0.55)),
	}
	for state in states:
		var box: StyleBoxTexture = states[state]
		if padding_units > 0.0:
			box.content_margin_left = padding_units * s
			box.content_margin_right = padding_units * s
		btn.add_theme_stylebox_override(state, box)
	btn.add_theme_stylebox_override("focus", StyleBoxEmpty.new())
	for state in ["font_color", "font_hover_color", "font_pressed_color",
			"font_focus_color"]:
		btn.add_theme_color_override(state, font_color)

## The tooltip box, sliced: the gold frame keeps its two texels and the label
## gets `TOOLTIP_PAD * scale` of padding inside it.
static func tooltip_panel(ui_scale: float) -> StyleBoxTexture:
	var s := maxf(1.0, roundf(ui_scale))
	var box := nine_slice(TOOLTIP_TEX, TOOLTIP_CAP, s)
	box.set_content_margin_all(TOOLTIP_PAD * s)
	return box

## A text field that reads as a row: the house art behind it, with the padding a
## typed name needs. `track` puts the slider's track behind it instead of the
## button bevel - a name is a value you fill in, not a thing you press.
static func style_line_edit(edit: LineEdit, ui_scale: float,
		font_color := Color.WHITE, track := false) -> void:
	var s := maxf(1.0, roundf(ui_scale))
	var art := SLIDER_TRACK_TEX if track else BUTTON_TEX
	var cap := SLIDER_CAP if track else BUTTON_CAP
	var normal := nine_slice(art, cap, s)
	normal.set_content_margin_all(4.0 * s)
	normal.content_margin_left = 6.0 * s
	normal.content_margin_right = 6.0 * s
	var focus := nine_slice(art, cap, s, Color(1.15, 1.15, 1.15))
	focus.set_content_margin_all(4.0 * s)
	focus.content_margin_left = 6.0 * s
	focus.content_margin_right = 6.0 * s
	edit.add_theme_stylebox_override("normal", normal)
	edit.add_theme_stylebox_override("focus", focus)
	edit.add_theme_color_override("font_color", font_color)
	edit.add_theme_color_override("caret_color", font_color)
