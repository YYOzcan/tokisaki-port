#!/usr/bin/env bash
# Tokisaki Port -- run the game to gameplay with the test pad script, then
# sample every thread with build/kzprof (tools/kzprof.c) and kill the game.
#   tools/prof_perf.sh [warmup=210] [seconds=20] [out=logs/kzprof.txt]
# Extra environment is passed through to the game.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WARM="${1:-210}"; SECS="${2:-20}"; OUT="${3:-${ROOT}/logs/kzprof.txt}"
[ "${ROOT}/build/kzprof" -nt "${ROOT}/tools/kzprof.c" ] || cc -O2 -o "${ROOT}/build/kzprof" "${ROOT}/tools/kzprof.c"
export KZ2_RUN_DIR="${KZ2_RUN_DIR:-${ROOT}/run_test}" PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-0}"
export KZ2_FPS="${KZ2_FPS:-1}" KZ2_ALLOW_PTRACE=1
# as in a window: no full-frame readback per present unless frames are dumped
export PS3RECOMP_VK_PRESENT_READBACK="${PS3RECOMP_VK_PRESENT_READBACK:-0}"
export PAD_SCRIPT="${PAD_SCRIPT:-20:0x4000,26:0x4000,32:0x4000,38:0x0008,44:0x4000,50:0x4000,56:0x0008,62:0x4000}"
"${ROOT}/tools/run.sh" > "${OUT%.txt}.log" 2>&1 &
PID=$!
sleep "${WARM}"
GPID="$(pgrep -P "${PID}" -x killzone2 || echo "${PID}")"
"${ROOT}/tools/gpu_busy.sh" "${SECS}" > "${OUT%.txt}.gpu" 2>&1 &
GBPID=$!
KZPROF_RAW="${OUT%.txt}.raw" "${ROOT}/build/kzprof" "${GPID}" "${SECS}" 1000 80 > "${OUT}"
wait "${GBPID}"
kill -9 "${GPID}" "${PID}" 2>/dev/null
# the symbols of this binary, so the raw samples can be read after a rebuild
nm --defined-only "${KZ2_BUILD_DIR:-${ROOT}/build}/killzone2" | grep ' [tTwW] ' | gzip -1 > "${OUT%.txt}.nm.gz"
wait 2>/dev/null
grep -a '\[fps\]' "${OUT%.txt}.log" | tail -4
cat "${OUT%.txt}.gpu"
