extends Node
# Far-region self-heal verification: teleport to the area that previously had
# persistent NOCACHE holes (chunk cluster around block -544,-704), settle long
# enough for streaming + far-region rebuilds + the self-heal re-queue cycles,
# then print the perf report. The hole signature is:
#   - "Partial cache" > 0  -> regions rebuilt with missing member caches
#   - "Far cached" << "Far eligible" with terrain present

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
    print("PROBEV ready")

    get_tree().create_timer(170.0).timeout.connect(func():
        print("PROBEV WATCHDOG QUIT")
        var r: String = cm.get_performance_report()
        var lines: PackedStringArray = r.split("\n")
        for l in lines:
            if l.contains("Far eligible") or l.contains("Far cached") or l.contains("Partial cache") or l.contains("Region members"):
                print("PROBEV ", l.strip_edges())
        get_tree().quit())

    # Block coords of the previously-hole-ridden cluster (chunk -17,-22 area).
    var cx := -544.0
    var cz := -704.0
    player.call("teleport_to", Vector3(cx, 460.0, cz))
    await wait(1200)
    var surf := surface_at(cx, cz)
    print("PROBEV surface at (", cx, ",", cz, ") = ", surf)

    # Stay put in the mid/far band so regions rebuild + self-heal fully.
    player.call("teleport_to", Vector3(cx, surf + 80.0, cz + 384.0))
    await wait(2400)
    print("PROBEV settled")

    var r: String = cm.get_performance_report()
    var lines: PackedStringArray = r.split("\n")
    for l in lines:
        if l.contains("Far eligible") or l.contains("Far cached") or l.contains("Partial cache") or l.contains("Region members"):
            print("PROBEV ", l.strip_edges())
    print("PROBEV DONE")
    get_tree().quit()

func surface_at(wx: float, wz: float) -> float:
    var x := int(wx)
    var z := int(wz)
    if cm == null:
        return -999.0
    var found := -999.0
    var y := 1000
    while y > 0:
        if cm.get_block(x, y, z) != 0:
            found = float(y)
            break
        y -= 8
    return found

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame