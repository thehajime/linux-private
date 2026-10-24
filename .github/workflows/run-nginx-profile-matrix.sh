#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 4 ]]; then
	echo "usage: $0 PROFILE KERNEL ROOTFS_ARCHIVE OUTPUT_DIR" >&2
	exit 2
fi
PROFILE=$1
KERNEL=$2
ROOTFS_ARCHIVE=$3
OUTPUT_ROOT=$4

case "$PROFILE" in
	um-mmu-seccomp|um-nommu-seccomp) CPU_COUNTS=(1 4 8 16) ;;
	um-mmu|um-nommu|um-nommu-plain-sas|um-nommu-plain-sas-zpoline|um-nommu-sas-seccomp|um-nommu-sas-zpoline)
		CPU_COUNTS=(1) ;;
	*) echo "unknown nginx profile: $PROFILE" >&2; exit 2 ;;
esac
WORKER_COUNTS=(1 4 8 16)
CONNECTION_COUNTS=(1 4 8 16)
mkdir -p "$OUTPUT_ROOT/$PROFILE"

kernel_args()
{
	local ncpus=$1
	case "$PROFILE" in
		um-mmu|um-nommu) printf 'ncpus=%s' "$ncpus" ;;
		um-mmu-seccomp|um-nommu-seccomp) printf 'ncpus=%s seccomp=on' "$ncpus" ;;
		um-nommu-plain-sas) printf 'seccomp=on' ;;
		um-nommu-plain-sas-zpoline) printf 'zpoline=1' ;;
		um-nommu-sas-seccomp) printf 'zpoline=0 swmmu_direct=1' ;;
		um-nommu-sas-zpoline) printf 'zpoline=1 swmmu_direct=1' ;;
	esac
}

needs_rawio=false
case "$PROFILE" in *-zpoline) needs_rawio=true ;; esac
kernel_sha=$(sed -n 's/^kernel_sha=//p' "$RUNNER_TEMP/nginx-rootfs/manifest.txt")
aports_sha=$(sed -n 's/^aports_sha=//p' "$RUNNER_TEMP/nginx-rootfs/manifest.txt")
gcc_version=$(sed -n 's/^gcc_version=//p' "$RUNNER_TEMP/nginx-rootfs/manifest.txt")
status=0

for ncpus in "${CPU_COUNTS[@]}"; do
	for workers in "${WORKER_COUNTS[@]}"; do
		for connections in "${CONNECTION_COUNTS[@]}"; do
			threads=2
			if [[ "$connections" == 1 ]]; then
				threads=1
			fi
			out="$OUTPUT_ROOT/$PROFILE/w${workers}-n${ncpus}-c${connections}"
			rootfs="$RUNNER_TEMP/nginx-rootfs-${PROFILE}-${ncpus}-${workers}-${connections}"
			rm -rf "$rootfs"
			mkdir -p "$rootfs" "$out"
			tar -xzf "$ROOTFS_ARCHIVE" -C "$rootfs"
			printf 'source_run_id=%s\nhelper_commit=%s\ndiagnostics=%s\n' \
				"${NGINX_SOURCE_RUN_ID:-}" "${NGINX_HELPER_COMMIT:-}" \
				"${NGINX_DIAGNOSTICS:-0}" > "$out/replay-source.txt"
			printf 'Running profile=%s CPUs=%s workers=%s connections=%s\n' \
				"$PROFILE" "$ncpus" "$workers" "$connections"
			if ! NGINX_WRK_WARMUP_SECONDS=15 \
				bash .github/workflows/run-nginx-benchmark.sh \
					--kernel "$KERNEL" --rootfs "$rootfs" \
					--workers "$workers" --ncpus "$ncpus" --threads "$threads" \
					--connections "$connections" --duration 30 --profile "$PROFILE" \
					--kernel-args "$(kernel_args "$ncpus")" --needs-rawio "$needs_rawio" \
					--kernel-sha "$kernel_sha" --aports-sha "$aports_sha" \
					--gcc-version "$gcc_version" --output "$out"; then
				status=1
			fi
			rm -rf "$rootfs"
		done
	done
done
exit "$status"
