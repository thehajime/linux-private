#!/usr/bin/env bash
set -uo pipefail

usage() {
	echo "Usage: run-native-nginx-benchmark.sh --workers N --threads N --connections N --duration SEC --output DIR" >&2
	exit 2
}

while (($#)); do
	case "$1" in
	--workers) WORKERS=$2; shift 2;;
	--threads) THREADS=$2; shift 2;;
	--connections) CONNECTIONS=$2; shift 2;;
	--duration) DURATION=$2; shift 2;;
	--output) OUTPUT=$2; shift 2;;
	*) echo "Unknown argument: $1" >&2; usage;;
	esac
done

: "${WORKERS:?missing --workers}"
: "${THREADS:?missing --threads}"
: "${CONNECTIONS:?missing --connections}"
: "${DURATION:?missing --duration}"
: "${OUTPUT:?missing --output}"
mkdir -p "$OUTPUT"
OUTPUT=$(cd "$OUTPUT" && pwd -P)
case "$WORKERS:$THREADS:$CONNECTIONS" in
	1:2:4|1:2:16|1:2:64|4:2:4|4:2:16|4:2:64) ;;
	*) echo "unsupported benchmark parameters" >&2; usage;;
esac

mkdir -p "$OUTPUT/htdocs" "$OUTPUT/tmp" \
	"$OUTPUT/tmp/client-body" "$OUTPUT/tmp/proxy" \
	"$OUTPUT/tmp/fastcgi" "$OUTPUT/tmp/uwsgi" "$OUTPUT/tmp/scgi"
printf 'nginx-matrix-ok\n' >"$OUTPUT/htdocs/index.html"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
	-subj '/CN=native-nginx.local' \
	-keyout "$OUTPUT/native.key" -out "$OUTPUT/native.crt" \
	2>"$OUTPUT/certificate.log"
chmod 0600 "$OUTPUT/native.key"
cat >"$OUTPUT/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon off;
pid $OUTPUT/nginx.pid;
error_log $OUTPUT/nginx.log notice;
events { worker_connections 256; }
http {
	access_log off;
	client_body_temp_path $OUTPUT/tmp/client-body;
	proxy_temp_path $OUTPUT/tmp/proxy;
	fastcgi_temp_path $OUTPUT/tmp/fastcgi;
	uwsgi_temp_path $OUTPUT/tmp/uwsgi;
	scgi_temp_path $OUTPUT/tmp/scgi;
	server {
		listen 127.0.0.1:8080;
		root $OUTPUT/htdocs;
		location / { try_files \$uri \$uri/ =404; }
	}
	server {
		listen 127.0.0.1:8443 ssl;
		ssl_certificate $OUTPUT/native.crt;
		ssl_certificate_key $OUTPUT/native.key;
		ssl_protocols TLSv1.2 TLSv1.3;
		root $OUTPUT/htdocs;
		location / { try_files \$uri \$uri/ =404; }
	}
}
EOF

