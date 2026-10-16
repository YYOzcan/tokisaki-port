#!/usr/bin/env bash
# Tokisaki Port -- run the game under gdb for N seconds, then dump every
# thread's host backtrace (lifted guest functions show up as func_XXXXXXXX).
#   tools/snapshot.sh [seconds] > logs/snapshot.txt
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-20}"
mkdir -p "${ROOT}/run/vfs" "${ROOT}/run/hdd0" "${ROOT}/logs"
[ -e "${ROOT}/run/vfs/PS3_GAME" ] || ln -s "${ROOT}/game_dump/PS3_GAME" "${ROOT}/run/vfs/PS3_GAME"
export PS3_VFS_ROOT="${PS3_VFS_ROOT:-${ROOT}/run/vfs}" PS3_HDD0_ROOT="${ROOT}/run/hdd0"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-${HOME}/.config/rpcs3/dev_flash}"
export KZ2_NO_CRASH_HANDLER=1
( sleep "${SECS}"; pkill -INT -x killzone2; sleep 8; pkill -INT -x killzone2 ) &
gdb -q -batch -ex "set pagination off" -ex "handle SIGPIPE nostop noprint" \
    -ex run -ex "thread apply all bt 25" -ex "echo ===== SECOND SNAPSHOT =====\n" -ex continue -ex "thread apply all bt 25" -ex kill \
    --args "${ROOT}/build/killzone2" "${ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF" \
    > "${ROOT}/logs/snapshot_game.log" 2>&1
grep -E "^Thread|^#" "${ROOT}/logs/snapshot_game.log" | sed -E 's/ \(ctx=0x[0-9a-f]+\)//; s/ at \/home[^ ]*//'
