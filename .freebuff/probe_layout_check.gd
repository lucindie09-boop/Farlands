extends SceneTree

# The startup self-check, asked directly: does the engine agree with the packing our
# preview code writes?
#
# Expected windowed: has_binding=true, ok=true.
# Expected headless: ok=true as well, but for a different reason — the dummy renderer
# keeps no instance data, so there is nothing to compare and the check must stay quiet
# rather than report a failure that is an artefact of the harness.

func _initialize() -> void:
	var cm = ClassDB.instantiate("ChunkManager")
	if cm == null:
		print("LAYOUT_PROBE could_not_instantiate_ChunkManager")
		quit(1)
		return
	print("LAYOUT_PROBE has_binding=", cm.has_method("debug_multimesh_layout_ok"),
		" headless=", DisplayServer.get_name() == "headless")
	print("LAYOUT_PROBE ok=", cm.debug_multimesh_layout_ok())
	quit(0)
