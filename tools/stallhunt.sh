#!/usr/bin/env bash
# Tokisaki Port -- catch the occasional stop: N runs to gameplay with the test
# pad script AT ONCE (each in its own run_hunt<i>/ with run_test's saves, so
# they load the CPU and each other as a busy machine would), then, after SECS
# seconds, every run whose presentation stopped (two fps windows without a
# frame) gets every thread's host backtrace from gdb (lifted guest functions
# show as func_XXXXXXXX) before all are killed.
#   tools/stallhunt.sh [runs=3] [secs=240] [tag=hunt]
# Writes logs/<tag>_<i>.log, and logs/<tag>_<i>.bt for a stopped run.
# Extra environment is passed through to the game.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N="${1:-3}"; SECS="${2:-240}"; TAG="${3:-hunt}"
export PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-0}" KZ2_FPS=1 KZ2_ALLOW_PTRACE=1
export PS3RECOMP_VK_PRESENT_READBACK="${PS3RECOMP_VK_PRESENT_READBACK:-0}"
export PAD_SCRIPT="${PAD_SCRIPT:-20:0x4000,26:0x4000,32:0x4000,38:0x0008,44:0x4000,50:0x4000,56:0x0008,62:0x4000}"
declare -a PIDS
for i in $(seq "${N}"); do
    RUN="${ROOT}/run_hunt${i}"
    rm -rf "${RUN}"; mkdir -p "${RUN}"
    cp -a "${ROOT}/run_test/gamedata" "${RUN}/" 2>/dev/null || true
    KZ2_RUN_DIR="${RUN}" "${ROOT}/tools/run.sh" > "${ROOT}/logs/${TAG}_${i}.log" 2>&1 &
    PIDS[i]=$!
done
sleep "${SECS}"
for i in $(seq "${N}"); do
    LOG="${ROOT}/logs/${TAG}_${i}.log"
    GPID="$(pgrep -P "${PIDS[i]}" -x killzone2 || echo "${PIDS[i]}")"
    last="$(grep -a '\[fps\]' "${LOG}" | tail -1 | sed -E 's/.*\(([0-9]+) total\).*/\1/')"
    stop="$(grep -a '\[fps\]' "${LOG}" | tail -2 | awk '$2 == "0.0" {z++} END {print (z >= 2) ? "STOPPED" : "ok"}')"
    if [ "${stop}" = STOPPED ] && [ -d "/proc/${GPID}" ]; then
        gdb -q -batch -p "${GPID}" -ex "set pagination off" -ex "thread apply all bt 40" \
            > "${ROOT}/logs/${TAG}_${i}.bt" 2>&1
    fi
    echo "run ${i}: frames ${last:-0}, ${stop}"
done
for i in $(seq "${N}"); do
    GPID="$(pgrep -P "${PIDS[i]}" -x killzone2 || echo "${PIDS[i]}")"
    kill -9 "${GPID}" "${PIDS[i]}" 2>/dev/null
done
wait 2>/dev/null
