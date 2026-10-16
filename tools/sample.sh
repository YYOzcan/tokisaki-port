#!/usr/bin/env bash
# Tokisaki Port -- poor man's profiler: after WARMUP seconds, interrupt the
# game COUNT times, INTERVAL seconds apart, and record the guest main thread's
# backtrace each time (the thread running guest_main). Prints a histogram of
# the lifted guest functions on those stacks.
#   tools/sample.sh [warmup=20] [count=20] [interval=1]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WARM="${1:-20}"; COUNT="${2:-20}"; IVAL="${3:-1}"
mkdir -p "${ROOT}/run/vfs" "${ROOT}/run/hdd0" "${ROOT}/logs"
[ -e "${ROOT}/run/vfs/PS3_GAME" ] || ln -s "${ROOT}/game_dump/PS3_GAME" "${ROOT}/run/vfs/PS3_GAME"
export PS3_VFS_ROOT="${PS3_VFS_ROOT:-${ROOT}/run/vfs}" PS3_HDD0_ROOT="${ROOT}/run/hdd0"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-${HOME}/.config/rpcs3/dev_flash}"
export KZ2_NO_CRASH_HANDLER=1 KZ2_RSX="${KZ2_RSX:-null}" PS3RECOMP_VK_GUEST_PROGRAMS="${PS3RECOMP_VK_GUEST_PROGRAMS:-1}"
CMDS="$(mktemp)"
{
    echo "set pagination off"; echo "handle SIGPIPE nostop noprint"; echo "run"
    for _ in $(seq "${COUNT}"); do
        echo "echo ===SAMPLE===\\n"
        echo "thread apply all bt 150"
        echo "continue"
    done
    echo "kill"
} > "${CMDS}"
( sleep "${WARM}"; for _ in $(seq "${COUNT}"); do pkill -INT -x killzone2; sleep "${IVAL}"; done; sleep 2; pkill -INT -x killzone2 ) &
gdb -q -batch -x "${CMDS}" --args "${ROOT}/build/killzone2" "${ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF" \
    > "${ROOT}/logs/sample_game.log" 2>&1
rm -f "${CMDS}"
# keep only the guest main thread's frames (the stack that ends in guest_main)
awk '/^Thread /{if (blk ~ /guest_main/) print blk; blk=""} /^#/{blk=blk $0 "\n"} END{if (blk ~ /guest_main/) print blk}' \
    "${ROOT}/logs/sample_game.log" > "${ROOT}/logs/sample_main.txt"
grep -o 'func_[0-9A-F]\{8\}' "${ROOT}/logs/sample_main.txt" | sort | uniq -c | sort -rn | head -40
