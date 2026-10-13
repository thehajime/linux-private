#!/bin/sh

NIC=vec0
SERVER=172.17.0.1
TMPDIR_IPERF=/tmp/iperf3-tmp

mount proc /proc -t proc
echo "nameserver 8.8.8.8" > /etc/resolv.conf
/sbin/ifconfig lo 127.0.0.1 up
/sbin/ifconfig "$NIC" 172.17.0.2 up
/sbin/ip route add default via "$SERVER" dev "$NIC"

DEFAULT_TCP_WMEM=$(cat /proc/sys/net/ipv4/tcp_wmem)
DEFAULT_TCP_RMEM=$(cat /proc/sys/net/ipv4/tcp_rmem)
if grep -Eq '(^|[[:space:]])netperf_only=1([[:space:]]|$)' /proc/cmdline; then
    NETPERF_ONLY=1
else
    NETPERF_ONLY=0
    # Keep the existing enlarged buffers for iperf3. Netperf runs with the
    # guest defaults because the enlarged values caused severe local delays.
    /sbin/sysctl -w net.ipv4.tcp_wmem="40960 873800 1677721600"
    /sbin/sysctl -w net.ipv4.tcp_rmem="40960 873800 1677721600"
fi

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

if [ "$NETPERF_ONLY" -eq 1 ]; then
    echo "netperf-only diagnostic mode enabled"
    sh /root/netperf-bench.sh
    status=$?
    /sbin/halt -f
    exit "$status"
fi

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

# Restore the values present at guest boot before starting netperf.
/sbin/sysctl -w "net.ipv4.tcp_wmem=$DEFAULT_TCP_WMEM"
/sbin/sysctl -w "net.ipv4.tcp_rmem=$DEFAULT_TCP_RMEM"

sh /root/netperf-bench.sh

/sbin/halt -f
