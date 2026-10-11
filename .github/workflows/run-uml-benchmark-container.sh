#!/usr/bin/env bash
set -u -o pipefail

if [[ $# -lt 3 ]]; then
	echo "Usage: $0 CONTAINER_NAME OUTPUT_LOG COMMAND [ARG ...]" >&2
	exit 2
fi

container_name=$1
output_log=$2
shift 2

mkdir -p "$(dirname "$output_log")"
: >"$output_log"

"$@" 2>&1 | tee -a "$output_log" &
pipeline_pid=$!

while kill -0 "$pipeline_pid" 2>/dev/null; do
	if grep -Eq 'Kernel panic - not syncing:|Kernel BUG at|Oops:' "$output_log"; then
		echo "Detected a guest kernel failure; stopping container $container_name" >&2
		docker rm -f "$container_name" >/dev/null 2>&1 || true
		for _ in {1..10}; do
			kill -0 "$pipeline_pid" 2>/dev/null || break
			sleep 1
		done
		if kill -0 "$pipeline_pid" 2>/dev/null; then
			kill -TERM "$pipeline_pid" 2>/dev/null || true
		fi
		wait "$pipeline_pid" 2>/dev/null || true
		exit 70
	fi
	sleep 1
done

wait "$pipeline_pid"
status=$?
docker rm -f "$container_name" >/dev/null 2>&1 || true
exit "$status"
