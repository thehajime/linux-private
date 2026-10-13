#!/bin/sh
set -eu
set -o pipefail

: "${SWMMU_APK_DIR:?Set SWMMU_APK_DIR to the downloaded NOMMU APK artifact}"
: "${SWMMU_KERNEL_SHA:?Set SWMMU_KERNEL_SHA to the kernel commit being tested}"
: "${SWMMU_APK_KERNEL_SHA:?Set SWMMU_APK_KERNEL_SHA to the kernel commit in the APK manifest}"
: "${SWMMU_APORTS_SHA:?Set SWMMU_APORTS_SHA to the aports commit in the APK manifest}"
: "${SWMMU_INIT_SCRIPT:?Set SWMMU_INIT_SCRIPT to the nginx guest init script}"
: "${SWMMU_WORK:?Set SWMMU_WORK to a writable workspace directory}"

ROOTFS="$SWMMU_WORK/rootfs"
OUT="$SWMMU_WORK/artifact"
PROFILE="$SWMMU_APK_DIR/nommu-swmmu"
PACKAGES="$PROFILE/packages"
MANIFEST="$SWMMU_APK_DIR/manifest.txt"
rootfs_start_epoch=$(date +%s)

rm -rf "$ROOTFS" "$OUT"
mkdir -p "$SWMMU_WORK/logs" "$OUT/packages" "$OUT/tools" "$OUT/keys" "$ROOTFS"

test -s "$MANIFEST"
manifest_kernel_sha=$(sed -n 's/^kernel_commit=//p' "$MANIFEST")
manifest_aports_sha=$(sed -n 's/^aports_commit=//p' "$MANIFEST")
[ "$manifest_kernel_sha" = "${SWMMU_APK_KERNEL_SHA}" ] || {
    echo "APK kernel SHA mismatch: manifest=$manifest_kernel_sha expected=$SWMMU_APK_KERNEL_SHA" >&2
    exit 1
}
[ "${SWMMU_KERNEL_SHA}" != "${SWMMU_APK_KERNEL_SHA}" ] &&
	echo "Using APKs built for $SWMMU_APK_KERNEL_SHA to test kernel $SWMMU_KERNEL_SHA; workflow verified trap.c-only compatibility."
[ "$manifest_aports_sha" = "$SWMMU_APORTS_SHA" ] || {
	echo "aports SHA mismatch: APKs=$manifest_aports_sha workflow=$SWMMU_APORTS_SHA" >&2
	exit 1
}
grep -q '^profiles=.*nommu-swmmu' "$MANIFEST"
kernel_sha=$SWMMU_KERNEL_SHA
apk_kernel_sha=$manifest_kernel_sha
aports_sha=$manifest_aports_sha

for package in musl-nommu busybox-nommu libcrypto3 libssl3 nginx-nommu coreutils-nommu; do
	set -- "$PACKAGES/$package"-*.apk
	[ "$#" -eq 1 ] && [ -f "$1" ] || {
		echo "expected one APK for $package in $PACKAGES" >&2
		exit 1
	}
	cp "$1" "$OUT/packages/"
done

cp "$PROFILE/tools/busybox-host" "$OUT/tools/busybox-host"
chmod 0755 "$OUT/tools/busybox-host"
cp "$SWMMU_APK_DIR/keys/"*.rsa.pub "$OUT/keys/"
gcc_version=$(cat "$PROFILE/toolchain/gcc-version")
test -n "$gcc_version"

mkdir -p "$SWMMU_WORK/apk-keys"
cp /etc/apk/keys/*.rsa.pub "$SWMMU_WORK/apk-keys/"
cp "$OUT/keys/"*.rsa.pub "$SWMMU_WORK/apk-keys/"
apk --root "$ROOTFS" --initdb --keys-dir "$SWMMU_WORK/apk-keys" \
	--repositories-file /etc/apk/repositories add --no-cache --no-scripts \
	--force-non-repository \
	alpine-baselayout alpine-keys apk-tools ca-certificates-bundle \
	"$OUT/packages/musl-nommu"-*.apk \
	"$OUT/packages/busybox-nommu"-*.apk \
	"$OUT/packages/libcrypto3"-*.apk \
	"$OUT/packages/libssl3"-*.apk \
	"$OUT/packages/nginx-nommu"-*.apk \
	"$OUT/packages/coreutils-nommu"-*.apk \
	2>&1 | tee "$SWMMU_WORK/logs/base-rootfs.log"
mkdir -p "$ROOTFS/etc/apk/keys"
cp /etc/apk/repositories "$ROOTFS/etc/apk/repositories"
cp /etc/apk/keys/*.rsa.pub "$ROOTFS/etc/apk/keys/"
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
apk_kernel_sha=$apk_kernel_sha
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
rootfs_elapsed_seconds=$(($(date +%s) - rootfs_start_epoch))
printf 'rootfs_creation_seconds=%s\n' "$rootfs_elapsed_seconds" | tee -a "$OUT/manifest.txt" "$SWMMU_WORK/logs/rootfs-timing.log"
printf 'Prepared SWMMU nginx rootfs from kernel %s and aports %s (GCC %s) in %s seconds\n' \
	"$kernel_sha" "$aports_sha" "$gcc_version" "$rootfs_elapsed_seconds"
