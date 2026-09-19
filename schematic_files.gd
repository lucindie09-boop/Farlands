extends RefCounted

## Where build files live, in ONE place.
##
## `/paste <name>` and the wand's menu have to agree about all of it: which
## folders are searched, in what order, which extensions a bare name is tried
## with, and which files are listed as available. Two copies of that would drift
## the first time a folder is added, and the symptom would be a file the menu
## shows but the command cannot open (or the reverse).
##
## Static, so a caller uses it without instancing anything:
##     const Files := preload("res://schematic_files.gd")
##     Files.resolve("church")           -> "res://schematics/church.schematic"
##     Files.list()                      -> ["church.schematic", ...]

# The folders, most specific first. `res://schematics/` is the project's own
# folder (what ships with the game), `user://schematics/` is the player's, and
# the project root and user root are searched last so a file dropped next to
# project.godot still resolves.
const DIRS := ["res://schematics/", "user://schematics/"]
const FALLBACK_DIRS := ["res://", "user://"]
const EXTENSIONS := ["schematic", "schem", "nbt"]

## The path a name opens to, or "" when nothing matches. A path that already
## exists is used as given, so an absolute res://user:// path works too.
static func resolve(name: String) -> String:
	if name.is_empty():
		return ""
	if FileAccess.file_exists(name):
		return name
	var candidates: Array[String] = [name]
	if not name.get_extension().to_lower() in EXTENSIONS:
		for extension in EXTENSIONS:
			candidates.append(name + "." + extension)
	for candidate in candidates:
		for prefix in DIRS + FALLBACK_DIRS:
			if FileAccess.file_exists(prefix + candidate):
				return prefix + candidate
	return ""

## Every build file the folders hold, as bare file names, sorted.
static func list() -> Array[String]:
	var found: Array[String] = []
	for prefix in DIRS + FALLBACK_DIRS:
		var dir := DirAccess.open(prefix)
		if dir == null:
			continue
		dir.list_dir_begin()
		var entry := dir.get_next()
		while entry != "":
			if not dir.current_is_dir() and entry.get_extension().to_lower() in EXTENSIONS:
				if not found.has(entry):
					found.append(entry)
			entry = dir.get_next()
		dir.list_dir_end()
	found.sort()
	return found

## The bytes of a build file, empty when it cannot be read.
static func read(name: String) -> PackedByteArray:
	var path := resolve(name)
	if path.is_empty():
		return PackedByteArray()
	return FileAccess.get_file_as_bytes(path)

## A label for the menu: the file name without its extension, which is the part
## that tells one build from another.
static func display_name(file_name: String) -> String:
	var base := file_name.get_file()
	var dot := base.rfind(".")
	return base if dot <= 0 else base.substr(0, dot)
