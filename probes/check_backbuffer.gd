extends SceneTree
## Does this engine build have the node and the two modes shader_overlay.gd now
## names? A three-line question with a three-line answer, headless, no window and
## no scene - the point is only that the names are real.
##
##   "$GODOT" --headless --path . --script res://probes/check_backbuffer.gd

func _initialize() -> void:
	var copy := BackBufferCopy.new()
	copy.copy_mode = BackBufferCopy.COPY_MODE_VIEWPORT
	var viewport_mode := copy.copy_mode
	copy.copy_mode = BackBufferCopy.COPY_MODE_DISABLED
	var disabled_mode := copy.copy_mode
	copy.visible = false
	# A hidden copy must stay hidden: the overlay relies on a node that is not
	# processed at all when its effect is off.
	var hidden_ok := copy.visible == false
	copy.free()
	print("backbuffer: viewport=%d disabled=%d hidden_stays_hidden=%s"
		% [viewport_mode, disabled_mode, hidden_ok])
	quit(0)