server_pid=
cleanup() {
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT

failures=()
printf 'RUNNING\n' >"$OUTPUT/harness.status"
printf 'NOT_RUN\n' >"$OUTPUT/smoke-http.status"
printf 'NOT_RUN\n' >"$OUTPUT/smoke-tls.status"
printf 'NOT_RUN\n' >"$OUTPUT/wrk-warmup.status"
printf '%s\n' "$WORKERS" >"$OUTPUT/workers.txt"
printf '1\n' >"$OUTPUT/ncpus.txt"
printf '%s\n' "$THREADS" >"$OUTPUT/threads.txt"
printf '%s\n' "$CONNECTIONS" >"$OUTPUT/connections.txt"
printf '%s\n' "$DURATION" >"$OUTPUT/duration.txt"
printf '%s\n' "$(nproc)" >"$OUTPUT/host-cpu-count.txt"

if ! nginx -t -c "$OUTPUT/nginx.conf" >"$OUTPUT/nginx-test.log" 2>&1; then
	failures+=("nginx configuration check failed")
	cat "$OUTPUT/nginx-test.log"
else
	printf 'Starting native nginx: workers=%s host_cpus=%s\n' "$WORKERS" "$(nproc)"
	nginx -c "$OUTPUT/nginx.conf" >"$OUTPUT/nginx-stdout.log" 2>&1 &
	server_pid=$!
	ready=0
	for _ in {1..60}; do
		if curl --fail --silent --show-error --max-time 1 http://127.0.0.1:8080/ \
			>"$OUTPUT/http.body" 2>"$OUTPUT/smoke-http.log"; then
			ready=1
			break
		fi
		kill -0 "$server_pid" 2>/dev/null || break
		sleep 1
	done
	if [[ "$ready" != 1 ]] || [[ "$(cat "$OUTPUT/http.body" 2>/dev/null)" != nginx-matrix-ok ]]; then
		printf 'FAIL\n' >"$OUTPUT/smoke-http.status"
		failures+=("native nginx HTTP readiness/smoke failed")
		cat "$OUTPUT/smoke-http.log"
	else
		printf 'Native nginx HTTP smoke passed.\n'
		printf 'PASS\n' >"$OUTPUT/smoke-http.status"
	fi
	if curl --fail --silent --show-error --insecure --max-time 5 \
		--output "$OUTPUT/tls.body" --stderr "$OUTPUT/smoke-tls.log" \
		https://127.0.0.1:8443/ && \
		[[ "$(cat "$OUTPUT/tls.body")" == nginx-matrix-ok ]]; then
		printf 'PASS\n' >"$OUTPUT/smoke-tls.status"
		printf 'Native nginx TLS smoke passed.\n'
	else
		printf 'FAIL\n' >"$OUTPUT/smoke-tls.status"
		failures+=("native nginx TLS smoke failed")
		cat "$OUTPUT/smoke-tls.log"
	fi
fi

if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
	warmup_rc=0
	printf 'Starting native wrk warmup: connections=%s duration=15s.\n' "$CONNECTIONS"
	timeout --foreground --signal=TERM --kill-after=5s 90s \
		wrk -t"$THREADS" -c"$CONNECTIONS" -T 30s -d15s \
		http://127.0.0.1:8080/ >"$OUTPUT/wrk-warmup.log" 2>&1 || warmup_rc=$?
	if [[ "$warmup_rc" -eq 0 ]] && \
		awk '/Requests\/sec:/ { if ($2 > 0) found = 1 } END { exit !found }' "$OUTPUT/wrk-warmup.log" && \
		! grep -Eq 'Socket errors:.*(connect|read|write|timeout) [1-9][0-9]*' "$OUTPUT/wrk-warmup.log"; then
		printf 'PASS\n' >"$OUTPUT/wrk-warmup.status"
	else
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		printf 'wrk warmup failed with exit status %s\n' "$warmup_rc" >"$OUTPUT/wrk-warmup.error"
	fi
	cat "$OUTPUT/wrk-warmup.log"
	wrk_rc=0
	printf 'Starting native wrk measurement: workers=%s connections=%s duration=%ss.\n' "$WORKERS" "$CONNECTIONS" "$DURATION"
	timeout --foreground --signal=TERM --kill-after=5s "$((DURATION + 45))s" \
		wrk -t"$THREADS" -c"$CONNECTIONS" -T 30s -d"${DURATION}s" --latency \
		http://127.0.0.1:8080/ >"$OUTPUT/wrk.log" 2>&1 || wrk_rc=$?
	printf '%s\n' "$wrk_rc" >"$OUTPUT/wrk.exit-code"
	if [[ "$wrk_rc" -ne 0 ]] || \
		! awk '/Requests\/sec:/ { if ($2 > 0) found = 1 } END { exit !found }' "$OUTPUT/wrk.log"; then
		failures+=("native wrk measurement failed with exit status $wrk_rc")
	fi
	cat "$OUTPUT/wrk.log"
else
	printf 'not run: nginx did not start\n' >"$OUTPUT/wrk.log"
	printf '125\n' >"$OUTPUT/wrk.exit-code"
fi

if ((${#failures[@]})); then
	printf 'FAIL\n' >"$OUTPUT/harness.status"
	printf '%s\n' "${failures[@]}" >"$OUTPUT/harness.error"
else
	printf 'PASS\n' >"$OUTPUT/harness.status"
	: >"$OUTPUT/harness.error"
fi

python3 - "$OUTPUT" "$WORKERS" "$THREADS" "$CONNECTIONS" "$DURATION" <<'PY'
import json
import pathlib
import re
import sys

out = pathlib.Path(sys.argv[1])
workers, threads, connections, duration = map(int, sys.argv[2:])
raw = (out / "wrk.log").read_text(errors="replace")
failures = (out / "harness.error").read_text().splitlines()
warnings = []
warmup_status = (out / "wrk-warmup.status").read_text().strip()
if warmup_status != "PASS":
    warnings.append(f"wrk warmup status is {warmup_status or 'missing'}")
request_match = re.search(r"Requests/sec:\s*([0-9.]+)", raw)
latency_match = re.search(r"^\s*Latency\s+([0-9.]+)(ns|us|µs|ms|s)", raw, re.M)
latency = None
if latency_match:
    amount = float(latency_match.group(1))
    latency = amount * {"ns": 1e-6, "us": 1e-3, "µs": 1e-3, "ms": 1.0, "s": 1000.0}[latency_match.group(2)]
socket_match = re.search(r"Socket errors:\s*connect\s+(\d+),\s*read\s+(\d+),\s*write\s+(\d+),\s*timeout\s+(\d+)", raw, re.I)
socket_errors = [int(value) for value in socket_match.groups()] if socket_match else ([0, 0, 0, 0] if request_match else None)
if not request_match or float(request_match.group(1)) <= 0:
    failures.append("wrk Requests/sec measurement is missing or not positive")
if latency is None:
    failures.append("wrk latency measurement is missing")
if socket_errors is None:
    failures.append("wrk socket error summary is missing")
elif any(socket_errors):
    failures.append(f"wrk reported socket errors {socket_errors}")
result = {
    "schema_version": 1,
    "status": "FAIL" if failures else "PASS",
    "failures": failures,
    "warnings": warnings,
    "profile": "native",
    "workers": workers,
    "ncpus": 1,
    "host_cpu_count": int((out / "host-cpu-count.txt").read_text().strip()),
    "wrk_threads": threads,
    "connections": connections,
    "duration_seconds": duration,
    "warmup_status": warmup_status,
    "kernel_sha": "native-runner",
    "aports_sha": "system-packages",
    "gcc_version": "system-nginx",
    "requests_per_second": float(request_match.group(1)) if request_match else None,
    "average_latency_ms": latency,
    "socket_errors": socket_errors,
    "smoke_http": (out / "smoke-http.status").read_text().strip() == "PASS",
    "smoke_tls": (out / "smoke-tls.status").read_text().strip() == "PASS",
    "raw_logs": ["nginx.conf", "nginx.log", "nginx-stdout.log", "nginx-test.log", "wrk-warmup.log", "wrk.log", "smoke-http.log", "smoke-tls.log"],
}
(out / "measurement.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
print(json.dumps(result, sort_keys=True))
raise SystemExit(1 if failures else 0)
PY
