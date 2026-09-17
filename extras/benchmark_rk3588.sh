#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: $0 REFERENCE_BIN CANDIDATE_BIN TEST_DATA NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
reference=$1
candidate=$2
data=$3
output=$4
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ ! -r /proc/device-tree/compatible ]] ||
   ! tr '\0' '\n' < /proc/device-tree/compatible | grep -q 'rockchip,rk3588'; then
    echo "This runner requires an actual RK3588/RK3588S Linux board; host emulation is not a board measurement." >&2
    exit 2
fi
if [[ -e "$output" ]]; then
    echo "Choose a new output directory: $output" >&2
    exit 2
fi
mkdir -p -- "$output"

capture_board() {
    python3 - <<'PY'
import glob, json, os, platform
from pathlib import Path
info = {"kernel": platform.release(), "affinity": sorted(os.sched_getaffinity(0))}
for name in ("model", "compatible"):
    path = Path("/proc/device-tree") / name
    if path.exists():
        info[name] = path.read_bytes().replace(b"\0", b";").decode(errors="replace")
for pattern in (
    "/sys/devices/system/cpu/cpu[0-9]*/cpu_capacity",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_governor",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq",
    "/sys/devices/system/cpu/cpufreq/policy*/cpuinfo_max_freq",
    "/sys/class/thermal/thermal_zone*/temp",
):
    for name in glob.glob(pattern):
        info[name] = Path(name).read_text().strip()
print(json.dumps(info, indent=2))
PY
}
capture_board > "$output/board-before.json"
export OMP_WAIT_POLICY=${OMP_WAIT_POLICY:-PASSIVE}
export OMP_DYNAMIC=FALSE
export OMP_MAX_ACTIVE_LEVELS=1
export OMP_PROC_BIND=${OMP_PROC_BIND:-close}
export OMP_PLACES=${OMP_PLACES:-cores}

runner=()
if [[ -n "${CPU_LIST:-}" ]]; then
    runner=(taskset --cpu-list "$CPU_LIST")
fi
read -r -a threads <<< "${THREADS:-1 2 4 8}"
"${runner[@]}" python3 "$script_dir/evaluate.py" \
    --reference "$reference" --candidate "$candidate" --data "$data" \
    --output "$output/results" --threads "${threads[@]}" \
    --repeats "${REPEATS:-5}" --warmups "${WARMUPS:-1}"
capture_board > "$output/board-after.json"
