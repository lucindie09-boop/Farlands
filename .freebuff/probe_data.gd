extends Node
# Correlate far-region NOCACHE members (logged by the instrumented DLL) with
# real block data: sample every chunk in the area the far-band screenshot
# covered, and print which chunks contain terrain (non-air). NOCACHE + terrain
# = a real LOD hole.

var main: Node
var cm: Node
var player: Node3D

func _ready() -> void:
    main = load("res://Main.tscn").instantiate()
    add_child(main)
    await get_tree().process_frame
    await get_tree().process_frame
    cm = main.get_node_or_null("ChunkManager")
    player = main.get_node("Player")
    print("PROBE init done")
    get_tree().create_timer(150.0).timeout.connect(func():
        print("PROBE WATCHDOG QUIT")
        get_tree().quit())

    # Far band position matching the far_look shot: player ~19 chunks from the
    # signature chunk (-80,-528) so the NOCACHE set matches that screenshot.
    print("PROBE teleporting")
    player.call("teleport_to", Vector3(-80.0, 360.0, 92.0))
    print("PROBE teleported")
    await wait(1500)
    print("PROBE settled; scanning")

    # Scan the area the far shot covered: x -544..-256, z -800..-640,
    # cy 7..11 (y 224..384). Sample every 8 blocks.
    for cy in range(7, 12):
        var y0 := cy * 32
        for cx in range(-17, -7):
            for cz in range(-25, -19):
                var non_air := 0
                for x in range(cx * 32 + 8, cx * 32 + 32, 8):
                    for z in range(cz * 32 + 8, cz * 32 + 32, 8):
                        for y in range(y0, y0 + 32, 8):
                            if cm.get_block(x, y, z) != 0:
                                non_air += 1
                if non_air > 0:
                    print("TERRAIN chunk=(%d,%d,%d) non_air=%d" % [cx, cy, cz, non_air])
    print("PROBE scan done")
    print("PROBE DONE")
    get_tree().quit()

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame