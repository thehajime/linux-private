#!/bin/sh
set -eu

apk add --no-cache alpine-sdk doas git

addgroup -S abuild 2>/dev/null || true
adduser -D -G abuild builder
mkdir -p /etc/doas.d /var/cache/distfiles /out/packages /out/keys
printf 'permit nopass builder as root\n' >/etc/doas.d/abuild.conf
chmod 0400 /etc/doas.d/abuild.conf
mkdir -p /home/builder/aports/main /home/builder/aports/scripts
cp -a /src/main/musl /src/main/busybox /home/builder/aports/main/
cp -a /src/scripts/swmmu-profile.sh /home/builder/aports/scripts/
chown -R builder:abuild /home/builder /out /var/cache/distfiles

su -s /bin/sh builder -c '
set -eu
export PACKAGER="UML CI <uml-ci@example.invalid>"
export REPODEST="$HOME/packages"
abuild-keygen -a -i -n
cd "$HOME/aports/main/musl"
abuild -r
cd "$HOME/aports/main/busybox"
abuild -r

for package in busybox-nommu musl-nommu; do
    set -- "$REPODEST/main/x86_64/$package"-*.apk
    [ "$#" -eq 1 ] && [ -f "$1" ] || {
        echo "expected one built APK for $package" >&2
        exit 1
    }
    cp "$1" /out/packages/
done

set -- "$HOME/.abuild/"*.rsa.pub
[ "$#" -eq 1 ] && [ -f "$1" ]
cp "$1" /out/keys/
'
