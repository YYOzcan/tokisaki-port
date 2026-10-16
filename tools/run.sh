#!/usr/bin/env bash
# Tokisaki Port -- run the recompiled Killzone 2.
#
# run/vfs/PS3_GAME links to the disc dump; /dev_hdd0 maps to run/hdd0 so the
# game's installs and saves never touch the dump itself.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${ROOT}/run/vfs" "${ROOT}/run/hdd0"
[ -e "${ROOT}/run/vfs/PS3_GAME" ] || ln -s "${ROOT}/game_dump/PS3_GAME" "${ROOT}/run/vfs/PS3_GAME"

if [ -z "${PS3_VFS_ROOT:-}" ]; then
    if [ -d "${ROOT}/run/vfs_nointro" ]; then
        export PS3_VFS_ROOT="${ROOT}/run/vfs_nointro"
    else
        export PS3_VFS_ROOT="${ROOT}/run/vfs"
    fi
fi
export PS3_HDD0_ROOT="${ROOT}/run/hdd0"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-${HOME}/.config/rpcs3/dev_flash}"
export PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-1}"
export PS3RECOMP_VK_GUEST_PROGRAMS="${PS3RECOMP_VK_GUEST_PROGRAMS:-1}"

cd "${ROOT}/run"   # cellSaveData writes ./gamedata/dev_hdd0/... relative to the cwd
exec "${ROOT}/build/killzone2" "${ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF" "$@"
