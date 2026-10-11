#!/usr/bin/env bash
set -uo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: run-swmmu-nginx-benchmark.sh --kernel FILE --rootfs DIR --workers N \
       --threads N --connections N --duration SEC --output DIR \
       --kernel-sha SHA --aports-sha SHA --gcc-version VERSION
EOF
	exit 2
}

while (($#)); do
	case "$1" in
	--kernel) KERNEL=$2; shift 2;;
	--rootfs) ROOTFS=$2; shift 2;;
	--workers) WORKERS=$2; shift 2;;
	--threads) THREADS=$2; shift 2;;
	--connections) CONNECTIONS=$2; shift 2;;
	--duration) DURATION=$2; shift 2;;
	--output) OUTPUT=$2; shift 2;;
	--kernel-sha) KERNEL_SHA=$2; shift 2;;
	--aports-sha) APORTS_SHA=$2; shift 2;;
	--gcc-version) GCC_VERSION=$2; shift 2;;
	--profile) PROFILE=$2; shift 2;;
	--kernel-args) KERNEL_ARGS_TEXT=$2; shift 2;;
	--needs-rawio) NEEDS_RAWIO=$2; shift 2;;
	*) echo "Unknown argument: $1" >&2; usage;;
	esac
done

: "${KERNEL:?missing --kernel}"
: "${ROOTFS:?missing --rootfs}"
ROOTFS=$(cd -- "$ROOTFS" && pwd -P) || {
	echo "rootfs directory not found" >&2
	exit 1
}
: "${WORKERS:?missing --workers}"
: "${THREADS:?missing --threads}"
: "${CONNECTIONS:?missing --connections}"
: "${DURATION:?missing --duration}"
: "${OUTPUT:?missing --output}"
: "${KERNEL_SHA:?missing --kernel-sha}"
: "${APORTS_SHA:?missing --aports-sha}"
: "${GCC_VERSION:?missing --gcc-version}"
: "${PROFILE:=um-nommu-sas-zpoline}"
: "${KERNEL_ARGS_TEXT:=}"
: "${NEEDS_RAWIO:=false}"
KERNEL_ARGS=()
if [[ -n "$KERNEL_ARGS_TEXT" ]]; then
	read -r -a KERNEL_ARGS <<<"$KERNEL_ARGS_TEXT"
fi

mkdir -p "$OUTPUT"
printf 'RUNNING\n' >"$OUTPUT/harness.status"
printf 'NOT_RUN\n' >"$OUTPUT/wrk-warmup.status"
printf '%s\n' "$WORKERS" >"$OUTPUT/workers.txt"
printf '%s\n' "$THREADS" >"$OUTPUT/threads.txt"
printf '%s\n' "$CONNECTIONS" >"$OUTPUT/connections.txt"
printf '%s\n' "$DURATION" >"$OUTPUT/duration.txt"
printf '%s\n' "$KERNEL_SHA" >"$OUTPUT/kernel-sha.txt"
printf '%s\n' "$APORTS_SHA" >"$OUTPUT/aports-sha.txt"
printf '%s\n' "$GCC_VERSION" >"$OUTPUT/gcc-version.txt"
: >"$OUTPUT/host-runtime-probe.log"
: >"$OUTPUT/guest-runtime-probe.log"

HARNESS_ERROR=
CLIENT=swmmu-wrk-client
UML_PID=
HOST_PROBE_PID=
HARNESS_STATUS=PASS
GUEST_IP=172.18.0.2
EXPECTED_BODY=nginx-matrix-ok
if [[ "$PROFILE" == um-nommu-sas-seccomp || "$PROFILE" == um-nommu-sas-zpoline ]]; then
	EXPECTED_BODY=swmmu-nginx-ok
fi

case "$WORKERS:$THREADS:$CONNECTIONS:$DURATION" in
	1:2:4:*|1:2:16:*|1:2:64:*|4:2:4:*|4:2:16:*|4:2:64:*) ;;
	*) echo "unsupported benchmark parameters" >&2; usage;;
esac

fail() {
	HARNESS_STATUS=FAIL
	if [[ -n "$HARNESS_ERROR" ]]; then
		HARNESS_ERROR+="; "
	fi
	HARNESS_ERROR+="$*"
	printf '%s\n' "$HARNESS_ERROR" >"$OUTPUT/harness.error"
}

