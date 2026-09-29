extends Node
# Runs AFTER the player controller's _process (sibling added later), forcing
# the camera's pitch so the probe controls the view exactly.

var target: Node = null

func _process(_delta: float) -> void:
    if target == null:
        return
    var cam: Camera3D = target.cam
    if cam != null:
        var p: Vector3 = cam.rotation
        cam.rotation = Vector3(target.want_pitch, p.y, p.z)
