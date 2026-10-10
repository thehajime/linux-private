#!/bin/sh
set -e
export PATH=/sbin:/usr/sbin:/bin:/usr/bin

busybox mount -t proc proc /proc
busybox mount -t sysfs sysfs /sys || true
busybox mkdir -p /dev/shm /var/www/localhost/htdocs /var/lib/nginx/tmp
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /dev/shm
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /var/lib/nginx/tmp

busybox ifconfig lo up
busybox ifconfig vec0 172.18.0.2 netmask 255.255.0.0 up
busybox route add default gw 172.18.0.1 dev vec0

nginx -t -c /etc/nginx/nginx-matrix.conf
nginx -c /etc/nginx/nginx-matrix.conf
cat /sys/devices/system/cpu/online >/tmp/online-cpus.txt
echo NGINX_MATRIX_READY

while [ ! -e /run/nginx-benchmark-stop ]; do
	sleep 1
done
nginx -s quit -c /etc/nginx/nginx-matrix.conf || true
sleep 1
busybox poweroff -f
