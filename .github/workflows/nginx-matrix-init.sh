#!/bin/sh
set -e
export PATH=/sbin:/usr/sbin:/bin:/usr/bin

busybox mount -t proc proc /proc
busybox mount -t sysfs sysfs /sys || true
busybox mkdir -p /run
busybox rm -f /run/nginx-benchmark-stop
if busybox grep -q '^master_process on;$' /etc/nginx/nginx-matrix.conf; then
	busybox mkfifo /run/nginx-benchmark-stop
fi
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /tmp
busybox mkdir -p /dev/shm /var/www/localhost/htdocs /var/lib/nginx/tmp
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /dev/shm
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /var/lib/nginx/tmp

busybox ifconfig lo up
busybox ifconfig vec0 172.18.0.2 netmask 255.255.0.0 up
busybox route add default gw 172.18.0.1 dev vec0

nginx -t -c /etc/nginx/nginx-matrix.conf
if busybox grep -q '^master_process off;$' /etc/nginx/nginx-matrix.conf; then
	if [ -r /sys/devices/system/cpu/online ]; then
		busybox cat /sys/devices/system/cpu/online >/run/online-cpus.txt
	else
		echo unavailable >/run/online-cpus.txt
	fi
	echo NGINX_MATRIX_READY
	exec nginx -c /etc/nginx/nginx-matrix.conf
fi
nginx -c /etc/nginx/nginx-matrix.conf
if [ -r /sys/devices/system/cpu/online ]; then
	busybox cat /sys/devices/system/cpu/online >/run/online-cpus.txt
else
	echo unavailable >/run/online-cpus.txt
fi
echo NGINX_MATRIX_READY

IFS= read -r _ < /run/nginx-benchmark-stop
nginx -s quit -c /etc/nginx/nginx-matrix.conf || true
busybox poweroff -f
