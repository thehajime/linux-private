#!/bin/busybox sh

SERVER='@SERVER_IP@'
REPETITIONS='@REPETITIONS@'
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

/bin/busybox mount -t proc proc /proc 2>/dev/null || :
/bin/busybox ip link set lo up
/bin/busybox ip link set vec0 up
/bin/busybox ip addr add 172.17.0.2/16 dev vec0
/bin/busybox ip route add default via "$SERVER" dev vec0
echo "TASK_B_NETPERF_BEGIN mode=$(cat /proc/cmdline) server=$SERVER repetitions=$REPETITIONS epoch=$(/bin/busybox date +%s)"
/bin/busybox ip -details addr show dev vec0
/bin/busybox ip route
/bin/busybox ip neigh show dev vec0
/bin/busybox ping -c 1 -W 2 "$SERVER"
echo "PREFLIGHT_PING_STATUS=$?"
/root/netperf -V
failed=0

for duration in 1 10; do
  rep=1
  while [ "$rep" -le "$REPETITIONS" ]; do
    for test in TCP_STREAM TCP_MAERTS; do
      start=$(/bin/busybox date +%s)
      echo "CASE_BEGIN duration=$duration rep=$rep test=$test size=64 epoch=$start"
      /root/netperf -H "$SERVER" -p 12865 -l "$duration" -t "$test" \
        -- -o ELAPSED_TIME,THROUGHPUT,THROUGHPUT_UNITS,LOCAL_SEND_SIZE,LOCAL_RECV_SIZE,LOCAL_RECV_WIDTH,REMOTE_SEND_SIZE,REMOTE_RECV_SIZE,COMMAND_LINE \
        -m 64,64
      rc=$?
      end=$(/bin/busybox date +%s)
      echo "CASE_END duration=$duration rep=$rep test=$test status=$rc wall_seconds=$((end-start)) epoch=$end"
      [ "$rc" -eq 0 ] || failed=1
    done
    rep=$((rep + 1))
  done
done

echo "TASK_B_NETPERF_END status=$failed epoch=$(/bin/busybox date +%s)"
/bin/busybox halt -f
exit "$failed"
