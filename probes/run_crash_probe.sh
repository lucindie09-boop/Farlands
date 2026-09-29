#!/usr/bin/env bash
# Run the crash-reporter probe with the deliberate fault ARMED.
#
# Separate from run_probe.sh because that one restores the world directory and this
# one needs to inspect the crash folder afterwards (and a deliberate fault kills the
# process, so the two want different handling). The probe touches no blocks, so
# there is no world state to protect.
#
#   .freebuff/run_crash_probe.sh [timeout_seconds]
set -uo pipefail

TIMEOUT="${1:-180}"
GODOT="${GODOT:-/c/Users/lucin/Documents/Developement/Godot/Godot_v4.7.1-stable_win64_console.exe}"
CRASHES="/c/Users/lucin/AppData/Roaming/Godot/app_userdata/Farlands/crashes"

echo "=== before ==="
ls -la --time-style=+%H:%M:%S "$CRASHES" 2>/dev/null | tail -5

FARLANDS_CRASH_TEST=1 timeout "$TIMEOUT" "$GODOT" --path "$(cd "$(dirname "$0")/.." && pwd)" \
    -s .freebuff/probe_crash_dump.gd 2>&1 | grep -E "CRASH_PROBE|\[crash\]|ERROR|WARNING: " | head -30
echo "=== after (newest 4) ==="
ls -la --time-style=+%H:%M:%S "$CRASHES" 2>/dev/null | tail -4
