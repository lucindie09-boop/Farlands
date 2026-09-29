extends SceneTree

# Proves the crash handler is live in this build: first that the module reported a
# directory for its reports, then that a deliberate fault actually writes one.
#
# The deliberate fault only arms when the environment says so, so this probe needs
# FARLANDS_CRASH_TEST=1 (run_crash_probe.sh sets it). Without it the second half is
# skipped and the probe only reports the install line.

func _initialize() -> void:
	var dir := OS.get_user_data_dir() + "/crashes"
	var before: Array = []
	var d := DirAccess.open(dir)
	if d != null:
		for f in d.get_files():
			before.append(f)
	print("CRASH_PROBE dir=", dir, " files_before=", before.size())

	# No scene is loaded under -s, so the class is instantiated directly. The binding
	# is what matters here, not a live world.
	var cm = ClassDB.instantiate("ChunkManager")
	if cm == null:
		print("CRASH_PROBE could_not_instantiate_ChunkManager")
	else:
		print("CRASH_PROBE has_binding=", cm.has_method("debug_crash_for_test"))

	var armed := OS.get_environment("FARLANDS_CRASH_TEST") == "1"
	print("CRASH_PROBE armed=", armed)
	if armed:
		print("CRASH_PROBE faulting now")
		cm.debug_crash_for_test()
		print("CRASH_PROBE STILL ALIVE AFTER FAULT (the handler let us continue)")
	else:
		print("CRASH_PROBE not armed; skipping the fault")
	quit(0)
