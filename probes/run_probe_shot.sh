#!/usr/bin/env bash
# Windowed variant of run_probe.sh, for probes that need a REAL viewport (a
# screenshot from the dummy renderer is blank). Copies the shots the run wrote
# into .freebuff/shots and touches nothing else.
#
# It used to snapshot user:// before the run and restore it after - which ended
# in `rm -rf "$USERDATA"` on the LIVE directory, with no check for a running
# game. A probe run while the player was in the game therefore deleted and
# recreated their whole data directory underneath it (logs, settings, saved
# liquids, world edits), which is almost certainly what a week of "startup
# crashes, sometimes" was: the crash times line up with the probe window, and
# they stopped when this stopped. Nothing here writes to user:// now, and a
# probe that finds a game running refuses to start at all.
#
#   .freebuff/run_probe_shot.sh .freebuff/probe_menu_shot.gd [timeout]
set -uo pipefail

PROBE="${1:?usage: run_probe_shot.sh <res:// path of a .gd probe> [timeout]}"
TIMEOUT="${2:-300}"
GODOT="${GODOT:-/c/Users/lucin/Documents/Developement/Godot/Godot_v4.7.1-stable_win64_console.exe}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
USERDATA="/c/Users/lucin/AppData/Roaming/Godot/app_userdata/Farlands"
SHOTS="$ROOT/.freebuff/shots"

games=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name like '%Godot%'\" | Where-Object { \$_.CommandLine -notmatch '--editor(\s|$)' } | Measure-Object).Count" 2>/dev/null | tr -d '\r')
if [ "${games:-0}" != "0" ]; then
	echo "run_probe_shot: a Godot game process is running; close it first (a probe must never" >&2
	echo "                run beside the player's game)" >&2
	exit 2
fi

mkdir -p "$SHOTS"
timeout "$TIMEOUT" "$GODOT" --path "$ROOT" \
	--script "res://$PROBE" 2>&1 | grep -E "PROBE|probe:|SCRIPT ERROR|Parse Error|at: |ERROR"
status=${PIPESTATUS[0]}
echo "run_probe_shot: probe exited $status"

# The screenshots are the point of the run: copy them out, leave them in place.
harvested=0
for dir in menu_shots paste_shots shader_shots heart_shots body_shots brightness_shots lod_grid_shots lod_depth_shots; do
	if [ -d "$USERDATA/$dir" ]; then
		mkdir -p "$SHOTS/$dir"
		cp -r "$USERDATA/$dir"/. "$SHOTS/$dir/" 2>/dev/null && harvested=$((harvested + 1))
	fi
done
echo "run_probe_shot: harvested $harvested shot directory(s) into $SHOTS"
exit "$status"
