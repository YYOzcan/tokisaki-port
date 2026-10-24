#!/usr/bin/env bash
# Tokisaki Port -- one long run to gameplay with the test pad script: every
# 30 s the resident memory and the last frame rate, to catch leaks, slow
# drifts and stops over a play session.
#   tools/longrun.sh [secs=900] [log=logs/longrun.log]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-900}"; LOG="${2:-${ROOT}/logs/longrun.log}"
export KZ2_RUN_DIR="${KZ2_RUN_DIR:-${ROOT}/run_test}" PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-0}" KZ2_FPS=1
export PS3RECOMP_VK_PRESENT_READBACK="${PS3RECOMP_VK_PRESENT_READBACK:-0}"
export PAD_SCRIPT="${PAD_SCRIPT:-20:0x4000,26:0x4000,32:0x4000,38:0x0008,44:0x4000,50:0x4000,56:0x0008,62:0x4000}"
"${ROOT}/tools/run.sh" > "${LOG}" 2>&1 &
PID=$!
sleep 5
GPID="$(pgrep -P "${PID}" -x killzone2 || echo "${PID}")"
for t in $(seq 30 30 "${SECS}"); do
    sleep 30
    rss=$(awk '/VmRSS/ {printf "%d", $2/1024}' "/proc/${GPID}/status" 2>/dev/null)
    fps=$(grep -a '\[fps\]' "${LOG}" | tail -1 | awk '{print $2, $5}')
    echo "t=${t}s rss=${rss:-gone}MB fps/frames=${fps}"
    [ -z "${rss}" ] && break
done
kill -9 "${GPID}" "${PID}" 2>/dev/null; wait 2>/dev/null
