#!/bin/sh
set -eu
trap 'status=$?; if [ "$status" -ne 0 ]; then echo "build-nommu-apks.sh failed (exit $status)" >&2; fi' EXIT

apk add --no-cache alpine-sdk bash doas git bc bison flex openssl-dev perl python3 elfutils-dev gmp-dev mpfr-dev mpc1-dev isl-dev rsync

# Build the plugin with the same Alpine GCC that will compile the guest
# userspace packages. GCC plugins are tied to the host compiler ABI.
make -C /kernel ARCH=um O=/tmp/kernel-build x86_64_defconfig
/kernel/scripts/config --file /tmp/kernel-build/.config \
	--disable MMU \
	--enable NOMMU_SWMMU \
	--enable GCC_PLUGINS \
	--enable GCC_PLUGIN_SWMMU
make -C /kernel ARCH=um O=/tmp/kernel-build olddefconfig
make -C /kernel ARCH=um O=/tmp/kernel-build scripts
make -C /kernel ARCH=um O=/tmp/kernel-build \
	INSTALL_HDR_PATH=/tmp/kernel-uapi headers_install
test -r /tmp/kernel-build/scripts/gcc-plugins/swmmu_plugin.so
test -r /tmp/kernel-uapi/include/linux/nommu_swmmu.h

addgroup -S abuild 2>/dev/null || true
adduser -D -G abuild builder
mkdir -p /etc/doas.d /var/cache/distfiles /out/keys
printf 'permit nopass builder as root\n' >/etc/doas.d/abuild.conf
chmod 0400 /etc/doas.d/abuild.conf

for profile in nommu-plain nommu-swmmu; do
	mkdir -p "/home/builder/aports-$profile/main" "/home/builder/aports-$profile/scripts" "/out/$profile/packages" "/out/$profile/tools" "/out/$profile/toolchain"
	cp -a /src/main/musl /src/main/busybox "/home/builder/aports-$profile/main/"
	cp -a /src/scripts/swmmu-profile.sh /src/scripts/swmmu-cc "/home/builder/aports-$profile/scripts/"
	if [ "$profile" = nommu-swmmu ]; then
		cp -a /src/main/openssl /src/main/zlib /src/main/apk-tools \
			/src/main/python3 /src/main/nginx /src/main/coreutils \
			"/home/builder/aports-$profile/main/"
		cp -a /src/scripts/swmmu-prepare.sh "/home/builder/aports-$profile/scripts/"
		# Avoid enabling target extensions against host-only libraries.
		python3 - "/home/builder/aports-$profile/main/python3/APKBUILD" <<'PYAPK'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = '\tmakedepends="bash python3-dev openssl-dev"'
new = '\tmakedepends="$makedepends bash python3-dev openssl-dev"'
old_subpackages = '\tarch="x86_64"\n\tsubpackages="$pkgname-nommu::x86_64"'
new_subpackages = old_subpackages + '\n\toptions="$options !tracedeps"'
old_configure = '\t\tac_cv_file__dev_ptmx=yes \\\n'
new_configure = '\t\tPKG_CONFIG=false \\\n\t\tac_cv_file__dev_ptmx=yes \\\n'
if (text.count(old) != 1 or text.count(old_subpackages) != 1
        or text.count(old_configure) != 1):
    raise SystemExit(f"expected one SWMMU_ONLY dependency/configure block in {path}")
text = (text.replace(old, new)
        .replace(old_subpackages, new_subpackages)
        .replace(old_configure, new_configure))
path.write_text(text)
PYAPK
	fi
done
chown -R builder:abuild /home/builder /out /var/cache/distfiles

su -s /bin/sh builder -c '
set -eu
export PACKAGER="UML CI <uml-ci@example.invalid>"
abuild-keygen -a -i -n

build_profile() {
	profile=$1
	export REPODEST="/out/$profile"
	${CC:-gcc} -dumpfullversion >"/out/$profile/toolchain/gcc-version"
	if [ "$profile" = nommu-swmmu ]; then
		export SWMMU=1
		export SWMMU_KERNEL_SRC=/kernel
		export SWMMU_KERNEL_BUILD=/tmp/kernel-build
		export SWMMU_PLUGIN=/tmp/kernel-build/scripts/gcc-plugins/swmmu_plugin.so
		export SWMMU_UAPI=/tmp/kernel-uapi/include
		export SWMMU_SYSROOT="$HOME/swmmu-sysroot"
	else
		export SWMMU=0
		unset SWMMU_PLUGIN SWMMU_UAPI SWMMU_SYSROOT SWMMU_WRAPPER_SOURCE
	fi
	cd "$HOME/aports-$profile/main/musl"
	abuild -r
	cd "$HOME/aports-$profile/main/busybox"
	abuild -r
	if [ "$profile" = nommu-swmmu ]; then
		"$HOME/aports-$profile/scripts/swmmu-prepare.sh" check
		cd "$HOME/aports-$profile/main/openssl"
		SWMMU_ONLY=1 abuild -r
		cd "$HOME/aports-$profile/main/zlib"
		SWMMU_ONLY=1 abuild -r
		cd "$HOME/aports-$profile/main/apk-tools"
		SWMMU_ONLY=1 abuild -r
		cd "$HOME/aports-$profile/main/coreutils"
		SWMMU_ONLY=1 abuild -r
		cd "$HOME/aports-$profile/main/nginx"
		SWMMU_ONLY=1 abuild -r
		cd "$HOME/aports-$profile/main/python3"
		SWMMU_ONLY=1 abuild -r
	fi
	for package in busybox-nommu musl-nommu; do
		set -- "$REPODEST/main/x86_64/$package"-*.apk
		[ "$#" -eq 1 ] && [ -f "$1" ] || {
			echo "expected one built APK for $profile/$package; matches: $*" >&2
			exit 1
		}
		echo "Collecting $profile/$package: $1"
		cp "$1" "/out/$profile/packages/"
	done
	if [ "$profile" = nommu-swmmu ]; then
		for package in libcrypto3 libssl3 zlib-nommu apk-tools-nommu \
			coreutils-nommu nginx-nommu python3-nommu; do
			set -- "$REPODEST/main/x86_64/$package"-*.apk
			[ "$#" -eq 1 ] && [ -f "$1" ] || {
				echo "expected one built SWMMU APK for $package; matches: $*" >&2
				exit 1
			}
			echo "Collecting $profile/$package: $1"
			cp "$1" "/out/$profile/packages/"
		done
	fi
	set -- "$REPODEST/main/x86_64/busybox-static"-*.apk
	[ "$#" -eq 1 ] && [ -f "$1" ] || {
		echo "expected one static host BusyBox APK for $profile; matches: $*" >&2
		exit 1
	}
	echo "Extracting host BusyBox from: $1"
	if ! tar -xOf "$1" bin/busybox.static >"/out/$profile/tools/busybox-host"; then
		echo "could not extract bin/busybox.static from $1; APK entries follow:" >&2
		tar -tvf "$1" >&2 || true
		exit 1
	fi
	chmod 755 "/out/$profile/tools/busybox-host"
}

build_profile nommu-plain
build_profile nommu-swmmu

set -- "$HOME/.config/abuild/"*.rsa.pub
[ "$#" -eq 1 ] && [ -f "$1" ] || {
	echo "expected one abuild public key; matches: $*" >&2
	ls -la "$HOME/.config/abuild" >&2 || true
	exit 1
}
echo "Collecting abuild public key: $1"
cp "$1" /out/keys/
'
