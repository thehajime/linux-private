#!/bin/sh

NIC=vec0
SERVER=172.17.0.1
TMPDIR_IPERF=/tmp/iperf3-tmp

mount proc /proc -t proc
echo "nameserver 8.8.8.8" > /etc/resolv.conf
/sbin/ifconfig lo 127.0.0.1 up
/sbin/ifconfig "$NIC" 172.17.0.2 up
/sbin/ip route add default via "$SERVER" dev "$NIC"

/sbin/sysctl -w net.ipv4.tcp_wmem="40960 873800 1677721600"
/sbin/sysctl -w net.ipv4.tcp_rmem="40960 873800 1677721600"

echo "=== iperf3 diagnostic: interface and route ==="
/sbin/ip -details address show dev "$NIC" || true
/sbin/ip route get "$SERVER" || true
cat /proc/mounts

# iperf3 backs each stream buffer with a temporary file and MAP_SHARED mmap.
# Use guest tmpfs instead of the hostfs-backed rootfs /tmp used by UML.
mkdir -p "$TMPDIR_IPERF"
if mount -t tmpfs -o size=16m tmpfs "$TMPDIR_IPERF"; then
    export TMPDIR="$TMPDIR_IPERF"
    echo "iperf3 TMPDIR=$TMPDIR (guest tmpfs)"
else
    echo "iperf3 tmpfs mount failed; using default temporary directory" >&2
fi
cat /proc/mounts

IPERF=$(command -v iperf3)
echo "iperf3 binary: $IPERF"
"$IPERF" --version
sleep 5

echo "===iperf3 forward==="
"$IPERF" -4 -c "$SERVER" -fm
IPERF_FORWARD_STATUS=$?
echo "iperf3 measurement status: forward $IPERF_FORWARD_STATUS"

echo "===iperf3 reverse==="
"$IPERF" -4 -c "$SERVER" -R -fm
IPERF_REVERSE_STATUS=$?
echo "iperf3 measurement status: reverse $IPERF_REVERSE_STATUS"

sh /root/netperf-bench.sh

/sbin/halt -f
