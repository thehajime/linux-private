#!/bin/sh

export PATH=/home:/sbin:/usr/sbin:/bin:/usr/bin

/bin/busybox mount -t proc proc /proc || exit 1
/bin/busybox mkdir -p /sys /dev/shm /dev/pts
/bin/busybox mount -t sysfs sysfs /sys || true
/bin/busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /dev/shm || true
/bin/busybox mount -t devpts devpts /dev/pts || true

case "${LTP_DIRECT_TEST:-}" in
	setns02|clock_gettime04|nanosleep01) ;;
	*) echo "unsupported direct test: ${LTP_DIRECT_TEST:-unset}"; exit 2 ;;
esac

test_bin="/opt/ltp/testcases/bin/$LTP_DIRECT_TEST"
if [ ! -x "$test_bin" ]; then
	echo "missing LTP binary: $test_bin"
	exit 2
fi

echo "LTP_DIRECT_START test=$LTP_DIRECT_TEST pid=$$"
"$test_bin"
test_rc=$?
echo "LTP_DIRECT_EXIT_CODE=$test_rc"
printf '%s\n' "$test_rc" > "/github/workspace/output/ltp/direct-${LTP_DIRECT_TEST}.status"
/bin/busybox poweroff -f
exit "$test_rc"
