#!/usr/bin/env bash
# Usage: KZ2_RSX=null PAD_SCRIPT=... tools/gdb/run_gdb.sh tools/gdb/<script>.py
# Run the game under gdb with the same environment tools/run.sh sets up.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="${KZ2_RUN_DIR:-$ROOT/run_test}"
export PS3_VFS_ROOT="$ROOT/run/vfs_nointro" PS3_HDD0_ROOT="$RUN/hdd0"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-$HOME/.config/rpcs3/dev_flash}"
export PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-0}" PS3RECOMP_VK_GUEST_PROGRAMS=1
cd "$RUN"
exec gdb -q -batch -x "$1" --args "$ROOT/build/killzone2" "$ROOT/game_dump/PS3_GAME/USRDIR/EBOOT.ELF"
