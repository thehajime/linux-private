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
printf '%s\n' "$WORKERS" >"$OUTPUT/workers.txt"
printf '%s\n' "$THREADS" >"$OUTPUT/threads.txt"
printf '%s\n' "$CONNECTIONS" >"$OUTPUT/connections.txt"
printf '%s\n' "$DURATION" >"$OUTPUT/duration.txt"
printf '%s\n' "$KERNEL_SHA" >"$OUTPUT/kernel-sha.txt"
printf '%s\n' "$APORTS_SHA" >"$OUTPUT/aports-sha.txt"
printf '%s\n' "$GCC_VERSION" >"$OUTPUT/gcc-version.txt"

HARNESS_ERROR=
CLIENT=swmmu-wrk-client
UML_PID=
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
	if [[ -f "$ROOTFS/tmp/nginx-error.log" ]]; then
		cp "$ROOTFS/tmp/nginx-error.log" "$OUTPUT/nginx.log"
	else
		: >"$OUTPUT/nginx.log"
	fi
	if [[ -n "$UML_PID" ]]; then
		if [[ -d "$ROOTFS/run" ]]; then
			touch "$ROOTFS/run/nginx-benchmark-stop"
			for _ in {1..15}; do
				kill -0 "$UML_PID" 2>/dev/null || break
				sleep 1
			done
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
	if [[ -f "$ROOTFS/tmp/nginx-error.log" ]]; then
		cp "$ROOTFS/tmp/nginx-error.log" "$OUTPUT/nginx.log"
	fi
	if docker inspect "$CLIENT" >/dev/null 2>&1; then
		docker logs "$CLIENT" >"$OUTPUT/client-container.log" 2>&1 || true
		docker rm -f "$CLIENT" >/dev/null 2>&1 || true
	fi
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
sed -i 's#error_log stderr notice;#error_log /tmp/nginx-error.log notice;#' "$NGINX_CONF"
cp "$NGINX_CONF" "$OUTPUT/nginx.conf" 2>/dev/null || \
	fail "nginx SWMMU config is missing"
if ! grep -q "^worker_processes ${WORKERS};$" "$OUTPUT/nginx.conf" 2>/dev/null || \
	! grep -q '^error_log /tmp/nginx-error.log notice;$' "$OUTPUT/nginx.conf" 2>/dev/null; then
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
	for _ in {1..120}; do
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
		fail "UML guest did not report its nginx ready marker within 120 seconds"
	else
		if [[ -s "$ROOTFS/tmp/online-cpus.txt" ]]; then
			cp "$ROOTFS/tmp/online-cpus.txt" "$OUTPUT/online-cpus.txt"
		fi
		if [[ " $KERNEL_ARGS_TEXT " == *" ncpus=4 "* ]]; then
			if [[ "$(tr -d '\n' <"$ROOTFS/tmp/online-cpus.txt" 2>/dev/null)" != "0-3" ]]; then
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
	if ! timeout 10s docker exec "$CLIENT" wrk -t"$THREADS" -c"$CONNECTIONS" \
		-d5s "http://$GUEST_IP:8080/" >"$OUTPUT/wrk-warmup.log" 2>&1; then
		fail "wrk warmup failed or timed out"
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

if [[ "$HARNESS_STATUS" == PASS ]]; then
	printf 'PASS\n' >"$OUTPUT/harness.status"
fi

trap - EXIT
finish
finish_status=$?
[[ "$HARNESS_STATUS" == PASS && "$finish_status" -eq 0 ]]
