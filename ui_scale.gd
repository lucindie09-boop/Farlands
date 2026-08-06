extends Node

# Global UI scale shared by hotbar, inventory, chat, and the settings menu.
# Hotbar/inventory read it directly (default 3.0); settings and chat derive
# their own factors so their default look is preserved:
#   settings: UIScale.value * 2/3   (default -> 2.0)
#   chat:     UIScale.value / 3.0   (default -> 1.0)
var value: float = 3.0
