extends Node

# Simple test script to render block icons
# Attach this to a node in the scene and run the game

func _ready() -> void:
	# Wait a moment for the BlockIconRenderer to initialize
	await get_tree().process_frame
	await get_tree().process_frame
	
	var renderer = get_node_or_null("/root/BlockIconRenderer")
	if renderer == null:
		print("BlockIconRenderer not found!")
		return
	
	print("Starting icon test...")
	renderer.test_render_icons()
	
	print("Icon test complete. Check user:// for saved PNG files.")