capture_logs() {
	if [[ -n "$HOST_PROBE_PID" ]]; then
		kill "$HOST_PROBE_PID" 2>/dev/null || true
		wait "$HOST_PROBE_PID" 2>/dev/null || true
		HOST_PROBE_PID=
	fi
	if [[ -f "$ROOTFS/run/nginx-error.log" ]]; then
		cp "$ROOTFS/run/nginx-error.log" "$OUTPUT/nginx.log"
	else
		: >"$OUTPUT/nginx.log"
	fi
	if [[ -n "$UML_PID" ]]; then
		if [[ -d "$ROOTFS/run" ]]; then
			if [[ -p "$ROOTFS/run/nginx-benchmark-stop" ]]; then
				timeout --foreground 5s bash -c 'printf "stop\\n" > "$1"' _ \
					"$ROOTFS/run/nginx-benchmark-stop" 2>/dev/null || true
				for _ in {1..15}; do
					kill -0 "$UML_PID" 2>/dev/null || break
					sleep 1
				done
			fi
		fi
		if kill -0 "$UML_PID" 2>/dev/null; then
			kill -TERM "$UML_PID" 2>/dev/null || true
			for _ in {1..10}; do
				kill -0 "$UML_PID" 2>/dev/null || break
				sleep 1
			done
		fi
		kill -KILL "$UML_PID" 2>/dev/null || true
		wait "$UML_PID" 2>/dev/null || true
		UML_PID=
	fi
	if [[ -f "$ROOTFS/run/nginx-error.log" ]]; then
		cp "$ROOTFS/run/nginx-error.log" "$OUTPUT/nginx.log"
	fi
	if [[ -f "$ROOTFS/run/nginx-runtime-probe.log" ]]; then
		cp "$ROOTFS/run/nginx-runtime-probe.log" "$OUTPUT/guest-runtime-probe.log"
	fi
	if docker inspect "$CLIENT" >/dev/null 2>&1; then
		docker logs "$CLIENT" >"$OUTPUT/client-container.log" 2>&1 || true
		docker rm -f "$CLIENT" >/dev/null 2>&1 || true
	fi
}

start_runtime_probes() {
	case "$PROFILE" in
		um-nommu-sas-seccomp|um-nommu-sas-zpoline) ;;
		*) return 0 ;;
	esac
	(
		while kill -0 "$UML_PID" 2>/dev/null; do
			{
				date -Ins
				ps -eo pid,ppid,stat,pcpu,time,wchan:32,comm,args |
					awk -v parent="$UML_PID" 'NR == 1 || $1 == parent || $2 == parent'
				for child in $(ps -o pid= --ppid "$UML_PID" 2>/dev/null); do
					ps -L -p "$child" -o pid,tid,stat,pcpu,time,wchan:32,comm 2>/dev/null || true
				done
				ip -s link show dev tap100 2>&1 || true
			} >>"$OUTPUT/host-runtime-probe.log"
			if [[ -p "$ROOTFS/run/nginx-benchmark-stop" ]]; then
				if ! timeout --foreground 5s bash -c \
					'printf "probe %s\\n" "$(date -Ins)" >"$1"' _ \
					"$ROOTFS/run/nginx-benchmark-stop"; then
					printf '%s guest probe FIFO request timed out\n' \
						"$(date -Ins)" >>"$OUTPUT/host-runtime-probe.log"
					break
				fi
			fi
			sleep 5
		done
	) &
	HOST_PROBE_PID=$!
}

finish() {
	capture_logs
	printf '%s\n' "$HARNESS_STATUS" >"$OUTPUT/harness.status"
	if [[ -z "$HARNESS_ERROR" ]]; then
		rm -f "$OUTPUT/harness.error"
	fi
	python3 .github/workflows/parse-swmmu-nginx-measurement.py \
		--output "$OUTPUT" --workers "$WORKERS" --threads "$THREADS" \
		--connections "$CONNECTIONS" --duration "$DURATION" \
		--profile "$PROFILE" \
		--kernel-sha "$KERNEL_SHA" --aports-sha "$APORTS_SHA" \
		--gcc-version "$GCC_VERSION"
}
trap finish EXIT

if [[ ( "$PROFILE" == um-nommu || "$PROFILE" == um-nommu-seccomp ) && "$WORKERS" -gt 1 ]]; then
	HARNESS_STATUS=SKIP
	HARNESS_ERROR="plain NOMMU UML does not support fork(), so nginx worker_processes=$WORKERS is unavailable"
	printf '%s\n' "$HARNESS_ERROR" >"$OUTPUT/harness.error"
	printf 'SKIP\n' >"$OUTPUT/harness.status"
	printf 'not run: plain NOMMU has no fork support\n' >"$OUTPUT/wrk.log"
	printf '125\n' >"$OUTPUT/wrk.exit-code"
	trap - EXIT
	finish
	finish_status=$?
	exit "$finish_status"
