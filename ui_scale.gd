extends Node

# Global UI scale shared by hotbar, inventory, chat, and the settings menu.
# Everything reads it directly: the settings menu draws one unit per UI-scale
# pixel, and hotbar/inventory/chat scale their textures by the same factor.
var value: float = 2.0
