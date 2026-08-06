# Shared UI drawing utilities for hotbar and inventory
# Pure static functions with no data loading to avoid performance overhead

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")

# Static function for drawing item count - deduped between hotbar and inventory
static func draw_item_count(canvas: CanvasItem, count_text: String, right_x: float, bottom_y: float, slot_size: float) -> void:
	var font_size = int(round(slot_size * 0.5))
	var margin = max(1.0, slot_size / 18.0)
	var text_width = MUNRO_FONT.get_string_size(count_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
	var descent = MUNRO_FONT.get_descent(font_size)
	var pos = Vector2(right_x - margin - text_width, bottom_y - margin - descent)
	var shadow = Vector2(margin * 0.5, margin * 0.5)
	canvas.draw_string(MUNRO_FONT, pos + shadow, count_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.09, 0.09, 0.09))
	canvas.draw_string(MUNRO_FONT, pos, count_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color.WHITE)