fi

if [[ ! -x "$KERNEL" ]]; then
	fail "UML kernel is missing or not executable"
elif [[ "$NEEDS_RAWIO" == true ]]; then
	if ! sudo setcap cap_sys_rawio+ep "$KERNEL" 2>"$OUTPUT/capability.log" || \
	   ! getcap "$KERNEL" | grep -q 'cap_sys_rawio=ep'; then
		fail "could not grant and verify CAP_SYS_RAWIO on UML kernel"
	fi
else
	: >"$OUTPUT/capability.log"
fi

if [[ "$NEEDS_RAWIO" == true ]]; then
	if ! sudo sysctl -w vm.mmap_min_addr=0 >"$OUTPUT/host-sysctl.log" 2>&1; then
		fail "could not set vm.mmap_min_addr for zpoline"
	fi
else
	: >"$OUTPUT/host-sysctl.log"
fi

if ! sudo ip tuntap add dev tap100 mode tap user "$(id -un)" \
	2>"$OUTPUT/tap.log"; then
	fail "could not create tap100"
elif ! sudo ip address add 172.18.0.1/16 dev tap100 >>"$OUTPUT/tap.log" 2>&1 || \
     ! sudo ip link set up dev tap100 >>"$OUTPUT/tap.log" 2>&1; then
	fail "could not configure tap100 for the SWMMU guest"
fi

NGINX_CONF="$ROOTFS/etc/nginx/nginx-matrix.conf"
GUEST_INIT=/sbin/nginx-matrix-init
if [[ "$PROFILE" == um-nommu-sas-seccomp || "$PROFILE" == um-nommu-sas-zpoline ]]; then
	NGINX_CONF="$ROOTFS/etc/nginx/nginx-swmmu.conf"
	GUEST_INIT=/sbin/swmmu-nginx-init
fi
sed -i -E "s/^worker_processes[[:space:]]+[0-9]+;/worker_processes ${WORKERS};/" "$NGINX_CONF"
sed -i -E 's#^error_log[[:space:]].*;#error_log /run/nginx-error.log notice;#' "$NGINX_CONF"
if [[ "$PROFILE" == um-nommu || "$PROFILE" == um-nommu-seccomp ]]; then
	sed -i -e 's/^daemon on;$/daemon off;/' \
		-e 's/^master_process on;$/master_process off;/' "$NGINX_CONF"
fi
cp "$NGINX_CONF" "$OUTPUT/nginx.conf" 2>/dev/null || \
	fail "nginx SWMMU config is missing"
