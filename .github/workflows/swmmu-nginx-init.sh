#!/bin/sh
set -e
export PATH=/sbin:/usr/sbin:/bin:/usr/bin

busybox mount -t proc proc /proc
busybox mount -t sysfs sysfs /sys || true
busybox mkdir -p /run
busybox rm -f /run/nginx-benchmark-stop
busybox rm -f /run/nginx-runtime-probe.log
control_channel=${NGINX_CONTROL_CHANNEL:-legacy}
if [ "$control_channel" = legacy ]; then
	busybox mkfifo /run/nginx-benchmark-stop
fi
busybox mkdir -p /dev/shm /var/www/localhost/htdocs /var/lib/nginx/tmp /var/lib/nginx/logs
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /dev/shm
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /var/lib/nginx/tmp

busybox ifconfig lo up
busybox ifconfig vec0 172.18.0.2 netmask 255.255.0.0 up
busybox route add default gw 172.18.0.1 dev vec0

nginx -t -c /etc/nginx/nginx-swmmu.conf
nginx -c /etc/nginx/nginx-swmmu.conf
if [ "$control_channel" = serial ]; then
	exec 3<>/dev/ttyS0
	busybox stty -echo <&3
fi
echo SWMMU_NGINX_READY
if [ "$control_channel" = legacy ]; then
	exec 3</run/nginx-benchmark-stop
fi

while IFS= read -r command <&3; do
	case "$command" in
	probe*)
		printf 'SWMMU_NGINX_GUEST_PROBE %s\n' "$command" >/dev/console 2>/dev/null || true
		printf '%s\n' "$command" >> /run/nginx-runtime-probe.log
		busybox ps -T -o pid,ppid,stat,time,comm,args >> /run/nginx-runtime-probe.log 2>&1
		busybox cat /proc/net/dev >> /run/nginx-runtime-probe.log 2>&1
		;;
	stop)
		nginx -s quit -c /etc/nginx/nginx-swmmu.conf || true
		busybox poweroff -f
		break
		;;
	esac
done
