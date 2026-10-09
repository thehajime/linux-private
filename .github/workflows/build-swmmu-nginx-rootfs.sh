#!/bin/sh
set -eu
set -o pipefail

: "${SWMMU_KERNEL_SRC:?Set SWMMU_KERNEL_SRC to the selected Linux source tree}"
: "${SWMMU_APORTS_SRC:?Set SWMMU_APORTS_SRC to the pinned aports checkout}"
: "${SWMMU_INIT_SCRIPT:?Set SWMMU_INIT_SCRIPT to the nginx guest init script}"
: "${SWMMU_WORK:?Set SWMMU_WORK to a writable workspace directory}"

ROOTFS="$SWMMU_WORK/rootfs"
BUILD="$SWMMU_WORK/kernel-build"
UAPI="$SWMMU_WORK/kernel-uapi/include"
SYSROOT="$SWMMU_WORK/sysroot"
REPODEST="$SWMMU_WORK/repo"
OUT="$SWMMU_WORK/artifact"
PACKAGES="$REPODEST/main/x86_64"

rm -rf "$ROOTFS" "$BUILD" "$SYSROOT" "$REPODEST" "$OUT" "$SWMMU_WORK/aports"
mkdir -p "$SWMMU_WORK/logs" "$REPODEST" "$OUT/packages" "$OUT/tools" "$OUT/keys"
mkdir -p "$ROOTFS" "$SYSROOT"

# This script runs as root inside setup-alpine while the checkouts remain owned
# by the runner user. Allow Git to inspect only these two mounted worktrees.
git config --global --add safe.directory "$SWMMU_KERNEL_SRC"
git config --global --add safe.directory "$SWMMU_APORTS_SRC"
kernel_sha=$(git -C "$SWMMU_KERNEL_SRC" rev-parse HEAD)
aports_sha=$(git -C "$SWMMU_APORTS_SRC" rev-parse HEAD)
gcc_version=$(gcc -dumpfullversion)
expected_aports_sha=2fa7f100befb14cb343a659446e539237c66366c
[ "$aports_sha" = "$expected_aports_sha" ] || {
	echo "unexpected SWMMU aports revision: $aports_sha" >&2
	exit 1
}

make -C "$SWMMU_KERNEL_SRC" ARCH=um O="$BUILD" x86_64_defconfig \
	2>&1 | tee "$SWMMU_WORK/logs/kernel-config.log"
"$SWMMU_KERNEL_SRC/scripts/config" --file "$BUILD/.config" \
	--disable MMU \
	--enable UML_NOMMU_SAS \
	--enable NOMMU_SWMMU \
	--enable NOMMU_SWMMU_DEFAULT_ON \
	--enable GCC_PLUGINS \
	--enable GCC_PLUGIN_SWMMU
make -C "$SWMMU_KERNEL_SRC" ARCH=um O="$BUILD" olddefconfig \
	2>&1 | tee -a "$SWMMU_WORK/logs/kernel-config.log"
grep -q '^CONFIG_NOMMU_SWMMU=y$' "$BUILD/.config"
grep -q '^CONFIG_GCC_PLUGIN_SWMMU=y$' "$BUILD/.config"
make -C "$SWMMU_KERNEL_SRC" ARCH=um O="$BUILD" scripts \
	2>&1 | tee "$SWMMU_WORK/logs/kernel-plugin.log"
make -C "$SWMMU_KERNEL_SRC" ARCH=um O="$BUILD" \
	INSTALL_HDR_PATH="${UAPI%/include}" headers_install \
	2>&1 | tee "$SWMMU_WORK/logs/kernel-headers.log"
test -r "$BUILD/scripts/gcc-plugins/swmmu_plugin.so"
test -r "$UAPI/linux/nommu_swmmu.h"

addgroup -S abuild 2>/dev/null || true
adduser -D -G abuild builder
mkdir -p /etc/doas.d /var/cache/distfiles
printf 'permit nopass builder as root\n' >/etc/doas.d/abuild.conf
chmod 0400 /etc/doas.d/abuild.conf

APORTS_BUILD="$SWMMU_WORK/aports"
mkdir -p "$APORTS_BUILD/main" "$APORTS_BUILD/scripts"
for package in musl busybox openssl nginx coreutils; do
	cp -a "$SWMMU_APORTS_SRC/main/$package" "$APORTS_BUILD/main/"
done
cp -a "$SWMMU_APORTS_SRC/scripts/swmmu-profile.sh" \
	"$SWMMU_APORTS_SRC/scripts/swmmu-cc" \
	"$SWMMU_APORTS_SRC/scripts/swmmu-prepare.sh" "$APORTS_BUILD/scripts/"
chown -R builder:abuild "$SWMMU_WORK" /var/cache/distfiles

