#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LM_BENCH_BIN_DIR=${LMBENCH_BIN_DIR:-$SCRIPT_DIR}
PATH="$LM_BENCH_BIN_DIR:$PATH"
export PATH

lmbench_tmpdir=${LMBENCH_TMPDIR:-/var/tmp/lmbench}
mkdir -p "$lmbench_tmpdir"
if command -v hello >/dev/null 2>&1; then
	cp "$(command -v hello)" "$lmbench_tmpdir/hello"
	cp "$(command -v hello)" /tmp/hello
fi

mkdir -p /usr/include/sys || true
touch /usr/include/sys/types.h || true

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

ENOUGH=10000 lat_select -n 10 file
ENOUGH=10000 lat_select -n 100 file
ENOUGH=10000 lat_select -n 1000 file

ENOUGH=100000 lat_syscall null
ENOUGH=100000 lat_syscall read
ENOUGH=100000 lat_syscall write
ENOUGH=100000 lat_syscall stat
ENOUGH=100000 lat_syscall open

for method in $process_methods; do
	echo "lmbench process method: $method"
	ENOUGH=10000 lat_proc "--$method" fork
	ENOUGH=10000 lat_proc "--$method" exec
	ENOUGH=10000 lat_proc "--$method" shell
done

ENOUGH=10000 bw_pipe -m 64k -M 64k
ENOUGH=10000 bw_mem 512k rdwr
