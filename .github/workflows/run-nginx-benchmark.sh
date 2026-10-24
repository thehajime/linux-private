#!/usr/bin/env bash
set -uo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: run-nginx-benchmark.sh --kernel FILE --rootfs DIR --workers N --ncpus N \
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
	--ncpus) NCPUS=$2; shift 2;;
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
: "${NCPUS:?missing --ncpus}"
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
: "${NGINX_DIAGNOSTICS:=0}"
: "${NGINX_CONTROL_CHANNEL:=auto}"
: "${NGINX_WRK_WARMUP_SECONDS:=15}"
: "${NGINX_WRK_TIMEOUT_SECONDS:=30}"
for variable in NGINX_WRK_WARMUP_SECONDS NGINX_WRK_TIMEOUT_SECONDS; do
	if [[ ! "${!variable}" =~ ^[1-9][0-9]*$ ]]; then
		echo "$variable must be a positive integer" >&2
		exit 2
	fi
done
: "${NGINX_WRK_WARMUP_TIMEOUT_SECONDS:=$((NGINX_WRK_WARMUP_SECONDS + NGINX_WRK_TIMEOUT_SECONDS + 15))}"
if [[ ! "$NGINX_WRK_WARMUP_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
	echo "NGINX_WRK_WARMUP_TIMEOUT_SECONDS must be a positive integer" >&2
	exit 2
fi
KERNEL_ARGS=()
if [[ -n "$KERNEL_ARGS_TEXT" ]]; then
	read -r -a KERNEL_ARGS <<<"$KERNEL_ARGS_TEXT"
fi

mkdir -p "$OUTPUT"
printf 'RUNNING\n' >"$OUTPUT/harness.status"
printf 'NOT_RUN\n' >"$OUTPUT/wrk-warmup.status"
printf '%s\n' "$WORKERS" >"$OUTPUT/workers.txt"
printf '%s\n' "$NCPUS" >"$OUTPUT/ncpus.txt"
printf '%s\n' "$THREADS" >"$OUTPUT/threads.txt"
printf '%s\n' "$CONNECTIONS" >"$OUTPUT/connections.txt"
printf '%s\n' "$DURATION" >"$OUTPUT/duration.txt"
printf '%s\n' "$NGINX_WRK_WARMUP_SECONDS" >"$OUTPUT/warmup-duration.txt"
printf '%s\n' "$NGINX_WRK_WARMUP_TIMEOUT_SECONDS" >"$OUTPUT/warmup-timeout.txt"
printf '%s\n' "$NGINX_WRK_TIMEOUT_SECONDS" >"$OUTPUT/wrk-timeout.txt"
printf '%s\n' "$KERNEL_SHA" >"$OUTPUT/kernel-sha.txt"
printf '%s\n' "$APORTS_SHA" >"$OUTPUT/aports-sha.txt"
printf '%s\n' "$GCC_VERSION" >"$OUTPUT/gcc-version.txt"
: >"$OUTPUT/host-runtime-probe.log"
: >"$OUTPUT/host-vmlinux-backtrace.log"
{
	for resource in /proc/self/cgroup /proc/pressure/cpu \
		/sys/fs/cgroup/cpu.max /sys/fs/cgroup/cpu.stat \
		/sys/fs/cgroup/cpu/cpu.cfs_quota_us \
		/sys/fs/cgroup/cpu/cpu.cfs_period_us; do
		if [[ -r "$resource" ]]; then
			printf '\n%s\n' "$resource"
			cat "$resource"
		fi
	done
	group=$(awk -F: '$1 == "0" { print $3 }' /proc/self/cgroup)
	while [[ "$group" == /* ]]; do
		for resource in cpu.max cpu.stat; do
			file="/sys/fs/cgroup${group%/}/$resource"
			if [[ -r "$file" ]]; then
				printf '\n%s\n' "$file"
				cat "$file"
			fi
		done
		[[ "$group" == / ]] && break
		group=$(dirname "$group")
	done
} >"$OUTPUT/host-cpu-limits.log"
: >"$OUTPUT/guest-runtime-probe.log"
: >"$OUTPUT/tap100-tcpdump.log"
printf 'SKIP\n' >"$OUTPUT/tap100-pcap.status"
TAP_CAPTURE_PID_FILE="$OUTPUT/tap100-tcpdump.pid"

HARNESS_ERROR=
CLIENT=swmmu-wrk-client
UML_PID=
HOST_PROBE_PID=
TAP_CAPTURE_PID=
TAP_CAPTURE_LAUNCHER_PID=
CONSOLE_TAIL_PID=
HARNESS_STATUS=PASS
CONTROL_PID=
CONTROL_DIR=
CONTROL_FIFO=
CONTROL_PTY=
CONTROL_CHANNEL=$NGINX_CONTROL_CHANNEL
if [[ "$CONTROL_CHANNEL" == auto ]]; then
	case "$PROFILE" in
		um-nommu-sas-seccomp|um-nommu-sas-zpoline) CONTROL_CHANNEL=serial ;;
		*) CONTROL_CHANNEL=legacy ;;
	esac
fi
case "$CONTROL_CHANNEL" in
	serial|legacy) ;;
	*) echo 'invalid NGINX_CONTROL_CHANNEL' >&2; exit 2 ;;
esac
printf '%s\n' "$CONTROL_CHANNEL" >"$OUTPUT/control-channel.txt"
GUEST_IP=172.18.0.2
EXPECTED_BODY=nginx-matrix-ok
if [[ "$PROFILE" == um-nommu-sas-seccomp || "$PROFILE" == um-nommu-sas-zpoline ]]; then
	EXPECTED_BODY=swmmu-nginx-ok
fi

case "$WORKERS" in 1|4|8|16) ;; *) echo "unsupported worker count" >&2; usage;; esac
case "$CONNECTIONS" in 1|4|8|16) ;; *) echo "unsupported connection count" >&2; usage;; esac
case "$THREADS:$CONNECTIONS" in
	1:1|2:4|2:8|2:16) ;;
	*) echo "unsupported wrk thread/connection combination" >&2; usage;;
esac

fail() {
	HARNESS_STATUS=FAIL
	if [[ -n "$HARNESS_ERROR" ]]; then
		HARNESS_ERROR+="; "
	fi
	HARNESS_ERROR+="$*"
	printf '%s\n' "$HARNESS_ERROR" >"$OUTPUT/harness.error"
	printf 'nginx harness failure: %s\n' "$*" >&2
}

send_guest_command() {
	local message=$1
	local fifo="$ROOTFS/run/nginx-benchmark-stop"
	if [[ "$CONTROL_CHANNEL" == serial ]]; then
		fifo=$CONTROL_FIFO
	fi
	[[ -p "$fifo" ]] || return 1
	timeout --foreground 5s bash -c 'printf "%s\n" "$2" >"$1"' _ \
		"$fifo" "$message"
}

start_serial_control() {
	[[ "$CONTROL_CHANNEL" == serial ]] || return 0
	case "$PROFILE" in
		um-nommu-sas-seccomp|um-nommu-sas-zpoline) ;;
		*) fail 'serial control currently requires the SWMMU nginx init'; return 1 ;;
	esac
	CONTROL_DIR=$(mktemp -d)
	CONTROL_FIFO="$CONTROL_DIR/commands"
	python3 .github/workflows/uml-tty-relay.py --fifo "$CONTROL_FIFO" \
		--endpoint "$CONTROL_DIR/pty" --log "$OUTPUT/serial-control.log" \
		>"$OUTPUT/serial-control-helper.log" 2>&1 &
	CONTROL_PID=$!
	for _ in {1..100}; do
		[[ -s "$CONTROL_DIR/pty" ]] && break
		kill -0 "$CONTROL_PID" 2>/dev/null || break
		sleep 0.05
	done
	if [[ ! -s "$CONTROL_DIR/pty" ]]; then
		fail 'could not start host serial control relay'
		return 1
	fi
	CONTROL_PTY=$(cat "$CONTROL_DIR/pty")
	KERNEL_ARGS+=("ssl0=tty:$CONTROL_PTY")
}

capture_host_vmlinux_backtrace() {
	local timeout_pid=${UML_PID:-}
	local pid=
	local log="$OUTPUT/host-vmlinux-backtrace.log"
	local trace_status
	if [[ "$NGINX_DIAGNOSTICS" != 1 ]]; then
	case "$PROFILE:$WORKERS:$CONNECTIONS" in
		um-nommu-sas-seccomp:4:*) ;;
		um-nommu-sas-zpoline:1:4) ;;
		*) return 0 ;;
	esac
	fi
	{
		printf 'Host UML backtrace captured after a benchmark check failed.\n'
		if [[ -z "$timeout_pid" ]] || ! kill -0 "$timeout_pid" 2>/dev/null; then
			printf 'UML timeout wrapper is no longer running (pid=%s).\n' "${timeout_pid:-unknown}"
			return 0
		fi
		pid=$(ps -eo pid=,ppid=,comm= | awk -v parent="$timeout_pid" '$2 == parent && $3 == "vmlinux" {print $1; exit}')
		if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
			printf 'Could not find a live vmlinux child of timeout pid=%s.\n' "$timeout_pid"
			ps -eo pid,ppid,stat,pcpu,time,wchan:32,comm,args
			return 0
		fi
		printf 'pid=%s exe=%s\n' "$pid" "$(readlink -f "/proc/$pid/exe" 2>/dev/null || echo unavailable)"
		printf '\nHost process mappings (includes guest SWMMU aliases when present):\n'
		cat "/proc/$pid/maps" 2>&1 || true
		if [[ "$NGINX_DIAGNOSTICS" == 1 ]] && command -v strace >/dev/null 2>&1; then
			# Run only after failure, before GDB stops can alter the stalled state.
			trace_status=0
			sudo timeout --signal=INT --kill-after=2s 3s \
				strace -f -w -c -e trace=mmap,munmap,mprotect,poll,epoll_wait,futex \
				-p "$pid" >"$OUTPUT/host-syscall-profile.log" 2>&1 || trace_status=$?
			printf '\nstrace timeout status=%s (124 is the intended sampling bound)\n' \
				"$trace_status" >>"$OUTPUT/host-syscall-profile.log"
		fi
		if ! command -v gdb >/dev/null 2>&1; then
			printf 'gdb is unavailable on the runner.\n'
			return 0
		fi
		for sample in 1 2 3; do
			printf '\nSnapshot %s at %s\n' "$sample" "$(date -Ins)"
			sudo timeout --foreground --signal=TERM --kill-after=3s 20s \
				gdb -q -batch -ex 'set pagination off' -ex 'set debuginfod enabled off' \
				-ex 'info registers rip rsp rdi rsi rdx rax' -ex 'x/16i $rip' \
				-ex 'p cpu_tasks[0]->pid' -ex 'p cpu_tasks[0]->comm' \
				-ex 'p cpu_tasks[0]->mm' -ex 'p swmmu_host_active_mm' \
				-ex 'p cpu_tasks[0]->nvcsw' -ex 'p cpu_tasks[0]->nivcsw' \
				-ex 'p cpu_tasks[0]->se.sum_exec_runtime' \
				-ex 'p cpu_tasks[0]->thread.regs' \
				-ex 'info threads' -ex 'thread apply all bt 15' \
				-ex detach -ex quit -p "$pid" || true
			[[ "$sample" == 3 ]] || sleep 1
		done

	} >"$log" 2>&1 || printf 'GDB attach/backtrace failed (exit=%s).\n' "$?" >>"$log"
}

start_tap_capture() {
	if [[ "$NGINX_DIAGNOSTICS" != 1 ]]; then
	case "$PROFILE:$WORKERS:$CONNECTIONS" in
		um-nommu-sas-seccomp:4:*) ;;
		um-nommu-sas-zpoline:1:4) ;;
		*) return 0 ;;
	esac
	fi
	if ! command -v tcpdump >/dev/null 2>&1; then
		printf 'FAIL: tcpdump is unavailable\n' >"$OUTPUT/tap100-pcap.status"
		return 0
	fi
	rm -f "$TAP_CAPTURE_PID_FILE"
	sudo bash -c 'printf "%s\n" "$$" >"$1"; shift; exec "$@"' _ \
		"$TAP_CAPTURE_PID_FILE" tcpdump -n -i tap100 -U -s 128 \
		-w "$OUTPUT/tap100.pcap" 'tcp port 8080 or tcp port 8443' \
		>"$OUTPUT/tap100-tcpdump.log" 2>&1 &
	TAP_CAPTURE_LAUNCHER_PID=$!
	for _ in {1..50}; do
		[[ -s "$TAP_CAPTURE_PID_FILE" ]] && break
		kill -0 "$TAP_CAPTURE_LAUNCHER_PID" 2>/dev/null || break
		sleep 0.1
	done
	TAP_CAPTURE_PID=$(cat "$TAP_CAPTURE_PID_FILE" 2>/dev/null || true)
	if [[ -z "$TAP_CAPTURE_PID" ]] || ! sudo kill -0 "$TAP_CAPTURE_PID" 2>/dev/null; then
		kill -KILL "$TAP_CAPTURE_LAUNCHER_PID" 2>/dev/null || true
		TAP_CAPTURE_LAUNCHER_PID=
		TAP_CAPTURE_PID=
		printf 'FAIL: could not start tcpdump\n' >"$OUTPUT/tap100-pcap.status"
		return 0
	fi
	printf 'RUNNING\n' >"$OUTPUT/tap100-pcap.status"
}

stop_tap_capture() {
	if [[ -z "$TAP_CAPTURE_PID" ]]; then
		return 0
	fi
	sudo kill -INT "$TAP_CAPTURE_PID" 2>/dev/null || true
	for _ in {1..50}; do
		sudo kill -0 "$TAP_CAPTURE_PID" 2>/dev/null || break
		sleep 0.1
	done
	if sudo kill -0 "$TAP_CAPTURE_PID" 2>/dev/null; then
		printf 'tcpdump did not stop after SIGINT; sending SIGTERM\n' >>"$OUTPUT/tap100-tcpdump.log"
		sudo kill -TERM "$TAP_CAPTURE_PID" 2>/dev/null || true
		for _ in {1..20}; do
			sudo kill -0 "$TAP_CAPTURE_PID" 2>/dev/null || break
			sleep 0.1
		done
	fi
	if sudo kill -0 "$TAP_CAPTURE_PID" 2>/dev/null; then
		printf 'tcpdump did not stop after SIGTERM; sending SIGKILL\n' >>"$OUTPUT/tap100-tcpdump.log"
		sudo kill -KILL "$TAP_CAPTURE_PID" 2>/dev/null || true
	fi
	TAP_CAPTURE_PID=
	if [[ -n "$TAP_CAPTURE_LAUNCHER_PID" ]]; then
		for _ in {1..20}; do
			kill -0 "$TAP_CAPTURE_LAUNCHER_PID" 2>/dev/null || break
			sleep 0.1
		done
		if ! kill -0 "$TAP_CAPTURE_LAUNCHER_PID" 2>/dev/null; then
			wait "$TAP_CAPTURE_LAUNCHER_PID" 2>/dev/null || true
		else
			kill "$TAP_CAPTURE_LAUNCHER_PID" 2>/dev/null || true
			printf 'tcpdump launcher did not exit promptly\n' >>"$OUTPUT/tap100-tcpdump.log"
		fi
		TAP_CAPTURE_LAUNCHER_PID=
	fi
	if [[ -s "$OUTPUT/tap100.pcap" ]]; then
		if timeout --foreground 10s sudo tcpdump -nn -tttt -r "$OUTPUT/tap100.pcap" \
			>"$OUTPUT/tap100-packets.txt" \
			2>>"$OUTPUT/tap100-tcpdump.log"; then
			printf 'PASS\n' >"$OUTPUT/tap100-pcap.status"
		else
			printf 'FAIL: could not decode pcap\n' >"$OUTPUT/tap100-pcap.status"
		fi
	else
		printf 'FAIL: capture produced no pcap\n' >"$OUTPUT/tap100-pcap.status"
	fi
}

stop_console_log() {
	if [[ -n "$CONSOLE_TAIL_PID" ]]; then
		kill "$CONSOLE_TAIL_PID" 2>/dev/null || true
		wait "$CONSOLE_TAIL_PID" 2>/dev/null || true
		CONSOLE_TAIL_PID=
	fi
}

capture_logs() {
	local uml_status=0
	stop_tap_capture
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
			if [[ "$CONTROL_CHANNEL" == serial || -p "$ROOTFS/run/nginx-benchmark-stop" ]]; then
				send_guest_command stop 2>/dev/null || true
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
		wait "$UML_PID" 2>/dev/null || uml_status=$?
		printf '%s\n' "$uml_status" >"$OUTPUT/uml-exit.txt"
		if [[ "$CONTROL_CHANNEL" == serial ]] && \
		   { [[ "$uml_status" != 0 ]] || ! grep -q 'reboot: Power down' "$OUTPUT/kernel.log"; }; then
			fail 'guest serial shutdown did not complete normally'
		fi
		UML_PID=
	fi
	if [[ -n "$CONTROL_PID" ]]; then
		kill -TERM "$CONTROL_PID" 2>/dev/null || true
		wait "$CONTROL_PID" 2>/dev/null || true
		CONTROL_PID=
	fi
	if [[ -n "$CONTROL_DIR" ]]; then
		rm -rf "$CONTROL_DIR"
		CONTROL_DIR=
	fi
	stop_console_log
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
			# Keep normal serial-controlled measurements free of guest probes.
			# Legacy replay retains its original diagnostic behavior.
			if [[ "$CONTROL_CHANNEL" == legacy && -p "$ROOTFS/run/nginx-benchmark-stop" ]]; then
				if ! send_guest_command "probe $(date -Ins)"; then
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
		--output "$OUTPUT" --workers "$WORKERS" --ncpus "$NCPUS" \
		--threads "$THREADS" \
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
sed -i -E 's#^error_log[[:space:]].*;#error_log /run/nginx-error.log notice;#' "$NGINX_CONF"
if [[ "$PROFILE" == um-nommu || "$PROFILE" == um-nommu-seccomp || "$PROFILE" == um-nommu-plain-sas || "$PROFILE" == um-nommu-plain-sas-zpoline ]]; then
	sed -i -e 's/^daemon on;$/daemon off;/' \
		-e 's/^master_process on;$/master_process off;/' "$NGINX_CONF"
	install -m 0755 .github/workflows/nginx-matrix-init.sh \
		"$ROOTFS/sbin/nginx-matrix-init"
fi
cp "$NGINX_CONF" "$OUTPUT/nginx.conf" 2>/dev/null || \
	fail "nginx SWMMU config is missing"
if ! grep -q "^worker_processes ${WORKERS};$" "$OUTPUT/nginx.conf" 2>/dev/null || \
	! grep -q '^error_log /run/nginx-error.log notice;$' "$OUTPUT/nginx.conf" 2>/dev/null; then
	fail "matrix worker count or nginx diagnostic log configuration was not applied"
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	start_serial_control || true
	KERNEL_ARGS+=("NGINX_CONTROL_CHANNEL=$CONTROL_CHANNEL")
	if [[ "$CONTROL_CHANNEL" == serial ]]; then
		install -m 0755 .github/workflows/swmmu-nginx-init.sh \
			"$ROOTFS/sbin/swmmu-nginx-init"
	fi
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	: >"$OUTPUT/kernel.log"
	tail -n +1 -F "$OUTPUT/kernel.log" &
	CONSOLE_TAIL_PID=$!
	printf 'Starting UML nginx guest: profile=%s workers=%s connections=%s\n' \
		"$PROFILE" "$WORKERS" "$CONNECTIONS"
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
		printf 'UML nginx guest ready; running HTTP/TLS smoke tests and wrk.\n'
		if [[ -s "$ROOTFS/run/online-cpus.txt" ]]; then
			cp "$ROOTFS/run/online-cpus.txt" "$OUTPUT/online-cpus.txt"
		fi
		if [[ -s "$ROOTFS/run/nginx-worker-count.txt" ]]; then
			cp "$ROOTFS/run/nginx-worker-count.txt" "$OUTPUT/nginx-worker-count.txt"
			actual_workers=$(cat "$OUTPUT/nginx-worker-count.txt")
			if [[ "$actual_workers" != "$WORKERS" ]]; then
				fail "requested $WORKERS nginx workers but observed $actual_workers"
			fi
		fi
		if [[ " $KERNEL_ARGS_TEXT " == *" ncpus="* ]]; then
			online=$(tr -d '\n' <"$ROOTFS/run/online-cpus.txt" 2>/dev/null || true)
			case "$NCPUS:$online" in
				1:0|1:0-0) ;;
				4:0-3) ;;
				*) fail "UML expected $NCPUS CPUs online, observed '${online:-missing}'" ;;
			esac
			if [[ "$NCPUS" == 4 && "$online" != 0-3 ]] && \
			   ! grep -Eq 'smp: Brought up [0-9]+ node, 4 CPUs' "$OUTPUT/kernel.log"; then
				fail "SMP UML did not report all four CPUs online"
			fi
		fi
	fi
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	client_mounts=(
		--volume /usr/bin/curl:/usr/bin/curl:ro
		--volume /usr/bin/wrk:/usr/bin/wrk:ro
	)
	for binary in /usr/bin/curl /usr/bin/wrk; do
		while IFS= read -r library; do
			[[ -f "$library" ]] && client_mounts+=(--volume "$library:$library:ro")
		done < <(ldd "$binary" 2>/dev/null | awk '
			$1 ~ /^\// { print $1 }
			$2 == "=>" && $3 ~ /^\// { print $3 }
		' | sort -u)
	done
	if ! docker run --detach --name "$CLIENT" --network host \
		"${client_mounts[@]}" ubuntu:22.04 \
		bash -c 'sleep infinity' >"$OUTPUT/client-setup.log" 2>&1; then
		fail "could not start separate curl/wrk client container"
	elif ! docker exec "$CLIENT" /usr/bin/curl --version \
		>>"$OUTPUT/client-setup.log" 2>&1; then
		fail "host curl binary is not runnable in the separate client container"
	fi
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	start_tap_capture
	start_runtime_probes
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	printf 'Starting nginx HTTP and TLS smoke tests.\n'
	if timeout 10s docker exec "$CLIENT" curl --fail --silent --show-error \
		--max-time 5 "http://$GUEST_IP:8080/" >"$OUTPUT/http.body" \
		2>"$OUTPUT/smoke-http.log" && \
		[[ "$(cat "$OUTPUT/http.body")" == "$EXPECTED_BODY" ]]; then
		printf 'PASS\n' >"$OUTPUT/smoke-http.status"
	else
		printf 'FAIL\n' >"$OUTPUT/smoke-http.status"
		fail "HTTP smoke test returned an error or unexpected body"
		if [[ "$NGINX_DIAGNOSTICS" == 1 ]]; then
			capture_host_vmlinux_backtrace
			if [[ "$CONTROL_CHANNEL" == serial ]]; then
				send_guest_command "probe $(date -Ins)" || true
			fi
		fi
	fi
	if [[ -n "${NGINX_TLS_CURL_TIMEOUT:-}" ]]; then
		tls_curl_timeout=$NGINX_TLS_CURL_TIMEOUT
		tls_outer_timeout=$((tls_curl_timeout + 10))
	else
		tls_outer_timeout=10
		tls_curl_timeout=5
		if [[ "$PROFILE" == um-nommu-sas-seccomp ]]; then
			tls_outer_timeout=40
			tls_curl_timeout=30
		fi
	fi
	if timeout "${tls_outer_timeout}s" docker exec "$CLIENT" curl --fail --silent --show-error --verbose \
		--insecure --max-time "$tls_curl_timeout" "https://$GUEST_IP:8443/" >"$OUTPUT/tls.body" \
		2>"$OUTPUT/smoke-tls.log" && \
		[[ "$(cat "$OUTPUT/tls.body")" == "$EXPECTED_BODY" ]]; then
		printf 'PASS\n' >"$OUTPUT/smoke-tls.status"
	else
		printf 'FAIL\n' >"$OUTPUT/smoke-tls.status"
		fail "TLS smoke test returned an error or unexpected body"
		if [[ "$NGINX_DIAGNOSTICS" == 1 ]] || \
		   [[ "$PROFILE" == um-nommu-sas-zpoline && \
		      "$WORKERS" == 1 && "$CONNECTIONS" == 4 ]]; then
			capture_host_vmlinux_backtrace
		fi
	fi
else
	printf 'FAIL\n' >"$OUTPUT/smoke-http.status"
	printf 'FAIL\n' >"$OUTPUT/smoke-tls.status"
fi

if [[ "$HARNESS_STATUS" == PASS ]]; then
	printf 'Starting wrk warmup: workers=%s connections=%s duration=%ss timeout=%ss.\n' \
		"$WORKERS" "$CONNECTIONS" "$NGINX_WRK_WARMUP_SECONDS" "$NGINX_WRK_WARMUP_TIMEOUT_SECONDS"
	warmup_rc=0
	timeout "${NGINX_WRK_WARMUP_TIMEOUT_SECONDS}s" docker exec "$CLIENT" \
		wrk -t"$THREADS" -c"$CONNECTIONS" \
		-T "${NGINX_WRK_TIMEOUT_SECONDS}s" -d"${NGINX_WRK_WARMUP_SECONDS}s" \
		"http://$GUEST_IP:8080/" >"$OUTPUT/wrk-warmup.log" 2>&1 || warmup_rc=$?
	if [[ "$warmup_rc" -ne 0 ]]; then
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		printf 'wrk warmup failed or timed out with exit status %s\n' "$warmup_rc" \
			>"$OUTPUT/wrk-warmup.error"
	elif grep -Eq 'Socket errors:.*(connect|read|write|timeout) [1-9][0-9]*' \
		"$OUTPUT/wrk-warmup.log"; then
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		printf 'wrk warmup reported socket errors\n' >"$OUTPUT/wrk-warmup.error"
	elif ! awk '/Requests\/sec:/ { if ($2 > 0) found = 1 } END { exit !found }' \
		"$OUTPUT/wrk-warmup.log"; then
		printf 'FAIL\n' >"$OUTPUT/wrk-warmup.status"
		printf 'wrk warmup completed without serving any requests\n' \
			>"$OUTPUT/wrk-warmup.error"
	else
		printf 'PASS\n' >"$OUTPUT/wrk-warmup.status"
		rm -f "$OUTPUT/wrk-warmup.error"
	fi
	if [[ "$(cat "$OUTPUT/wrk-warmup.status")" == FAIL ]]; then
		printf 'Warmup did not complete cleanly; continue to the measured wrk run and retain both logs.\n' >&2
	fi
fi
if [[ "$HARNESS_STATUS" == PASS ]]; then
	# Independent wrk invocations, after one warmup, for same-condition boxes.
	NGINX_WRK_REPEATS=${NGINX_WRK_REPEATS:-5}
	if [[ ! "$NGINX_WRK_REPEATS" =~ ^[1-9][0-9]*$ ]] || ((NGINX_WRK_REPEATS > 20)); then
		fail "NGINX_WRK_REPEATS must be between 1 and 20"
		NGINX_WRK_REPEATS=1
	fi
	printf '%s\n' "$NGINX_WRK_REPEATS" >"$OUTPUT/wrk-sample-count.txt"
	for ((sample=1; sample<=NGINX_WRK_REPEATS; sample++)); do
		printf 'Starting main wrk measurement for %ss.\n' "$DURATION"
		wrk_rc=0
		timeout --foreground --signal=TERM --kill-after=5s \
			"$((DURATION + NGINX_WRK_TIMEOUT_SECONDS + 15))s" \
			docker exec "$CLIENT" wrk -t"$THREADS" -c"$CONNECTIONS" \
			-T "${NGINX_WRK_TIMEOUT_SECONDS}s" -d"${DURATION}s" --latency \
			"http://$GUEST_IP:8080/" \
			>"$OUTPUT/wrk.log" 2>&1 || wrk_rc=$?
		printf '%s\n' "$wrk_rc" >"$OUTPUT/wrk.exit-code"
		if [[ "$wrk_rc" -ne 0 ]]; then
			fail "wrk failed or timed out with exit status $wrk_rc"
		elif ! awk '/Requests\/sec:/ { if ($2 > 0) found = 1 } END { exit !found }' \
			"$OUTPUT/wrk.log"; then
			fail "wrk completed without serving any requests"
		fi
		if [[ "$(cat "$OUTPUT/wrk-warmup.status")" == FAIL ]] || \
		   [[ "$wrk_rc" -ne 0 ]] || \
		   ! awk '/Requests\/sec:/ { if ($2 > 0) found = 1 } END { exit !found }' \
			"$OUTPUT/wrk.log" || \
		   grep -Eq 'Socket errors:.*(connect|read|write|timeout) [1-9][0-9]*' \
			"$OUTPUT/wrk.log"; then
			capture_host_vmlinux_backtrace
		fi
		cp "$OUTPUT/wrk.log" "$OUTPUT/wrk-sample-$sample.log"
		cp "$OUTPUT/wrk.exit-code" "$OUTPUT/wrk-sample-$sample.exit-code"
	done
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
