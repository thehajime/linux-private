#!/bin/sh
set -eu

if [ "${RUN_LTP_BASH:-0}" != 1 ]; then
	if command -v apk >/dev/null 2>&1; then
		apk add --no-cache bash git python3 util-linux
	else
		apt-get update
		DEBIAN_FRONTEND=noninteractive apt-get install -y bash git python3 util-linux e2fsprogs parted qemu-utils
	fi
	export RUN_LTP_BASH=1
	exec /bin/bash "$0"
fi
set -euo pipefail

cd /github/workspace
mkdir -p output/ltp

case "$LTP_ARTIFACT_NAME" in
	""|*/*) echo "invalid LTP artifact name: $LTP_ARTIFACT_NAME" >&2; exit 1 ;;
esac
ltp_archive="/ltp-install-artifacts/ltp-alpine-${LTP_ARTIFACT_NAME}.tar.gz"
test -r "$ltp_archive"
tar xfz "$ltp_archive" -C /

cp .github/workflows/ltp-skip /ltp-skip
case "$LTP_TESTNAME" in
	um-mmu)
		echo ptrace06 >> /ltp-skip
		echo "Skipping known ptrace06 failure for um-mmu"
		;;
	um-nommu-seccomp|um-nommu-sas-seccomp)
		echo perf_event_open03 >> /ltp-skip
		echo "Skipping known perf_event_open03 failure for $LTP_TESTNAME"
		;;
esac

git clone -b fix-nommu https://github.com/thehajime/ltp
git -C ltp submodule update --init --recursive
cp .github/workflows/faketty /bin/faketty
chmod +x /bin/faketty

if [ "$LTP_TESTNAME" = riscv-nommu ]; then
	qemu-img create -f raw /ltp-image.img 2G
	mkfs.ext2 -d /opt /ltp-image.img
	faketty env LTP_SINGLE_FS_TYPE=ext4 /opt/ltp/kirk \
		--sut default:com=qemu --com \
		qemu:image=/work-buildroot/buildroot/output/images/rootfs.ext2:kernel=/work-buildroot/buildroot/output/images/Image:system=riscv64:user=root \
		-f syscalls --run-pattern '' --skip-file /ltp-skip -o /result.json \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
elif [ "$LTP_TESTNAME" = native ]; then
	faketty env LTP_SINGLE_FS_TYPE=ext4 /opt/ltp/kirk \
		-f syscalls --run-pattern '' --skip-file /ltp-skip -o /result.json \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
else
	sh -x ltp/ci/alpine-runtime.sh
	if command -v apk >/dev/null 2>&1; then apk del btrfs-progs || true; fi
	cp .github/workflows/ltp-exec.sh /ltp-exec.sh
	sed "s/cmd_option/${UML_OPTIONS}/" .github/workflows/ltp-entrypoint.sh \
		> /tmp/ltp-entrypoint.sh
	mv /tmp/ltp-entrypoint.sh .github/workflows/ltp-entrypoint.sh
	faketty sh -x .github/workflows/ltp-entrypoint.sh LTP_SINGLE_FS_TYPE=ext4 \
		| tee "output/ltp/${LTP_TESTNAME}-ltp.dat"
fi

./ltp/tools/kirk/kirk-src/utils/json2html.py -r /result.json \
	> "output/ltp/ltp-result-${LTP_TESTNAME}.html"
cp /result.json "output/ltp/result-${LTP_TESTNAME}.json"
cp -rpf /github/home/kirk.root/ output/ltp/ 2>/dev/null || true
cp -rpf /tmp/kirk.root/ output/ltp/ 2>/dev/null || true
