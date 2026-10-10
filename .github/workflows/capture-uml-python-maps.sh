#!/bin/sh

# Sample the UML guest's process table while the SAS/zpoline LTP guest runs.
# This script is started by the guest init script: host-side /proc only shows
# UML host processes, not guest tasks. The hostfs root exposes the workspace
# at the same path, so snapshots survive a guest kernel panic.

base=/github/workspace/output/ltp/uml-python-debug/guest-processes
mkdir -p "$base"

while :; do
	for proc in /proc/[0-9]*; do
		[ -r "$proc/comm" ] || continue
		comm=$(cat "$proc/comm" 2>/dev/null) || continue
		case "$comm" in
			python3|python3.14) ;;
			*) continue ;;
		esac

		pid=${proc#/proc/}
		dir="$base/$pid"
		mkdir -p "$dir"
		for item in maps status auxv stat; do
			tmp="$dir/$item.tmp"
			if cat "$proc/$item" > "$tmp" 2>/dev/null; then
				mv "$tmp" "$dir/$item"
			else
				rm -f "$tmp"
			fi
		done
		tr '\000' ' ' < "$proc/cmdline" > "$dir/cmdline.tmp" 2>/dev/null &&
			mv "$dir/cmdline.tmp" "$dir/cmdline"
		readlink "$proc/exe" > "$dir/exe" 2>/dev/null || true
		date -u '+%Y-%m-%dT%H:%M:%SZ' > "$dir/captured-at"
	done
	sleep 1
done
