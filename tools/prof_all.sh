#!/usr/bin/env bash
# Tokisaki Port -- whole-process sampling profiler (no perf needed).
# Runs the game to the campaign with the test pad script, then takes COUNT
# eu-stack snapshots of every thread, INTERVAL seconds apart, starting at
# WARMUP seconds, and kills the game. tools/prof_report.py summarises them.
#   tools/prof_all.sh [warmup=240] [count=40] [interval=0.5] [out=logs/prof_all.txt]
# Extra environment (KZ2_RSX, SPURS_MAXCONT, ...) is passed through.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WARM="${1:-240}"; COUNT="${2:-40}"; IVAL="${3:-0.5}"; OUT="${4:-${ROOT}/logs/prof_all.txt}"
export KZ2_RUN_DIR="${KZ2_RUN_DIR:-${ROOT}/run_test}" PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-0}"
export KZ2_FPS="${KZ2_FPS:-1}" KZ2_ALLOW_PTRACE=1
export PAD_SCRIPT="${PAD_SCRIPT:-20:0x4000,26:0x4000,32:0x4000,38:0x0008,44:0x4000,50:0x4000,56:0x0008,62:0x4000}"
nice -n 10 "${ROOT}/tools/run.sh" > "${OUT%.txt}.log" 2>&1 &
PID=$!
sleep "${WARM}"
GPID="$(pgrep -P "${PID}" -x killzone2 || echo "${PID}")"
: > "${OUT}"
for _ in $(seq "${COUNT}"); do
    echo "%%SAMPLE $(date +%s.%N)" >> "${OUT}"
    eu-stack -p "${GPID}" >> "${OUT}" 2>/dev/null
    sleep "${IVAL}"
done
kill -9 "${GPID}" "${PID}" 2>/dev/null
wait 2>/dev/null
grep -a '\[fps\]' "${OUT%.txt}.log" | tail -4
