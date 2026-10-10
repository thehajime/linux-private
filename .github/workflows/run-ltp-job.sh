#!/bin/sh
set -eu
set -o pipefail

cd /github/workspace
mkdir -p output/ltp

if [ "$LTP_TESTNAME" = native ]; then
	# The native job uses a stock Alpine image rather than the locally built
	# UML image, so install the runtime packages normally supplied by it.
	apk add --no-cache acl curl jq keyutils libaio libcap libselinux \
		libsepol libtirpc numactl openssl py3-msgpack e2fsprogs xfsprogs \
		dosfstools parted util-linux python3 bash git
fi

case "$LTP_ARTIFACT_NAME" in
	""|*/*) echo "invalid LTP artifact name: $LTP_ARTIFACT_NAME" >&2; exit 1 ;;
esac
ltp_archive="/ltp-install-artifacts/ltp-alpine-${LTP_ARTIFACT_NAME}.tar.gz"
if [ "${LTP_INSTALL_PREEXTRACTED:-0}" = 1 ]; then
	test -x /opt/ltp/kirk
else
	test -r "$ltp_archive"
	tar xfz "$ltp_archive" -C /
fi

cp .github/workflows/ltp-skip /ltp-skip
if [ "$LTP_TESTNAME" = um-nommu-sas-zpoline ]; then
	mkdir -p output/ltp/uml-python-debug
fi
case "$LTP_TESTNAME" in
	um-nommu)
		echo epoll_wait01 >> /ltp-skip
		echo "Skipping known epoll_wait01 kernel panic for um-nommu"
		;;
	um-mmu)
		echo ptrace06 >> /ltp-skip
		echo "Skipping known ptrace06 failure for um-mmu"
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
	um-nommu-sas-zpoline)
		# This LTP case deliberately dereferences a low address in the guest.
		# On SAS NOMMU the kernel panics without exiting UML, leaving Kirk's
		# docker exec blocked indefinitely and preventing diagnostics upload.
		echo nullptr >> /ltp-skip
		echo "Skipping nullptr: it panics and hangs the SAS NOMMU guest"
		;;
esac

ltp_source=/ltp-source
test -d "$ltp_source/tools/kirk/kirk-src"
cp .github/workflows/faketty /bin/faketty
chmod +x /bin/faketty

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
		-f syscalls --run-pattern '' --skip-file /ltp-skip -o /result.json \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
elif [ "$LTP_TESTNAME" = native ]; then
	run_faketty env LTP_SINGLE_FS_TYPE=ext4 /opt/ltp/kirk \
		-f syscalls --run-pattern '' --skip-file /ltp-skip -o /result.json \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
else
	cp .github/workflows/ltp-exec.sh /ltp-exec.sh
	sed "s/cmd_option/${UML_OPTIONS}/" .github/workflows/ltp-entrypoint.sh \
		> /tmp/ltp-entrypoint.sh
	mv /tmp/ltp-entrypoint.sh .github/workflows/ltp-entrypoint.sh
	case "$LTP_TESTNAME" in
		um-nommu*)
			run_faketty sh -x .github/workflows/ltp-entrypoint.sh \
				LTP_SINGLE_FS_TYPE=ext4 LTP_TESTNAME="$LTP_TESTNAME"
			;;
		*)
			run_faketty sh -x .github/workflows/ltp-entrypoint.sh LTP_SINGLE_FS_TYPE=ext4 \
				LTP_TESTNAME="$LTP_TESTNAME" \
				| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
			;;
	esac
fi

"$ltp_source/tools/kirk/kirk-src/utils/json2html.py" -r /result.json \
	> "output/ltp/ltp-result-${LTP_TESTNAME}.html"
cp /result.json "output/ltp/result-${LTP_TESTNAME}.json"
cp -rpf /github/home/kirk.root/ output/ltp/ 2>/dev/null || true
cp -rpf /tmp/kirk.root/ output/ltp/ 2>/dev/null || true
