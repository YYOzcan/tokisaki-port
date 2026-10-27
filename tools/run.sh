#!/usr/bin/env bash
# Tokisaki Port -- run the recompiled Killzone 2.
#
# run/vfs/PS3_GAME links to the disc dump; /dev_hdd0 maps to run/hdd0 so the
# game's installs and saves never touch the dump itself.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# KZ2_RUN_DIR: working directory (saves, hdd0) -- a second instance, e.g. a
# test run next to a play session, uses its own so their saves never collide.
RUN="${KZ2_RUN_DIR:-${ROOT}/run}"
mkdir -p "${ROOT}/run/vfs" "${RUN}/hdd0"
[ -e "${ROOT}/run/vfs/PS3_GAME" ] || ln -s "${ROOT}/game_dump/PS3_GAME" "${ROOT}/run/vfs/PS3_GAME"

if [ -z "${PS3_VFS_ROOT:-}" ]; then
    if [ -d "${ROOT}/run/vfs_nointro" ]; then
        export PS3_VFS_ROOT="${ROOT}/run/vfs_nointro"
    else
        export PS3_VFS_ROOT="${ROOT}/run/vfs"
    fi
fi
export PS3_HDD0_ROOT="${RUN}/hdd0"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-${HOME}/.config/rpcs3/dev_flash}"
export PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-1}"
export PS3RECOMP_VK_GUEST_PROGRAMS="${PS3RECOMP_VK_GUEST_PROGRAMS:-1}"

# KZ2_BUILD_DIR: run another build (default build/), e.g. to compare two.
BIN="$(cd "${KZ2_BUILD_DIR:-${ROOT}/build}" && pwd)/killzone2"
cd "${RUN}"   # cellSaveData writes ./gamedata/dev_hdd0/... relative to the cwd
exec "${BIN}" "${ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF" "$@"
