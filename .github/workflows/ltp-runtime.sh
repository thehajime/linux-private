#!/bin/sh
# Source from the native runner or the MMU guest before starting Kirk.

ltp_runtime_cleanup()
{
	ltp_cleanup_status=$1
	trap - EXIT
	if [ -n "${ltp_mdev_pid:-}" ]; then
		kill "$ltp_mdev_pid" 2>/dev/null || ltp_cleanup_status=1
		wait "$ltp_mdev_pid" || [ "$?" = 143 ] || ltp_cleanup_status=1
	fi
	if [ -n "${ltp_scratch_loop:-}" ]; then
		umount /ltp-scratch || ltp_cleanup_status=1
		losetup -d "$ltp_scratch_loop" || ltp_cleanup_status=1
	fi
	if [ "${ltp_runtime_mode:-}" = uml ]; then
		# A setup error in guest PID 1 must not turn into an init-exit
		# panic. The missing success marker still fails the CI gate.
		echo "LTP_RUNTIME_ABORT=$ltp_cleanup_status" >&2
		poweroff -f
	fi
	exit "$ltp_cleanup_status"
}

ltp_runtime_prepare()
{
	ltp_runtime_mode=$1
	ltp_mdev_pid=
	ltp_scratch_loop=
	trap 'ltp_runtime_cleanup "$?"' EXIT
	if ! id ltp >/dev/null 2>&1; then adduser -D ltp; fi

	if [ "$ltp_runtime_mode" = native ]; then
		# This must be the container's private cgroup namespace, never the
		# host hierarchy. Move the idle entrypoint and this docker-exec shell.
		test -f /sys/fs/cgroup/cgroup.controllers
		grep -q '^0::/$' /proc/1/cgroup
		test "$(cat /sys/fs/cgroup/memory.max)" != max
		mkdir -p /sys/fs/cgroup/ltp-runner
		for ltp_pid in $(cat /sys/fs/cgroup/cgroup.procs); do
			if ! echo "$ltp_pid" > /sys/fs/cgroup/ltp-runner/cgroup.procs 2>/dev/null; then
				if [ -d "/proc/$ltp_pid" ]; then
					echo "Cannot delegate live container PID $ltp_pid" >&2
					return 1
				fi
			fi
		done
		echo +memory > /sys/fs/cgroup/cgroup.subtree_control

		# /dev must be the container's private tmpfs. Only loop partition
		# events are handled; no module loading or other device actions.
		printf '%s\n' 'loop[0-9]+p[0-9]+ 0:0 0660' '.* 0:0 0000 !' > /etc/mdev.conf
		busybox mdev -d -f > /tmp/ltp-mdev.log 2>&1 &
		ltp_mdev_pid=$!
	else
		mkdir -p /sys/fs/cgroup
		mount -t cgroup2 cgroup2 /sys/fs/cgroup
		export LTP_VIRT_OVERRIDE="${LTP_VIRT_OVERRIDE-uml}"
	fi

	test -c /dev/loop-control || mknod /dev/loop-control c 10 237
	for ltp_loop_index in 0 1 2 3 4 5 6 7; do
		test -b "/dev/loop$ltp_loop_index" ||
			mknod "/dev/loop$ltp_loop_index" b 7 "$ltp_loop_index"
	done
	mkdir -p /ltp-scratch
	truncate -s 2G /tmp/ltp-scratch.img
	if [ "$ltp_runtime_mode" = native ]; then
		ltp_scratch_loop=$(losetup --find --show /tmp/ltp-scratch.img)
	else
		# BusyBox losetup need not support --show. No other guest testcase
		# is running yet, so this guest's free-loop selection is exclusive.
		ltp_scratch_loop=$(losetup -f)
		losetup "$ltp_scratch_loop" /tmp/ltp-scratch.img
	fi
	mkfs.ext4 -q -F "$ltp_scratch_loop"
	mount "$ltp_scratch_loop" /ltp-scratch
	chmod 1777 /ltp-scratch
	export TMPDIR=/ltp-scratch LTP_SINGLE_FS_TYPE=ext4
	df -h /ltp-scratch
}
