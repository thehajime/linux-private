#!/bin/sh
set -eu
set -o pipefail

: "${SWMMU_INIT_SCRIPT:?Set SWMMU_INIT_SCRIPT to the nginx guest init script}"
: "${SWMMU_WORK:?Set SWMMU_WORK to a writable workspace directory}"
: "${SWMMU_PREBUILT_PROFILE:?Set SWMMU_PREBUILT_PROFILE to the matching signed SWMMU APK artifact}"

ROOTFS="$SWMMU_WORK/rootfs"
OUT="$SWMMU_WORK/artifact"
LOGS="$SWMMU_WORK/logs"
PACKAGES="$SWMMU_PREBUILT_PROFILE/packages"
KEYS="${SWMMU_PREBUILT_PROFILE%/*}/keys"
TOOLS="$SWMMU_PREBUILT_PROFILE/tools"
MANIFEST="${SWMMU_PREBUILT_PROFILE%/*}/manifest.txt"

rm -rf "$ROOTFS" "$OUT"
mkdir -p "$ROOTFS" "$OUT/packages" "$OUT/tools" "$OUT/keys" "$LOGS"

for package in busybox-nommu musl-nommu libcrypto3 libssl3 nginx-nommu coreutils-nommu; do
	set -- "$PACKAGES/$package"-*.apk
	[ "$#" -eq 1 ] && [ -f "$1" ] || {
		echo "expected one aports prerelease APK for $package in $PACKAGES" >&2
		exit 1
	}
	cp "$1" "$OUT/packages/"
done
test -s "$TOOLS/busybox-host"
set -- "$KEYS/"*.rsa.pub
[ "$#" -eq 1 ] && [ -f "$1" ] || {
	echo "expected one aports prerelease public key in $KEYS" >&2
	exit 1
}
cp "$1" "$OUT/keys/"
cp "$TOOLS/busybox-host" "$OUT/tools/busybox-host"

kernel_sha=$(sed -n 's/^kernel_commit=//p' "$MANIFEST")
aports_sha=$(sed -n 's/^aports_commit=//p' "$MANIFEST")
gcc_version=$(cat "$SWMMU_PREBUILT_PROFILE/toolchain/gcc-version")
test -n "$kernel_sha"
test -n "$aports_sha"

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
	2>&1 | tee "$LOGS/base-rootfs.log"
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
	2>"$LOGS/certificate.log"
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
cp -a "$LOGS" "$OUT/logs"
tar --numeric-owner --owner=0 --group=0 -czf "$OUT/rootfs.tar.gz" -C "$ROOTFS" .
printf 'Prepared SWMMU nginx rootfs from aports %s (kernel %s, GCC %s)\n' \
	"$aports_sha" "$kernel_sha" "$gcc_version"
