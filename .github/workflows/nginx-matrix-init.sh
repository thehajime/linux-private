#!/bin/sh
set -ex
export PATH=/sbin:/usr/sbin:/bin:/usr/bin

report_exit()
{
	status=$?
	echo "NGINX_MATRIX_EXIT status=$status"
}
trap report_exit EXIT

busybox mount -t proc proc /proc
busybox mount -t sysfs sysfs /sys || true
busybox mkdir -p /run
busybox rm -f /run/nginx-benchmark-stop
busybox mkfifo /run/nginx-benchmark-stop
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /tmp
busybox mkdir -p /dev/shm /var/www/localhost/htdocs /var/lib/nginx/tmp
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /dev/shm
busybox mount -t tmpfs -o rw,nosuid,nodev tmpfs /var/lib/nginx/tmp

busybox ifconfig lo up
busybox ifconfig vec0 172.18.0.2 netmask 255.255.0.0 up
busybox route add default gw 172.18.0.1 dev vec0

nginx -t -c /etc/nginx/nginx-matrix.conf
if busybox grep -q '^master_process off;$' /etc/nginx/nginx-matrix.conf; then
	workers=$(busybox sed -n 's/^worker_processes \([0-9][0-9]*\);$/\1/p' \
		/etc/nginx/nginx-matrix.conf)
	case "$workers" in
		1|4|8|16) ;;
		*) echo "invalid worker_processes: $workers"; busybox poweroff -f ;;
	esac
	pids=
	i=1
	while [ "$i" -le "$workers" ]; do
		conf="/run/nginx-matrix-$i.conf"
		busybox sed \
			-e 's/^worker_processes [0-9][0-9]*;$/worker_processes 1;/' \
			-e "s#^pid .*;#pid /run/nginx-matrix-$i.pid;#" \
			-e 's/listen 0.0.0.0:8080;/listen 0.0.0.0:8080 reuseport;/' \
			-e 's/listen 0.0.0.0:8443 ssl;/listen 0.0.0.0:8443 ssl reuseport;/' \
			/etc/nginx/nginx-matrix.conf >"$conf"
		busybox grep -q 'listen 0.0.0.0:8080 reuseport;' "$conf"
		busybox grep -q 'listen 0.0.0.0:8443 ssl reuseport;' "$conf"
		nginx -t -c "$conf"
		nginx -c "$conf" &
		pids="$pids $!"
		i=$((i + 1))
	done
	actual=0
	tries=0
	while [ "$tries" -lt 20 ]; do
		actual=0
		for proc in /proc/[0-9]*; do
			[ -r "$proc/comm" ] || continue
			if [ "$(busybox cat "$proc/comm")" = nginx ]; then
				actual=$((actual + 1))
			fi
		done
		if [ "$actual" -ge "$workers" ]; then
			break
		fi
		tries=$((tries + 1))
		busybox sleep 1
	done
	echo "$actual" >/run/nginx-worker-count.txt
	if [ -r /sys/devices/system/cpu/online ]; then
		busybox cat /sys/devices/system/cpu/online >/run/online-cpus.txt
	else
		echo unavailable >/run/online-cpus.txt
	fi
	echo NGINX_MATRIX_READY
	IFS= read -r _ < /run/nginx-benchmark-stop
	for pid in $pids; do busybox kill -TERM "$pid" 2>/dev/null || true; done
	for pid in $pids; do wait "$pid" 2>/dev/null || true; done
	busybox poweroff -f
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