if ! grep -q "^worker_processes ${WORKERS};$" "$OUTPUT/nginx.conf" 2>/dev/null || \
	! grep -q '^error_log /run/nginx-error.log notice;$' "$OUTPUT/nginx.conf" 2>/dev/null; then
	fail "matrix worker count or nginx diagnostic log configuration was not applied"
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	timeout --foreground --signal=TERM --kill-after=10s 600s \
		"$KERNEL" vec0:transport=tap,ifname=tap100,depth=128,gro=1 \
		root=/dev/root "rootflags=$ROOTFS" rootfstype=hostfs rw mem=2048m \
		loglevel=8 console=tty "${KERNEL_ARGS[@]}" "init=$GUEST_INIT" \
		>"$OUTPUT/kernel.log" 2>&1 &
	UML_PID=$!
	ready=0
	ready_timeout=120
	if [[ "$PROFILE" == um-nommu-sas-seccomp ]]; then
		# This profile can take slightly longer to boot when the full matrix is busy.
		ready_timeout=240
	fi
	for ((second = 0; second < ready_timeout; second++)); do
		if grep -Eq 'SWMMU_NGINX_READY|NGINX_MATRIX_READY' "$OUTPUT/kernel.log"; then
			ready=1
			break
		fi
		if ! kill -0 "$UML_PID" 2>/dev/null; then
			break
		fi
		sleep 1
	done
	if [[ "$ready" != 1 ]]; then
		fail "UML guest did not report its nginx ready marker within ${ready_timeout} seconds"
	else
		if [[ -s "$ROOTFS/run/online-cpus.txt" ]]; then
			cp "$ROOTFS/run/online-cpus.txt" "$OUTPUT/online-cpus.txt"
		fi
		if [[ " $KERNEL_ARGS_TEXT " == *" ncpus=4 "* ]]; then
			if [[ "$(tr -d '\n' <"$ROOTFS/run/online-cpus.txt" 2>/dev/null)" != "0-3" ]] && \
			   ! grep -Eq 'smp: Brought up [0-9]+ node, 4 CPUs' "$OUTPUT/kernel.log"; then
				fail "SMP UML did not report all four CPUs online"
			fi
		fi
	fi
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	if ! docker run --detach --name "$CLIENT" --network host ubuntu:22.04 \
		bash -c 'sleep infinity' >"$OUTPUT/client-setup.log" 2>&1; then
		fail "could not start separate wrk client container"
	elif ! timeout 180s docker exec "$CLIENT" bash -ec \
		'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends curl wrk' \
		>>"$OUTPUT/client-setup.log" 2>&1; then
		fail "could not install curl and wrk in the separate client container"
	fi
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	start_runtime_probes
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	if timeout 10s docker exec "$CLIENT" curl --fail --silent --show-error \
		--max-time 5 "http://$GUEST_IP:8080/" >"$OUTPUT/http.body" \
		2>"$OUTPUT/smoke-http.log" && \
		[[ "$(cat "$OUTPUT/http.body")" == "$EXPECTED_BODY" ]]; then
		printf 'PASS\n' >"$OUTPUT/smoke-http.status"
	else
		printf 'FAIL\n' >"$OUTPUT/smoke-http.status"
		fail "HTTP smoke test returned an error or unexpected body"
	fi
	tls_outer_timeout=10
	tls_curl_timeout=5
	if [[ "$PROFILE" == um-nommu-sas-seccomp ]]; then
		tls_outer_timeout=40
		tls_curl_timeout=30
	fi
	if timeout "${tls_outer_timeout}s" docker exec "$CLIENT" curl --fail --silent --show-error --verbose \
		--insecure --max-time "$tls_curl_timeout" "https://$GUEST_IP:8443/" >"$OUTPUT/tls.body" \
		2>"$OUTPUT/smoke-tls.log" && \
		[[ "$(cat "$OUTPUT/tls.body")" == "$EXPECTED_BODY" ]]; then
		printf 'PASS\n' >"$OUTPUT/smoke-tls.status"
	else
		printf 'FAIL\n' >"$OUTPUT/smoke-tls.status"
		fail "TLS smoke test returned an error or unexpected body"
	fi
else
	printf 'FAIL\n' >"$OUTPUT/smoke-http.status"
	printf 'FAIL\n' >"$OUTPUT/smoke-tls.status"
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	warmup_rc=0
	timeout 40s docker exec "$CLIENT" wrk -t"$THREADS" -c"$CONNECTIONS" \
		-T 30s -d5s "http://$GUEST_IP:8080/" >"$OUTPUT/wrk-warmup.log" 2>&1 || warmup_rc=$?
	if [[ "$warmup_rc" -ne 0 ]]; then
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		fail "wrk warmup failed or timed out with exit status $warmup_rc"
	elif grep -Eq 'Socket errors:.*(connect|read|write|timeout) [1-9][0-9]*' \
		"$OUTPUT/wrk-warmup.log"; then
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		fail "wrk warmup reported socket errors"
	elif ! awk '/Requests\/sec:/ { if ($2 > 0) found = 1 } END { exit !found }' \
		"$OUTPUT/wrk-warmup.log"; then
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		fail "wrk warmup completed without serving any requests"
	else
		printf 'PASS\n' >"$OUTPUT/wrk-warmup.status"
	fi
fi
if [[ "$HARNESS_STATUS" == PASS ]]; then
	wrk_rc=0
	timeout --foreground --signal=TERM --kill-after=5s "$((DURATION + 45))s" \
		docker exec "$CLIENT" wrk -t"$THREADS" -c"$CONNECTIONS" \
		-T 30s -d"${DURATION}s" --latency "http://$GUEST_IP:8080/" \
		>"$OUTPUT/wrk.log" 2>&1 || wrk_rc=$?
	printf '%s\n' "$wrk_rc" >"$OUTPUT/wrk.exit-code"
	if [[ "$wrk_rc" -ne 0 ]]; then
		fail "wrk failed or timed out with exit status $wrk_rc"
	fi
else
	printf '%s\n' "not run: harness setup failed" >"$OUTPUT/wrk.log"
	printf '125\n' >"$OUTPUT/wrk.exit-code"
fi

if [[ -e "$ROOTFS/run/nginx-probe-start" ]]; then
	: >"$ROOTFS/run/nginx-probe-stop"
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	printf 'PASS\n' >"$OUTPUT/harness.status"
fi

trap - EXIT
finish
finish_status=$?
[[ ( "$HARNESS_STATUS" == PASS || "$HARNESS_STATUS" == SKIP ) && "$finish_status" -eq 0 ]]
