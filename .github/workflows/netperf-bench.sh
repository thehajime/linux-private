#!/bin/sh

PSIZES="64 128 256 512 1024 1500 65507"
TESTNAMES="TCP_STREAM TCP_MAERTS"
DEST_ADDR=${1:-"172.17.0.1"}
NETPERF=${2:-/root/netperf}
FAILED=0

run_case() {
    label=$1
    shift
    start=$(date +%s)
    echo "netperf case start: $label epoch=$start"
    "$@"
    status=$?
    end=$(date +%s)
    echo "netperf case end: $label status=$status elapsed_seconds=$((end - start)) epoch=$end"
    if [ "$status" -ne 0 ]; then
        FAILED=1
    fi
}

/sbin/sysctl -w net.ipv4.tcp_wmem="40960 873800 1677721600"
/sbin/sysctl -w net.ipv4.tcp_rmem="40960 873800 1677721600"

for size in $PSIZES
do
    for test in $TESTNAMES
    do
	echo "== netperf ($size-$test)  =="
	run_case "$size-$test" "$NETPERF" -H "$DEST_ADDR" -l 10 -t "$test" -- -o THROUGHPUT,THROUGHPUT_UNITS,LOCAL_SEND_SIZE,COMMAND_LINE -m "$size,$size"

    done
done

run_case TCP_RR "$NETPERF" -H "$DEST_ADDR" -l 10 -t TCP_RR -- -o THROUGHPUT,THROUGHPUT_UNITS,LOCAL_SEND_SIZE,COMMAND_LINE

exit "$FAILED"
