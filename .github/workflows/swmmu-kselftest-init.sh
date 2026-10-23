#!/bin/busybox sh

if ! /bin/busybox mount -t proc proc /proc; then
	echo PROCFS_MOUNT_FAILED
	/bin/busybox poweroff -f
	exit 1
fi
mode=0
case "$(/bin/busybox cat /proc/cmdline)" in *zpoline=1*) mode=1;; esac
status=0
for test in nommu_swmmu_test nommu_zpoline_test nommu_shared_file_test; do
	[ -x "/tests/$test" ] || continue
	log="/tests/$test-z$mode.tap"
	exit_file="/tests/$test-z$mode.exit"
	if [ "$test" = nommu_shared_file_test ]; then
		if ! /bin/busybox mount -t tmpfs tmpfs /tmp; then
			echo TMPFS_MOUNT_FAILED > "$log"
			echo 1 > "$exit_file"
			status=1
			continue
		fi
	fi
	"/tests/$test" "$mode" > "$log" 2>&1
	test_status=$?
	printf '%s\n' "$test_status" > "$exit_file"
	[ "$test_status" -eq 0 ] || status=1
done
/bin/busybox sync
/bin/busybox poweroff -f
exit "$status"
