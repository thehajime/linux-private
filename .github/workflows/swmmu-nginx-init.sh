#!/bin/sh
set -e
export PATH=/sbin:/usr/sbin:/bin:/usr/bin

mount -t proc proc /proc
mount -t sysfs sysfs /sys || true
mkdir -p /dev/shm /var/www/localhost/htdocs /var/lib/nginx/tmp
mount -t tmpfs -o rw,nosuid,nodev tmpfs /dev/shm

ip link set lo up
ip link set vec0 up
ip address add 172.18.0.2/16 dev vec0
ip route add default via 172.18.0.1 dev vec0

nginx -t -c /etc/nginx/nginx-swmmu.conf
nginx -c /etc/nginx/nginx-swmmu.conf
echo SWMMU_NGINX_READY

while :; do
	sleep 3600
done
