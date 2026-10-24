#!/bin/bash

OUTPUT="$1"
mkdir -p "$OUTPUT/out"

declare -A getpid forward reverse
profiles=(um-mmu um-mmu-seccomp um-mmu-tt-seccomp um-nommu \
          um-nommu-seccomp um-nommu-tt-seccomp um-nommu-plain-sas \
          um-nommu-plain-sas-zpoline um-nommu-sas-seccomp um-nommu-sas-zpoline native)
display_profile() {
    case "$1" in
        native) echo 'native' ;;
        um-mmu) echo 'um-mmu(p)' ;;
        um-mmu-seccomp) echo 'um-mmu(s)' ;;
        um-mmu-tt-seccomp) echo 'um-mmu-tt(s)' ;;
        um-nommu) echo 'um-nommu(p)' ;;
        um-nommu-seccomp) echo 'um-nommu(s)' ;;
        um-nommu-tt-seccomp) echo 'um-nommu-tt(s)' ;;
        um-nommu-plain-sas) echo 'um-nommu-sas(s)' ;;
        um-nommu-plain-sas-zpoline) echo 'um-nommu-sas(z)' ;;
        um-nommu-sas-seccomp) echo 'um-nommu-sas-swmmu(s)' ;;
        um-nommu-sas-zpoline) echo 'um-nommu-sas-swmmu(z)' ;;
    esac
}
mkdir -p "$OUTPUT/out"

# Keep normalized legacy files for consumers that still read them.  Missing
# measurements are represented in the report tables instead of gnuplot errors.
for file in "$OUTPUT"/*.dat; do
    [ -f "$file" ] || continue
    name=$(basename "$file" .dat)
    case "$name" in
        *-iperf3|*ltp*) continue ;;
    esac
    awk '/microseconds/ { for (i=1; i<=NF; i++) if ($i == "microseconds") print $(i-1) }' \
        "$file" > "$OUTPUT/out/$name-lmbench-out.dat"
    getpid["$name"]=$(awk '/average/ && !/time/ {print $2; exit}' "$file")
    if [ -n "${getpid[$name]}" ]; then
        printf '%s\n' "${getpid[$name]}" > "$OUTPUT/out/$name-getpid-out.dat"
    fi
done

for file in "$OUTPUT"/*-iperf3.dat; do
    [ -f "$file" ] || continue
    name=$(basename "$file" -iperf3.dat)
    mapfile -t values < <(awk '/receiver/ {
        value=$(NF-2); unit=$(NF-1)
        if (unit == "Gbits/sec") value *= 1000
        else if (unit == "Kbits/sec") value /= 1000
        else if (unit == "bits/sec") value /= 1000000
        print value
    }' "$file")
    forward["$name"]=${values[0]:-}
    reverse["$name"]=${values[1]:-}
    if [ -n "${forward[$name]}" ] || [ -n "${reverse[$name]}" ]; then
        printf '%s\n' "${forward[$name]:-}" "${reverse[$name]:-}" \
            > "$OUTPUT/out/$name-iperf3-out.dat"
    fi
done

# Native iperf3 is logged per direction so failures cannot shift positions.
for direction in forward reverse; do
    file="$OUTPUT/native-iperf3-$direction.dat"
    [ -f "$file" ] || continue
    value=$(awk '/receiver/ {
        value=$(NF-2); unit=$(NF-1)
        if (unit == "Gbits/sec") value *= 1000
        else if (unit == "Kbits/sec") value /= 1000
        else if (unit == "bits/sec") value /= 1000000
        print value
    }' "$file" | tail -n 1)
    case "$direction" in
        forward) forward[native]=$value ;;
        reverse) reverse[native]=$value ;;
    esac
done

value_or_na() {
    [ -n "$1" ] && printf '%s' "$1" || printf 'N/A'
}

printf '\n### do_getpid bench (nsec)\n\n'
printf '| |'
for profile in "${profiles[@]}"; do printf '%s|' "$(display_profile "$profile")"; done
printf '\n|--|'
for profile in "${profiles[@]}"; do printf -- '--|'; done
printf '\n|getpid|'
for profile in "${profiles[@]}"; do printf '%s|' "$(value_or_na "${getpid[$profile]:-}")"; done
printf '\n'

printf '\n### iperf3 bench (Mbps)\n\n'
printf '| |'
for profile in "${profiles[@]}"; do printf '%s|' "$(display_profile "$profile")"; done
printf '\n|--|'
for profile in "${profiles[@]}"; do printf -- '--|'; done
printf '\n|iperf3(f)|'
for profile in "${profiles[@]}"; do printf '%s|' "$(value_or_na "${forward[$profile]:-}")"; done
printf '\n|iperf3(r)|'
for profile in "${profiles[@]}"; do printf '%s|' "$(value_or_na "${reverse[$profile]:-}")"; done
printf '\n'