su -s /bin/sh builder -c '
set -eu
work=$1
kernel_src=$2
export PACKAGER="UML CI <uml-ci@example.invalid>"
export REPODEST="$work/repo"
export SWMMU=1
export SWMMU_KERNEL_SRC="$kernel_src"
export SWMMU_KERNEL_BUILD="$work/kernel-build"
export SWMMU_PLUGIN="$work/kernel-build/scripts/gcc-plugins/swmmu_plugin.so"
export SWMMU_UAPI="$work/kernel-uapi/include"
export SWMMU_SYSROOT="$work/sysroot"
export SWMMU_WRAPPER_SOURCE="$work/aports/scripts/swmmu-cc"
export SWMMU_GCC=/usr/bin/gcc
abuild-keygen -a -i -n

cd "$work/aports/main/musl"; abuild -r
cd "$work/aports/main/busybox"; abuild -r
"$work/aports/scripts/swmmu-prepare.sh" check
cd "$work/aports/main/openssl"; SWMMU_ONLY=1 abuild -r
cd "$work/aports/main/nginx"; SWMMU_ONLY=1 abuild -r
cd "$work/aports/main/coreutils"; SWMMU_ONLY=1 abuild -r

set -- "$HOME"/.config/abuild/*.rsa.pub
[ -f "$1" ] || set -- "$HOME"/.abuild/*.rsa.pub
[ "$#" -eq 1 ] && [ -f "$1" ]
cp "$1" "$work/artifact/keys/"
' swmmu-build "$SWMMU_WORK" "$SWMMU_KERNEL_SRC" 2>&1 | tee "$SWMMU_WORK/logs/apk-build.log"

for package in musl-nommu busybox-nommu libcrypto3 libssl3 nginx-nommu coreutils-nommu busybox-static; do
	set -- "$PACKAGES/$package"-*.apk
	[ "$#" -eq 1 ] && [ -f "$1" ] || {
		echo "expected one APK for $package in $PACKAGES" >&2
		exit 1
	}
	cp "$1" "$OUT/packages/"
done

set -- "$OUT/packages/busybox-static"-*.apk
tar -xOf "$1" bin/busybox.static >"$OUT/tools/busybox-host"
chmod 0755 "$OUT/tools/busybox-host"

mkdir -p "$SWMMU_WORK/apk-keys"
cp /etc/apk/keys/*.rsa.pub "$SWMMU_WORK/apk-keys/"
cp "$OUT/keys/"*.rsa.pub "$SWMMU_WORK/apk-keys/"
apk --root "$ROOTFS" --initdb --keys-dir "$SWMMU_WORK/apk-keys" \
	--repositories-file /etc/apk/repositories add --no-cache --no-scripts \
	alpine-baselayout alpine-keys apk-tools ca-certificates-bundle \
	"$OUT/packages/musl-nommu"-*.apk \
	"$OUT/packages/busybox-nommu"-*.apk \
	"$OUT/packages/libcrypto3"-*.apk \
	"$OUT/packages/libssl3"-*.apk \
	"$OUT/packages/nginx-nommu"-*.apk \
	"$OUT/packages/coreutils-nommu"-*.apk \
	2>&1 | tee "$SWMMU_WORK/logs/base-rootfs.log"
mkdir -p "$ROOTFS/etc/apk/keys"
cp "$OUT/keys/"*.rsa.pub "$ROOTFS/etc/apk/keys/"

"$OUT/tools/busybox-host" --install -s "$ROOTFS"
mkdir -p "$ROOTFS/var/www/localhost/htdocs" "$ROOTFS/var/lib/nginx/tmp" \
	"$ROOTFS/tmp" "$ROOTFS/run" "$ROOTFS/proc" "$ROOTFS/sys"
printf 'swmmu-nginx-ok\n' >"$ROOTFS/var/www/localhost/htdocs/index.html"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
	-subj '/CN=swmmu-nginx.local' \
	-keyout "$ROOTFS/etc/nginx/swmmu.key" \
	-out "$ROOTFS/etc/nginx/swmmu.crt" \
	2>"$SWMMU_WORK/logs/certificate.log"
chmod 0600 "$ROOTFS/etc/nginx/swmmu.key"
install -m 0755 "$SWMMU_INIT_SCRIPT" "$ROOTFS/sbin/swmmu-nginx-init"

cat >"$OUT/manifest.txt" <<EOF
kernel_sha=$kernel_sha
aports_sha=$aports_sha
gcc_version=$gcc_version
profile=SWMMU
packages=musl-nommu,busybox-nommu,libcrypto3,libssl3,nginx-nommu,coreutils-nommu
network=guest 172.18.0.2/16 via host 172.18.0.1/16 on tap100
EOF
for package in "$OUT/packages/"*.apk; do
	sha256sum "$package" >>"$OUT/manifest.txt"
done
cp -a "$SWMMU_WORK/logs" "$OUT/logs"
tar --numeric-owner --owner=0 --group=0 -czf "$OUT/rootfs.tar.gz" -C "$ROOTFS" .
printf 'Prepared SWMMU nginx rootfs from kernel %s and aports %s (GCC %s)\n' \
	"$kernel_sha" "$aports_sha" "$gcc_version"
