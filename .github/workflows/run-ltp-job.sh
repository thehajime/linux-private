#!/bin/sh
set -eu
set -o pipefail

cd /github/workspace
mkdir -p output/ltp

# Keep full workflow_dispatch suites below the job's 240 minute timeout so
# Kirk can write JSON/HTML diagnostics before the runner terminates the job.
# RISC-V reached its final test only near the old 180 minute job limit.
LTP_SUITE_TIMEOUT=${LTP_SUITE_TIMEOUT:-225m}

if [ "$LTP_TESTNAME" = native ]; then
	# The native job uses a stock Alpine image rather than the locally built
	# UML image, so install the runtime packages normally supplied by it.
	apk add --no-cache acl curl jq keyutils libaio libcap libselinux \
		libsepol libtirpc numactl openssl py3-msgpack e2fsprogs xfsprogs \
		dosfstools parted util-linux python3 bash git shadow
fi

case "$LTP_ARTIFACT_NAME" in
	""|*/*) echo "invalid LTP artifact name: $LTP_ARTIFACT_NAME" >&2; exit 1 ;;
esac
ltp_archive="/ltp-install-artifacts/ltp-alpine-${LTP_ARTIFACT_NAME}.tar.gz"
if [ "${LTP_INSTALL_PREEXTRACTED:-0}" = 1 ]; then
	test -x /opt/ltp/kirk
else
	test -r "$ltp_archive"
	# Restore installed helper/directory modes even under umask 0077.
	tar xpfz "$ltp_archive" -C /
fi

ltp_source=/ltp-source
test -d "$ltp_source/tools/kirk/kirk-src"

cp .github/workflows/ltp-skip /ltp-skip
if [ "$LTP_TESTNAME" = um-nommu-sas-zpoline ] ||
	[ "$LTP_TESTNAME" = um-nommu-plain-sas-zpoline ]; then
	mkdir -p output/ltp/uml-python-debug
fi
case "$LTP_TESTNAME" in
	native)
		# GitHub's ubuntu-24.04 runner currently uses an Azure kernel without
		# 736b380e28d0 ("ipv6: account for fraggap on the paged allocation
		# path").  setsockopt11 reproduces that host bug in skb_release_data(),
		# taints the host with an Oops, and makes later taint-sensitive tests
		# BROK.  Restrict the exclusion to the diagnosed kernel release so a
		# fixed or updated runner automatically restores the regression test.
		if [ "$(uname -r)" = 6.17.0-1022-azure ]; then
			echo setsockopt11 >> /ltp-skip
			echo "Skipping setsockopt11: runner kernel lacks 736b380e28d0 and Oopses"
		fi
		;;
	riscv-nommu)
		# This pthread futex test reports TBROK under the uClibc NOMMU
		# target and leaves Kirk's QEMU channel waiting for the next shell
		# prompt.  An otherwise identical full run completes in about eight
		# minutes when this unsupported case is omitted.
		echo futex_wait03 >> /ltp-skip
		echo "Skipping futex_wait03: RISC-V NOMMU pthread failure stalls the QEMU channel"
		;;
	um-nommu)
		echo epoll_wait01 >> /ltp-skip
		echo "Skipping known epoll_wait01 kernel panic for um-nommu"
		;;
	um-nommu-seccomp)
		echo perf_event_open03 >> /ltp-skip
		echo "Skipping known perf_event_open03 failure for $LTP_TESTNAME"
		echo waitpid01 >> /ltp-skip
		echo "Skipping waitpid01 associated with NOMMU seccomp exit panic"
		;;
	um-nommu-sas-seccomp)
		echo perf_event_open03 >> /ltp-skip
		echo "Skipping known perf_event_open03 failure for $LTP_TESTNAME"
		;;
	um-nommu-sas-zpoline|um-nommu-plain-sas-zpoline)
		# This LTP case deliberately dereferences a low address in the guest.
		# On SAS NOMMU the kernel panics without exiting UML, leaving Kirk's
		# docker exec blocked indefinitely and preventing diagnostics upload.
		echo nullptr >> /ltp-skip
		echo "Skipping nullptr: it panics and hangs the SAS NOMMU guest"
		# munmap04 reaches TPASS after creating 65,518 mappings, but zpoline
		# syscall interception makes teardown exceed LTP's 5m24s timeout.  The
		# same kernel and rootfs pass under seccomp in 57.6 seconds.
		echo munmap04 >> /ltp-skip
		echo "Skipping munmap04: zpoline teardown exceeds its LTP timeout"
		;;
esac

case "$LTP_TESTNAME" in
	um-nommu*)
		# These cases require MMU-only services (kcmp, mlock/munlock, or a MAP_32BIT
		# allocation below 2 GiB) that this NOMMU address-space model lacks.
		for test in kcmp03 mincore02 mmap23; do
			echo "$test" >> /ltp-skip
		done
		case "$LTP_TESTNAME" in
			um-nommu-sas-seccomp|um-nommu-sas-zpoline)
				if grep -q 'static int ipc_nommu_heap' "$ltp_source/lib/tst_test.c"; then
					grep -a -q 'mmap() for shared test IPC' /opt/ltp/testcases/bin/clone06 || {
						echo "Selected runtime NOMMU artifact lacks the shared-counter fix" >&2
						exit 1
					}
					echo "Enabling private clone result tests with shared NOMMU IPC"
				else
					printf '%s\n' clone06 clone08 >> /ltp-skip
				fi
				;;
			*) printf '%s\n' clone06 clone08 >> /ltp-skip ;;
		esac
		echo "Skipping NOMMU-incompatible MMU-only cases"
		;;
esac

cp .github/workflows/faketty /bin/faketty
chmod +x /bin/faketty

if [ "${LTP_FULL_RUN:-true}" = true ]; then
	LTP_RUN_PATTERN=
	: > output/ltp/run-pattern.txt
	printf 'full syscalls suite (workflow_dispatch)\n' > output/ltp/run-mode.txt
else
	LTP_RUN_PATTERN='getpid01|getppid01|getuid01'
	printf '%s\n' "$LTP_RUN_PATTERN" > output/ltp/run-pattern.txt
	printf 'push smoke subset\n' > output/ltp/run-mode.txt
fi

if [ -n "${LTP_RUN_PATTERN_OVERRIDE:-}" ]; then
	LTP_RUN_PATTERN=$LTP_RUN_PATTERN_OVERRIDE
	printf '%s\n' "$LTP_RUN_PATTERN" > output/ltp/run-pattern.txt
	printf 'focused run-pattern override\n' > output/ltp/run-mode.txt
fi

# Preserve the exact selection so a timed-out partial JSON cannot pass CI.
cp /opt/ltp/runtest/syscalls "output/ltp/runtest-${LTP_TESTNAME}.txt"
cp /ltp-skip "output/ltp/skip-${LTP_TESTNAME}.txt"

run_faketty()
{
	case "$LTP_TESTNAME" in
		um-nommu*)
			# docker exec allocates the required pseudo-terminal for NOMMU jobs.
			# The image's util-linux script binary is not runnable by the host
			# process when the image uses the NOMMU musl profile.
			"$@"
			;;
		*)
			faketty "$@"
			;;
	esac
}

if [ "$LTP_TESTNAME" = riscv-nommu ]; then
	qemu-img create -f raw /ltp-image.img 2G
	mkfs.ext2 -d /opt /ltp-image.img
	run_faketty env LTP_SINGLE_FS_TYPE=ext4 /opt/ltp/kirk \
		--sut default:com=qemu --com \
		qemu:image=/work-buildroot/buildroot/output/images/rootfs.ext2:kernel=/work-buildroot/buildroot/output/images/Image:system=riscv64:user=root \
		-f syscalls --suite-timeout "$LTP_SUITE_TIMEOUT" \
		--run-pattern "$LTP_RUN_PATTERN" --skip-file /ltp-skip -o /result.json \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
elif [ "$LTP_TESTNAME" = native ]; then
	. .github/workflows/ltp-runtime.sh
	ltp_runtime_prepare native
	dmesg > output/ltp/native-dmesg-before.txt
	cat /proc/sys/kernel/tainted > output/ltp/native-tainted-before.txt
	ltp_native_status=0
	run_faketty env LTP_SINGLE_FS_TYPE=ext4 /opt/ltp/kirk \
		-f syscalls --suite-timeout "$LTP_SUITE_TIMEOUT" \
		--run-pattern "$LTP_RUN_PATTERN" --skip-file /ltp-skip -o /result.json \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat" || ltp_native_status=$?
else
	cp .github/workflows/ltp-exec.sh /ltp-exec.sh
	chmod 755 /ltp-exec.sh
	sed "s/cmd_option/${UML_OPTIONS}/" .github/workflows/ltp-entrypoint.sh \
		> /tmp/ltp-entrypoint.sh
	mv /tmp/ltp-entrypoint.sh .github/workflows/ltp-entrypoint.sh
	case "$LTP_TESTNAME" in
		um-nommu*)
			# Match the NOMMU validation kit's UML virtualization setting.
			# An explicit empty override retains raw timing assertions for
			# focused diagnostics.
			set -- LTP_SINGLE_FS_TYPE=ext4 \
				"LTP_VIRT_OVERRIDE=${LTP_VIRT_OVERRIDE-uml}"
			set -- "$@" LTP_TESTNAME="$LTP_TESTNAME" \
				LTP_SUITE_TIMEOUT="$LTP_SUITE_TIMEOUT"
			run_faketty sh -x .github/workflows/ltp-entrypoint.sh "$@"
			;;
		*)
			run_faketty sh -x .github/workflows/ltp-entrypoint.sh LTP_SINGLE_FS_TYPE=ext4 \
				"LTP_VIRT_OVERRIDE=${LTP_VIRT_OVERRIDE-uml}" \
				LTP_TESTNAME="$LTP_TESTNAME" LTP_SUITE_TIMEOUT="$LTP_SUITE_TIMEOUT" \
				| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
			;;
	esac
fi

case "$LTP_TESTNAME" in
	um-nommu*)
		# ltp-exec.sh produces these while the optimized guest is active.
		test -s "output/ltp/ltp-result-${LTP_TESTNAME}.html"
		test -s "output/ltp/result-${LTP_TESTNAME}.json"
		;;
	*)
		"$ltp_source/tools/kirk/kirk-src/utils/json2html.py" -r /result.json \
			> "output/ltp/ltp-result-${LTP_TESTNAME}.html"
		cp /result.json "output/ltp/result-${LTP_TESTNAME}.json"
		cp -rpf /github/home/kirk.root/ output/ltp/ 2>/dev/null || true
		cp -rpf /tmp/kirk.root/ output/ltp/ 2>/dev/null || true
		;;
esac

if [ "$LTP_TESTNAME" = native ]; then
	dmesg > output/ltp/native-dmesg-after.txt
	cat /proc/sys/kernel/tainted > output/ltp/native-tainted-after.txt
	cp /tmp/ltp-mdev.log output/ltp/native-mdev.log
	cp /sys/fs/cgroup/memory.events output/ltp/native-memory-events.txt
	printf '%s\n' "$ltp_native_status" > output/ltp/native-complete-status.txt
	echo "LTP_NATIVE_COMPLETE=$ltp_native_status"
	exit "$ltp_native_status"
fi
