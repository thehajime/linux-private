#!/bin/bash
set -euxo pipefail

export PATH=/work-buildroot/buildroot/output/host/bin:$PATH
export ARCH=riscv
export CC=riscv64-buildroot-linux-uclibc-gcc
export HOST_CFLAGS=-fPIC
export UCLIBC=1
export NOMMU=1

if ! command -v curl >/dev/null 2>&1; then
	apt-get update
	DEBIAN_FRONTEND=noninteractive apt-get install -y curl
fi

"$CC" --version
./ver_linux
./build.sh -r autotools
CFLAGS='-fPIC -Wl,-elf2flt=-r' LDFLAGS='-Wl,-elf2flt=-r' \
	./build.sh -r configure -o out -t cross -c "$CC"

sed 's/\(.*HAVE_GETAUXVAL.*\)/\/\* \1\*\//' ../ltp-build/include/config.h \
	> /tmp/ltp-config.h
mv /tmp/ltp-config.h ../ltp-build/include/config.h
patch -p1 < ./ltx-fork.patch
curl -fsSLO https://raw.githubusercontent.com/thehajime/ltp/refs/heads/fix-nommu/kirk-block.patch
patch -p1 < ./kirk-block.patch

./build.sh -r build -o out
./build.sh -r test-shell -o out -t cross
./build.sh -r test-shell-loader -o out -t cross
./build.sh -r install -o out -i

mkdir -p /run-artifacts
test -d /root/ltp-install/opt
tar cfz /run-artifacts/ltp-alpine-riscv-nommu.tar.gz \
	-C /root/ltp-install ./opt
