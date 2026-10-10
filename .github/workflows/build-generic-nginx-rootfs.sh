#!/usr/bin/env bash
set -euo pipefail

: "${NGINX_PROFILE:?set NGINX_PROFILE to mmu or nommu}"
: "${NGINX_APK_DIR:?set NGINX_APK_DIR to the downloaded signed NOMMU APK artifact}"
: "${NGINX_WORK:?set NGINX_WORK to an output directory}"
: "${NGINX_INIT_SCRIPT:?set NGINX_INIT_SCRIPT to the guest init script}"
: "${NGINX_KERNEL_SHA:?set NGINX_KERNEL_SHA to the tested kernel SHA}"
: "${NGINX_APORTS_SHA:?set NGINX_APORTS_SHA to the APK source SHA}"
NGINX_GCC_VERSION=${NGINX_GCC_VERSION:-$(cat "$NGINX_APK_DIR/nommu-swmmu/toolchain/gcc-version")}
: "${NGINX_GCC_VERSION:?APK toolchain version is missing}"

rootfs="$NGINX_WORK/rootfs"
artifact="$NGINX_WORK/artifact"
keys="$NGINX_WORK/keys"
rm -rf "$rootfs" "$artifact" "$keys"
mkdir -p "$rootfs" "$artifact" "$keys"
cp /etc/apk/keys/*.rsa.pub "$keys/"
cp "$NGINX_APK_DIR/keys/"*.rsa.pub "$keys/"

if [[ "$NGINX_PROFILE" == nommu ]]; then
	profile="$NGINX_APK_DIR/nommu-plain"
	packages="$profile/packages"
	apk --root "$rootfs" --initdb --keys-dir "$keys" \
		--repositories-file /etc/apk/repositories add --no-cache --no-scripts \
		--force-non-repository --force-overwrite \
		alpine-baselayout alpine-keys apk-tools ca-certificates-bundle nginx openssl \
		"$packages/musl-nommu"-*.apk "$packages/busybox-nommu"-*.apk \
		2>&1 | tee "$NGINX_WORK/apk-install.log"
	for package in musl-nommu busybox-nommu; do
		cp "$packages/$package"-*.apk "$artifact/"
	done
else
	[[ "$NGINX_PROFILE" == mmu ]] || { echo "unknown profile: $NGINX_PROFILE" >&2; exit 2; }
	apk --root "$rootfs" --initdb --keys-dir "$keys" \
		--repositories-file /etc/apk/repositories add --no-cache --no-scripts \
		alpine-baselayout alpine-keys apk-tools busybox ca-certificates-bundle nginx openssl \
		2>&1 | tee "$NGINX_WORK/apk-install.log"
fi

mkdir -p "$rootfs/dev/shm" "$rootfs/proc" "$rootfs/sys" "$rootfs/run" \
	"$rootfs/tmp" "$rootfs/var/www/localhost/htdocs" "$rootfs/var/lib/nginx/tmp"
chmod 1777 "$rootfs/tmp"
printf 'nginx-matrix-ok\n' >"$rootfs/var/www/localhost/htdocs/index.html"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
	-subj '/CN=nginx-matrix.local' \
	-keyout "$rootfs/etc/nginx/matrix.key" \
	-out "$rootfs/etc/nginx/matrix.crt" \
	2>"$NGINX_WORK/certificate.log"
chmod 0600 "$rootfs/etc/nginx/matrix.key"
cat >"$rootfs/etc/nginx/nginx-matrix.conf" <<'EOF'
worker_processes 1;
user root;
daemon on;
master_process on;
pid /tmp/nginx-matrix.pid;
error_log /tmp/nginx-error.log notice;
events { worker_connections 256; }
http {
	access_log off;
	client_body_temp_path /tmp/nginx-client-body;
	proxy_temp_path /tmp/nginx-proxy;
	fastcgi_temp_path /tmp/nginx-fastcgi;
	uwsgi_temp_path /tmp/nginx-uwsgi;
	scgi_temp_path /tmp/nginx-scgi;
	server {
		listen 0.0.0.0:8080;
		root /var/www/localhost/htdocs;
		location / { try_files $uri $uri/ =404; }
	}
	server {
		listen 0.0.0.0:8443 ssl;
		ssl_certificate /etc/nginx/matrix.crt;
		ssl_certificate_key /etc/nginx/matrix.key;
		ssl_protocols TLSv1.2 TLSv1.3;
		root /var/www/localhost/htdocs;
		location / { try_files $uri $uri/ =404; }
	}
}
EOF
install -m 0755 "$NGINX_INIT_SCRIPT" "$rootfs/sbin/nginx-matrix-init"
cp -a "$NGINX_WORK/apk-install.log" "$NGINX_WORK/certificate.log" "$artifact/"
printf 'profile=%s\nkernel_sha=%s\naports_sha=%s\ngcc_version=%s\n' \
	"$NGINX_PROFILE" "$NGINX_KERNEL_SHA" "$NGINX_APORTS_SHA" \
	"$NGINX_GCC_VERSION" >"$artifact/manifest.txt"
for package in "$artifact/"*.apk; do
	[[ -f "$package" ]] && sha256sum "$package" >>"$artifact/manifest.txt"
done
tar --numeric-owner --owner=0 --group=0 -czf "$artifact/rootfs.tar.gz" -C "$rootfs" .
printf 'Built %s generic nginx rootfs at %s\n' "$NGINX_PROFILE" "$artifact/rootfs.tar.gz"
