#!/usr/bin/env bash
# Run a Godot probe against the dev's real world WITHOUT changing it.
#
# A probe that places blocks is writing persisted edits (user://chunks/*.edit), so
# an interrupted or careless run leaves litter in the world the user plays. This
# snapshots that directory, runs the probe, and puts the snapshot back afterwards,
# no matter how the probe ended.
#
#   .freebuff/run_probe.sh .freebuff/probe_flow.gd [timeout_seconds]
#
# The world must not be open in the editor/game while this runs: a running game
# holds its own copy and would overwrite the restore.
set -uo pipefail

PROBE="${1:?usage: run_probe.sh <res:// path of a .gd probe> [timeout]}"
TIMEOUT="${2:-300}"
GODOT="${GODOT:-/c/Users/lucin/Documents/Developement/Godot/Godot_v4.7.1-stable_win64_console.exe}"
WORLD="/c/Users/lucin/AppData/Roaming/Godot/app_userdata/Farlands/chunks"
BACKUP="$(mktemp -d)"

if [ ! -d "$WORLD" ]; then
	echo "run_probe: no saved world at $WORLD" >&2
	exit 2
fi

# A running GAME holds the world in memory and would save over the restore. The
# editor does not (it only holds the project), so it is not a problem.
games=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name like '%Godot%'\" | Where-Object { \$_.CommandLine -notmatch '--editor(\s|$)' } | Measure-Object).Count" 2>/dev/null | tr -d '\r')
if [ "${games:-0}" != "0" ]; then
	echo "run_probe: a Godot game process is running; close it so the restore sticks" >&2
	exit 2
fi

echo "run_probe: snapshotting $(du -sh "$WORLD" | cut -f1) of world -> $BACKUP"
cp -r "$WORLD" "$BACKUP/chunks"

restore() {
	rm -rf "$WORLD"
	cp -r "$BACKUP/chunks" "$WORLD"
	echo "run_probe: world restored"
	rm -rf "$BACKUP"
}
trap restore EXIT

timeout "$TIMEOUT" "$GODOT" --headless --path "$(dirname "$(dirname "$(readlink -f "$0")")")" \
	--script "res://$PROBE" 2>&1 | grep -E "PROBE|probe:|SCRIPT ERROR|Parse Error|ERROR: Failed"
status=${PIPESTATUS[0]}
echo "run_probe: probe exited $status"
exit "$status"
