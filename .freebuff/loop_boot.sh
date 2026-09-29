#!/usr/bin/env bash
# Runs the boot probe N times, printing whatever the run said and its exit code,
# to hunt the intermittent startup crash (which fires about a second in).
#
# No user:// snapshot here on purpose: this probe only BOOTS the game and looks
# at it, so there is nothing to restore, and the editor currently holds the
# log file open.
#
#   .freebuff/loop_boot.sh [runs] [seconds each]
set -uo pipefail

RUNS="${1:-6}"
SECS="${2:-45}"
GODOT="${GODOT:-/c/Users/lucin/Documents/Developement/Godot/Godot_v4.7.1-stable_win64_console.exe}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

for i in $(seq 1 "$RUNS"); do
	out="$(timeout "$SECS" "$GODOT" --path "$ROOT" --script res://.freebuff/probe_boot.gd 2>&1)"
	code=$?
	line="$(printf '%s' "$out" | grep -E "PROBE|CrashHandler|signal|Exception|0x|ERROR: " | tail -3 | tr '\n' '|')"
	echo "run $i: exit=$code  ${line:-<no output>}"
	if [ "$code" != "0" ] && [ "$code" != "124" ]; then
		echo "---- full output of the failing run ----"
		printf '%s\n' "$out" | tail -45
		echo "---------------------------------------"
	fi
done
