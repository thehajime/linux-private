#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LM_BENCH_BIN_DIR=${LMBENCH_BIN_DIR:-$SCRIPT_DIR}
PATH="$LM_BENCH_BIN_DIR:$PATH"
export PATH

lmbench_tmpdir=${LMBENCH_TMPDIR:-/var/tmp/lmbench}
mkdir -p "$lmbench_tmpdir"
lmbench_file="$lmbench_tmpdir/lmbench-target"
: >"$lmbench_file"
if command -v hello >/dev/null 2>&1; then
	cp "$(command -v hello)" "$lmbench_tmpdir/hello"
	cp "$(command -v hello)" /tmp/hello
fi

process_method=${LMBENCH_PROCESS_METHOD:-auto}
if [ "$process_method" = auto ]; then
	if [ -e /.lmbench-nommu ]; then
		process_method=vfork
	else
		process_method=both
	fi
fi

case "$process_method" in
	fork|vfork) process_methods=$process_method ;;
	both) process_methods="fork vfork" ;;
	*) echo "Unknown LMBENCH_PROCESS_METHOD: $process_method" >&2; exit 2 ;;
esac
echo "lmbench process methods: $process_methods"
measurement_failed=0

run_measurement() {
	measurement=$1
	shift
	if [ -n "${LMBENCH_MEASUREMENT_TIMEOUT_SECONDS:-}" ]; then
		if timeout --kill-after=5s "${LMBENCH_MEASUREMENT_TIMEOUT_SECONDS}s" "$@"; then
			echo "lmbench measurement status: $measurement PASS"
		else
			status=$?
			echo "lmbench measurement status: $measurement FAIL (exit $status)"
			measurement_failed=1
		fi
	elif "$@"; then
		echo "lmbench measurement status: $measurement PASS"
	else
		status=$?
		echo "lmbench measurement status: $measurement FAIL (exit $status)"
		measurement_failed=1
	fi
}

run_measurement select-10 env ENOUGH=10000 lat_select -n 10 file
run_measurement select-100 env ENOUGH=10000 lat_select -n 100 file
run_measurement select-1000 env ENOUGH=10000 lat_select -n 1000 file

run_measurement syscall env ENOUGH=100000 lat_syscall null
run_measurement read env ENOUGH=100000 lat_syscall read
run_measurement write env ENOUGH=100000 lat_syscall write
run_measurement stat env ENOUGH=100000 lat_syscall stat "$lmbench_file"
run_measurement open-close env ENOUGH=100000 lat_syscall open "$lmbench_file"

for method in $process_methods; do
	echo "lmbench process method: $method"
	run_measurement "$method-exit" env ENOUGH=10000 lat_proc "--$method" fork
	run_measurement "$method-execve" env ENOUGH=10000 lat_proc "--$method" exec
	run_measurement "$method-shell" env ENOUGH=10000 lat_proc "--$method" shell
done

run_measurement bw_pipe_64k env ENOUGH=10000 bw_pipe -m 64k -M 64k
run_measurement bw_mem_512k_rdwr env ENOUGH=10000 bw_mem 512k rdwr

exit "$measurement_failed"
