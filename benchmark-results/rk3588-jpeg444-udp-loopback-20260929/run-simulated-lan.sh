#!/usr/bin/env bash
set -euo pipefail

if [[ $(id -u) -ne 0 ]]; then
  echo "run-simulated-lan.sh must run as root" >&2
  exit 2
fi

PROBE=${PROBE:-/home/orangepi/mpp-eval-upload-20260929/rk3588_jpeg444_udp_loopback_probe}
INPUT=${INPUT:-/home/orangepi/jxs-eval-20260929/test-data/host-4k444/natural.yuv8p}
OUTDIR=${OUTDIR:-/home/orangepi/mpp-eval-upload-20260929/udp-loopback-formal}
FRAMES=${FRAMES:-1000}
WARMUPS=${WARMUPS:-30}
QUALITY=${QUALITY:-50}
QUEUE_CAPACITY=${QUEUE_CAPACITY:-3}
RUN_SATURATED=${RUN_SATURATED:-1}

CPU_GOVERNOR_PATHS=(
  /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
  /sys/devices/system/cpu/cpufreq/policy4/scaling_governor
  /sys/devices/system/cpu/cpufreq/policy6/scaling_governor
)
DMC_GOVERNOR_PATH=/sys/class/devfreq/dmc/governor
OLD_CPU_GOVERNORS=()
OLD_DMC_GOVERNOR=
OLD_RMEM_MAX=$(sysctl -n net.core.rmem_max)
OLD_WMEM_MAX=$(sysctl -n net.core.wmem_max)
TEMP_PID=

mkdir -p "$OUTDIR"
chown orangepi:orangepi "$OUTDIR"

cleanup() {
  local status=$?
  set +e
  if [[ -n "$TEMP_PID" ]]; then
    kill "$TEMP_PID" 2>/dev/null
    wait "$TEMP_PID" 2>/dev/null
  fi
  tc qdisc del dev lo root 2>/dev/null
  for index in "${!CPU_GOVERNOR_PATHS[@]}"; do
    if [[ -n "${OLD_CPU_GOVERNORS[$index]:-}" ]]; then
      printf '%s' "${OLD_CPU_GOVERNORS[$index]}" > "${CPU_GOVERNOR_PATHS[$index]}"
    fi
  done
  if [[ -n "$OLD_DMC_GOVERNOR" ]]; then
    printf '%s' "$OLD_DMC_GOVERNOR" > "$DMC_GOVERNOR_PATH"
  fi
  sysctl -q -w "net.core.rmem_max=$OLD_RMEM_MAX"
  sysctl -q -w "net.core.wmem_max=$OLD_WMEM_MAX"
  {
    echo "exit_status=$status"
    echo "qdisc=$(tc qdisc show dev lo)"
    for path in "${CPU_GOVERNOR_PATHS[@]}"; do
      echo "$path=$(cat "$path")"
    done
    echo "$DMC_GOVERNOR_PATH=$(cat "$DMC_GOVERNOR_PATH")"
    echo "net.core.rmem_max=$(sysctl -n net.core.rmem_max)"
    echo "net.core.wmem_max=$(sysctl -n net.core.wmem_max)"
  } > "$OUTDIR/restored-state.txt"
  chown -R orangepi:orangepi "$OUTDIR"
  exit "$status"
}
trap cleanup EXIT INT TERM

if ! tc qdisc show dev lo | grep -q '^qdisc noqueue'; then
  echo "refusing to replace an existing non-default loopback qdisc" >&2
  exit 2
fi
for path in "${CPU_GOVERNOR_PATHS[@]}"; do
  OLD_CPU_GOVERNORS+=("$(cat "$path")")
done
OLD_DMC_GOVERNOR=$(cat "$DMC_GOVERNOR_PATH")

sysctl -q -w net.core.rmem_max=16777216
sysctl -q -w net.core.wmem_max=16777216
for path in "${CPU_GOVERNOR_PATHS[@]}"; do
  printf performance > "$path"
done
printf performance > "$DMC_GOVERNOR_PATH"
tc qdisc replace dev lo root netem limit 10000 \
  delay 1ms 0.2ms distribution normal rate 1000mbit

{
  uname -a
  echo "probe_sha256=$(sha256sum "$PROBE" | awk '{print $1}')"
  echo "input_sha256=$(sha256sum "$INPUT" | awk '{print $1}')"
  echo "frames=$FRAMES"
  echo "warmups=$WARMUPS"
  echo "quality=$QUALITY"
  echo "queue_capacity=$QUEUE_CAPACITY"
  echo "qdisc=$(tc qdisc show dev lo)"
  for path in "${CPU_GOVERNOR_PATHS[@]}"; do
    echo "$path=$(cat "$path")"
  done
  echo "$DMC_GOVERNOR_PATH=$(cat "$DMC_GOVERNOR_PATH")"
  echo "dmc_frequency=$(cat /sys/class/devfreq/dmc/cur_freq)"
  echo "net.core.rmem_max=$(sysctl -n net.core.rmem_max)"
  echo "net.core.wmem_max=$(sysctl -n net.core.wmem_max)"
} > "$OUTDIR/active-state.txt"

(
  echo "timestamp,type,temp_mC"
  while true; do
    timestamp=$(date +%s.%N)
    for zone in /sys/class/thermal/thermal_zone*; do
      printf '%s,%s,%s\n' "$timestamp" "$(cat "$zone/type")" "$(cat "$zone/temp")"
    done
    sleep 1
  done
) > "$OUTDIR/temperatures.csv" &
TEMP_PID=$!

runuser -u orangepi -- env JXS_CHECKSUM_EVERY=100 \
  "$PROBE" "$INPUT" "$FRAMES" "$WARMUPS" 60 "$QUALITY" \
  "$QUEUE_CAPACITY" 39010 "$OUTDIR/paced.csv" \
  2>&1 | tee "$OUTDIR/paced.log"

if [[ "$RUN_SATURATED" == 1 ]]; then
  runuser -u orangepi -- env JXS_CHECKSUM_EVERY=100 \
    "$PROBE" "$INPUT" "$FRAMES" "$WARMUPS" 0 "$QUALITY" \
    "$QUEUE_CAPACITY" 39011 "$OUTDIR/saturated.csv" \
    2>&1 | tee "$OUTDIR/saturated.log"
fi
