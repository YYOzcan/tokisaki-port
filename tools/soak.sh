#!/usr/bin/env bash
# Tokisaki Port -- stability check: N runs to gameplay with the test pad
# script, each SECS seconds; reports per run the last frame count, the frame
# rate in the last minute and whether presentation stopped (two fps windows
# without a frame). Extra environment is passed through.
#   tools/soak.sh [runs=3] [secs=330] [tag=soak]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N="${1:-3}"; SECS="${2:-330}"; TAG="${3:-soak}"
export KZ2_RUN_DIR="${KZ2_RUN_DIR:-${ROOT}/run_test}" PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-0}" KZ2_FPS=1
export PAD_SCRIPT="${PAD_SCRIPT:-20:0x4000,26:0x4000,32:0x4000,38:0x0008,44:0x4000,50:0x4000,56:0x0008,62:0x4000}"
for i in $(seq "${N}"); do
    LOG="${ROOT}/logs/${TAG}_${i}.log"
    "${ROOT}/tools/run.sh" > "${LOG}" 2>&1 &
    PID=$!
    sleep "${SECS}"
    GPID="$(pgrep -P "${PID}" -x killzone2 || echo "${PID}")"
    kill -9 "${GPID}" "${PID}" 2>/dev/null; wait 2>/dev/null
    last="$(grep -a '\[fps\]' "${LOG}" | tail -1 | sed -E 's/.*\(([0-9]+) total\).*/\1/')"
    rate="$(grep -a '\[fps\]' "${LOG}" | tail -12 | awk '{s+=$2; n++} END {if (n) printf "%.1f", s/n}')"
    stop="$(grep -a '\[fps\]' "${LOG}" | tail -6 | awk '$2 == "0.0" {z++} END {print (z >= 2) ? "STOPPED" : "ok"}')"
    echo "run ${i}: frames ${last:-0}, last-minute ${rate:-0} fps, ${stop}"
done
