#!/bin/busybox sh

log=/tests/nommu_swmmu_test.tap
exit_file=/tests/nommu_swmmu_test.exit

echo SWMMU_KSELFTEST_BEGIN > "$log"
if ! /bin/busybox mount -t proc proc /proc; then
	echo PROCFS_MOUNT_FAILED >> "$log"
	echo 1 > "$exit_file"
	/bin/busybox poweroff -f
	exit 1
fi
/tests/nommu_swmmu_test >> "$log" 2>&1
test_status=$?
printf '%s\n' "$test_status" > "$exit_file"
/bin/busybox sync
/bin/busybox poweroff -f
exit "$test_status"
