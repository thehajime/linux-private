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
    output_file="/tmp/netperf-case-${label}-$$.log"
    rm -f "$output_file"
    "$@" >"$output_file" 2>&1 &
    test_pid=$!
    sample=0
    while kill -0 "$test_pid" 2>/dev/null; do
        sleep 5
        kill -0 "$test_pid" 2>/dev/null || break
        now=$(date +%s)
        if [ -r "/proc/$test_pid/stat" ]; then
            set -- $(awk '{print $3, $14, $15}' "/proc/$test_pid/stat")
            state=$1 user_ticks=$2 system_ticks=$3
        else
            state=exited user_ticks=0 system_ticks=0
        fi
        [ "$state" = Z ] && break
        wchan=$(cat "/proc/$test_pid/wchan" 2>/dev/null || echo unavailable)
        counters=$(awk '/vec0:/ {printf "rx_bytes=%s rx_packets=%s rx_errors=%s rx_dropped=%s tx_bytes=%s tx_packets=%s tx_errors=%s tx_dropped=%s", $2, $3, $4, $5, $10, $11, $12, $13}' /proc/net/dev)
        retransmits=$(awk '
            $1 == "Tcp:" && !header { for (i = 2; i <= NF; i++) names[i] = $i; header = 1; next }
            $1 == "Tcp:" && header { for (i = 2; i <= NF; i++) if (names[i] == "RetransSegs") { print $i; exit } }
        ' /proc/net/snmp)
        echo "netperf probe: $label sample=$sample epoch=$now pid=$test_pid state=$state user_ticks=$user_ticks system_ticks=$system_ticks wchan=$wchan tcp_retrans_segs=${retransmits:-unavailable} ${counters:-vec0=unavailable}"
        sample=$((sample + 1))
    done
    wait "$test_pid"
    status=$?
    command_end=$(date +%s)
    command_elapsed=$((command_end - start))
    echo "netperf command end: $label status=$status measurement_elapsed_seconds=$command_elapsed epoch=$command_end"
    cat "$output_file"
    if grep -Eiq 'recv_response:|send_request:|connection (reset|refused)|no route to host|timed out|unable to connect' "$output_file"; then
        echo "netperf case output contains a connection/protocol error: $label"
        status=1
    elif ! grep -Eq '^[[:space:]]*[0-9]+([.][0-9]+)?,' "$output_file"; then
        echo "netperf case produced no machine-readable measurement row: $label"
        status=1
    fi
    rm -f "$output_file"
    end=$(date +%s)
    echo "netperf case end: $label status=$status measurement_elapsed_seconds=$command_elapsed wrapper_elapsed_seconds=$((end - command_end)) elapsed_seconds=$((end - start)) epoch=$end"
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
